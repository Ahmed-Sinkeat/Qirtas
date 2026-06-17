#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <string.h>
#include "gui_internal.h"

static void update_search_match_count(AppGui *gui) {
    if (!gui->search_ctx) return;
    gint count = gtk_source_search_context_get_occurrences_count(gui->search_ctx);
    char buf[48];
    if (count == -1 || count == 0)
        snprintf(buf, sizeof(buf), "%s", qirtas_tr("no matches"));
    else
        snprintf(buf, sizeof(buf), "%d match%s", count, count == 1 ? "" : "es");
    gtk_label_set_text(GTK_LABEL(gui->search_match_label), buf);
}

/* Scroll so the match sits clear of the bottom search bar and the
 * side overlays (minimap, status pill) instead of right at an edge. */
static void scroll_to_match(AppGui *gui, GtkTextBuffer *buf) {
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(gui->source_view),
                                 gtk_text_buffer_get_insert(buf),
                                 0.0, TRUE, 0.5, 0.3);
}

static void do_search_forward(AppGui *gui) {
    if (!gui->search_ctx) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
    GtkTextIter start, end, ms, me;
    gtk_text_buffer_get_selection_bounds(buf, &start, &end);
    gtk_text_iter_forward_char(&end);
    gboolean wrapped;
    if (gtk_source_search_context_forward(gui->search_ctx, &end, &ms, &me, &wrapped)) {
        gtk_text_buffer_select_range(buf, &ms, &me);
        scroll_to_match(gui, buf);
    }
    update_search_match_count(gui);
}

static void do_search_backward(AppGui *gui) {
    if (!gui->search_ctx) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
    GtkTextIter start, end, ms, me;
    gtk_text_buffer_get_selection_bounds(buf, &start, &end);
    gboolean wrapped;
    if (gtk_source_search_context_backward(gui->search_ctx, &start, &ms, &me, &wrapped)) {
        gtk_text_buffer_select_range(buf, &ms, &me);
        scroll_to_match(gui, buf);
    }
    update_search_match_count(gui);
}

/* Find the match nearest the cursor without skipping past it, so the
 * selection stays anchored on the same hit as the user keeps typing,
 * and jumps to it immediately instead of waiting for the next/prev press. */
static void do_search_from_cursor(AppGui *gui) {
    if (!gui->search_ctx) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
    GtkTextIter start, end, ms, me;
    gtk_text_buffer_get_selection_bounds(buf, &start, &end);
    gboolean wrapped;
    if (gtk_source_search_context_forward(gui->search_ctx, &start, &ms, &me, &wrapped)) {
        gtk_text_buffer_select_range(buf, &ms, &me);
        scroll_to_match(gui, buf);
    }
}

/* The regex-building algorithm now lives in src/markdown.zig
 * (zig_arabic_search_regex), shared with future platforms and unit-tested.
 * NFKC normalization stays here because it is platform-provided (GLib on Linux;
 * an Android port uses java.text.Normalizer before calling the same Zig fn).
 * NFKC decomposes Arabic presentation-form ligatures (from IME/pasted PDF) so a
 * search for a ligature-forming word still matches plain text. */
static char *create_arabic_search_regex(const char *raw_input) {
    if (!raw_input || strlen(raw_input) == 0) return g_strdup("");

    gchar *input_norm = g_utf8_normalize(raw_input, -1, G_NORMALIZE_NFKC);
    char *zr = zig_arabic_search_regex(input_norm ? input_norm : raw_input);
    g_free(input_norm);

    /* Copy into a GLib-owned string so the caller's g_free is correct, then
     * release the Zig allocation with the matching deallocator. */
    char *result = g_strdup(zr ? zr : "");
    if (zr) zig_free_document_text(zr);
    return result;
}

void toggle_search(AppGui *gui) {
    gui->search_visible = !gui->search_visible;
    gtk_revealer_set_reveal_child(GTK_REVEALER(gui->search_revealer),
                                  gui->search_visible);
    if (gui->search_visible) {
        if (gui->btn_search) gtk_widget_add_css_class(gui->btn_search, "active");
        gtk_stack_set_visible_child_name(GTK_STACK(gui->stack), "editor");
        if (gui->btn_editor) gtk_widget_add_css_class(gui->btn_editor, "active");
        if (gui->status_pill) gtk_widget_set_visible(gui->status_pill, TRUE);
        if (gui->btn_search) gtk_widget_set_visible(gui->btn_search, TRUE);
        if (gui->btn_status_actions) gtk_widget_set_visible(gui->btn_status_actions, TRUE);
        if (gui->path_label) gtk_widget_set_visible(gui->path_label, TRUE);
        gtk_widget_grab_focus(gui->search_entry);
    } else {
        if (gui->btn_search) gtk_widget_remove_css_class(gui->btn_search, "active");
        gtk_widget_grab_focus(gui->source_view);
    }
}

void on_search_text_changed(GtkSearchEntry *entry, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (!gui->search_settings) return;

    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    char *regex_text = create_arabic_search_regex(text);

    gtk_source_search_settings_set_search_text(gui->search_settings, regex_text);
    g_free(regex_text);

    if (text && *text)
        do_search_from_cursor(gui);

    update_search_match_count(gui);
}

/* GtkSourceSearchContext computes occurrences-count asynchronously, so the
 * initial value right after set_search_text() is often still -1. Refresh
 * the label once the background scan reports the real count. */
void on_search_occurrences_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
    (void)obj; (void)pspec;
    update_search_match_count((AppGui *)user_data);
}

void on_search_next_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    do_search_forward((AppGui *)user_data);
}

void on_search_prev_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    do_search_backward((AppGui *)user_data);
}

gboolean on_search_entry_key(GtkEventControllerKey *ctrl,
                             guint keyval, guint keycode,
                             GdkModifierType state, gpointer user_data) {
    (void)ctrl; (void)keycode; (void)state;
    AppGui *gui = (AppGui *)user_data;
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        do_search_forward(gui);
        return TRUE;
    }
    if (keyval == GDK_KEY_Escape) {
        toggle_search(gui);
        return TRUE;
    }
    return FALSE;
}

void on_search_icon_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    toggle_search((AppGui *)user_data);
}

void on_close_search_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppGui *gui = (AppGui *)user_data;
    if (gui->search_visible) toggle_search(gui);
}

void on_replace_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppGui *gui = (AppGui *)user_data;
    if (!gui->search_ctx || !gui->replace_entry) return;

    const char *replacement = gtk_editable_get_text(GTK_EDITABLE(gui->replace_entry));
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));

    GtkTextIter start, ms, me;
    gtk_text_buffer_get_selection_bounds(buf, &start, NULL);
    gboolean wrapped;
    if (!gtk_source_search_context_forward(gui->search_ctx, &start, &ms, &me, &wrapped))
        return;

    GError *error = NULL;
    if (gtk_source_search_context_replace(gui->search_ctx, &ms, &me,
                                          replacement, -1, &error)) {
        do_search_forward(gui);
    }
    if (error) g_error_free(error);
    update_search_match_count(gui);
}

void on_replace_all_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppGui *gui = (AppGui *)user_data;
    if (!gui->search_ctx || !gui->replace_entry) return;

    const char *replacement = gtk_editable_get_text(GTK_EDITABLE(gui->replace_entry));
    GError *error = NULL;
    gtk_source_search_context_replace_all(gui->search_ctx, replacement, -1, &error);
    if (error) g_error_free(error);
    update_search_match_count(gui);
}
