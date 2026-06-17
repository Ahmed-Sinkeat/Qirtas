#include <gtk/gtk.h>
#include <string.h>
#include "gui_internal.h"

/* ── Quick Switcher (Ctrl+P) ──
 * Fuzzy filename popup. Matching runs through the same Arabic
 * normalization as workspace search, so ملاحظه finds الملاحظة.md. */

extern void zig_open_file(const char *filename);

#define SWITCHER_MAX_FILES 2000
#define SWITCHER_MAX_ROWS  50
#define SWITCHER_MAX_DEPTH 3

typedef struct {
    GtkWidget *window;
    GtkWidget *entry;
    GtkWidget *listbox;
    GPtrArray *files; /* owned char* relative paths */
} SwitcherData;

static void switcher_scan_dir(GPtrArray *files, const char *dir, const char *prefix, int depth) {
    if (depth > SWITCHER_MAX_DEPTH || files->len >= SWITCHER_MAX_FILES) return;
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *nm;
    while ((nm = g_dir_read_name(d)) != NULL && files->len < SWITCHER_MAX_FILES) {
        if (nm[0] == '.') continue;
        gchar *full = g_build_filename(dir, nm, NULL);
        gchar *rel = prefix[0] ? g_strdup_printf("%s/%s", prefix, nm) : g_strdup(nm);
        if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
            switcher_scan_dir(files, full, rel, depth + 1);
            g_free(rel);
        } else if (g_str_has_suffix(nm, ".md") || g_str_has_suffix(nm, ".txt")) {
            g_ptr_array_add(files, rel); /* transfer */
        } else {
            g_free(rel);
        }
        g_free(full);
    }
    g_dir_close(d);
}

/* Subsequence fuzzy score on normalized lowercase text.
 * -1 = no match. Bonuses: consecutive runs, match at a word start. */
/* fuzzy_score moved to src/markdown.zig (zig_fuzzy_score) — shared, testable. */

static void switcher_open_path(SwitcherData *sd, const char *path) {
    gchar *dup = g_strdup(path);
    gtk_window_destroy(GTK_WINDOW(sd->window));
    zig_open_file(dup);
    g_free(dup);
    if (global_gui && global_gui->source_view)
        gtk_widget_grab_focus(global_gui->source_view);
}

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box;
    SwitcherData *sd = (SwitcherData *)user_data;
    const char *path = g_object_get_data(G_OBJECT(row), "path");
    if (path) switcher_open_path(sd, path);
}

static void switcher_refilter(SwitcherData *sd) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(sd->listbox)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(sd->listbox), child);

    const char *query = gtk_editable_get_text(GTK_EDITABLE(sd->entry));
    char *norm_q = zig_normalize_arabic(query);
    const char *q = norm_q ? norm_q : query;

    typedef struct { const char *path; int score; } Scored;
    GArray *scored = g_array_new(FALSE, FALSE, sizeof(Scored));

    for (guint i = 0; i < sd->files->len; i++) {
        const char *path = g_ptr_array_index(sd->files, i);
        char *norm_p = zig_normalize_arabic(path);
        int sc = zig_fuzzy_score(norm_p ? norm_p : path, q);
        if (norm_p) zig_free_normalized(norm_p);
        if (sc >= 0) {
            Scored e = { path, sc };
            g_array_append_val(scored, e);
        }
    }
    if (norm_q) zig_free_normalized(norm_q);

    /* simple selection sort of top rows — N small */
    guint shown = 0;
    while (shown < SWITCHER_MAX_ROWS && scored->len > 0) {
        guint best = 0;
        for (guint i = 1; i < scored->len; i++) {
            if (g_array_index(scored, Scored, i).score > g_array_index(scored, Scored, best).score)
                best = i;
        }
        Scored e = g_array_index(scored, Scored, best);
        g_array_remove_index_fast(scored, best);

        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *lbl = gtk_label_new(e.path);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_widget_set_margin_start(lbl, 10);
        gtk_widget_set_margin_end(lbl, 10);
        gtk_widget_set_margin_top(lbl, 6);
        gtk_widget_set_margin_bottom(lbl, 6);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
        gtk_widget_add_css_class(row, "tree-row");
        g_object_set_data_full(G_OBJECT(row), "path", g_strdup(e.path), g_free);
        gtk_list_box_append(GTK_LIST_BOX(sd->listbox), row);
        shown++;
    }
    g_array_free(scored, TRUE);

    GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(sd->listbox), 0);
    if (first) gtk_list_box_select_row(GTK_LIST_BOX(sd->listbox), first);
}

