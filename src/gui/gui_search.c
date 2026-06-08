#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <string.h>
#include "gui_internal.h"

static void update_search_match_count(AppGui *gui) {
    if (!gui->search_ctx) return;
    gint count = gtk_source_search_context_get_occurrences_count(gui->search_ctx);
    char buf[48];
    if (count == -1 || count == 0)
        snprintf(buf, sizeof(buf), "no matches");
    else
        snprintf(buf, sizeof(buf), "%d match%s", count, count == 1 ? "" : "es");
    gtk_label_set_text(GTK_LABEL(gui->search_match_label), buf);
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
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(gui->source_view),
                                     gtk_text_buffer_get_insert(buf),
                                     0.15, FALSE, 0.0, 0.5);
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
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(gui->source_view),
                                     gtk_text_buffer_get_insert(buf),
                                     0.15, FALSE, 0.0, 0.5);
    }
    update_search_match_count(gui);
}

static char *create_arabic_search_regex(const char *input) {
    if (!input || strlen(input) == 0) return g_strdup("");

    GString *pattern = g_string_new("");
    const char *p = input;

    while (*p != '\0') {
        gunichar c = g_utf8_get_char(p);
        const char *next_p = g_utf8_next_char(p);

        if ((c >= 0x064B && c <= 0x065F) || c == 0x0670) {
            p = next_p;
            continue;
        }

        if (strchr(".*+?^${}()|[]\\", (char)c) != NULL) {
            g_string_append_printf(pattern, "\\%c", (char)c);
        } else if (c == 0x0627 || c == 0x0623 || c == 0x0625 || c == 0x0622 || c == 0x0671) {
            g_string_append(pattern, "[اأإآٱ]");
        } else if (c == 0x0629 || c == 0x0647) {
            g_string_append(pattern, "[ةه]");
        } else if (c == 0x064A || c == 0x0649) {
            g_string_append(pattern, "[يى]");
        } else {
            char utf8_buf[6] = {0};
            int len = g_unichar_to_utf8(c, utf8_buf);
            g_string_append_len(pattern, utf8_buf, len);
        }

        if (c >= 0x0600 && c <= 0x06FF) {
            g_string_append(pattern, "[\\x{064B}-\\x{065F}\\x{0670}]*");
        }

        p = next_p;
    }

    char *result = pattern->str;
    g_string_free(pattern, FALSE);
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

    update_search_match_count(gui);
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