static void on_entry_changed(GtkEditable *editable, gpointer user_data) {
    (void)editable;
    switcher_refilter((SwitcherData *)user_data);
}

static void on_entry_activate(GtkEntry *entry, gpointer user_data) {
    (void)entry;
    SwitcherData *sd = (SwitcherData *)user_data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(sd->listbox));
    if (!row) row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(sd->listbox), 0);
    if (row) {
        const char *path = g_object_get_data(G_OBJECT(row), "path");
        if (path) switcher_open_path(sd, path);
    }
}

static gboolean on_switcher_key(GtkEventControllerKey *ctrl, guint keyval,
                                guint keycode, GdkModifierType state, gpointer user_data) {
    (void)ctrl; (void)keycode; (void)state;
    SwitcherData *sd = (SwitcherData *)user_data;
    if (keyval == GDK_KEY_Escape) {
        gtk_window_destroy(GTK_WINDOW(sd->window));
        return TRUE;
    }
    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_Up) {
        GtkListBoxRow *cur = gtk_list_box_get_selected_row(GTK_LIST_BOX(sd->listbox));
        int idx = cur ? gtk_list_box_row_get_index(cur) : -1;
        idx += (keyval == GDK_KEY_Down) ? 1 : -1;
        if (idx < 0) idx = 0;
        GtkListBoxRow *next = gtk_list_box_get_row_at_index(GTK_LIST_BOX(sd->listbox), idx);
        if (next) gtk_list_box_select_row(GTK_LIST_BOX(sd->listbox), next);
        return TRUE;
    }
    return FALSE;
}

static void on_switcher_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    SwitcherData *sd = (SwitcherData *)user_data;
    g_ptr_array_free(sd->files, TRUE);
    g_free(sd);
}

void show_quick_switcher(AppGui *gui) {
    SwitcherData *sd = g_new0(SwitcherData, 1);
    sd->files = g_ptr_array_new_with_free_func(g_free);
    switcher_scan_dir(sd->files, ".", "", 0);

    GtkWidget *win = gtk_window_new();
    sd->window = win;
    gtk_window_set_title(GTK_WINDOW(win), qirtas_tr("Quick Open"));
    gtk_window_set_default_size(GTK_WINDOW(win), 480, 380);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(gui->window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_widget_add_css_class(win, "settings-sheet-window");

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 10);
    gtk_widget_set_margin_end(vbox, 10);
    gtk_widget_set_margin_top(vbox, 10);
    gtk_widget_set_margin_bottom(vbox, 10);
    gtk_window_set_child(GTK_WINDOW(win), vbox);

    GtkWidget *entry = gtk_search_entry_new();
    sd->entry = entry;
    gtk_widget_add_css_class(entry, "search-entry");
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(entry), qirtas_tr("Type to search files…"));
    gtk_box_append(GTK_BOX(vbox), entry);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(vbox), scroll);

    GtkWidget *listbox = gtk_list_box_new();
    sd->listbox = listbox;
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), listbox);

    g_signal_connect(entry, "changed", G_CALLBACK(on_entry_changed), sd);
    g_signal_connect(entry, "activate", G_CALLBACK(on_entry_activate), sd);
    g_signal_connect(listbox, "row-activated", G_CALLBACK(on_row_activated), sd);
    g_signal_connect(win, "destroy", G_CALLBACK(on_switcher_destroy), sd);

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_switcher_key), sd);
    gtk_widget_add_controller(win, keys);

    switcher_refilter(sd);
    gtk_window_present(GTK_WINDOW(win));
    gtk_widget_grab_focus(entry);
}
