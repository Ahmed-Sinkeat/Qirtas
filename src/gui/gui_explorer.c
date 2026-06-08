#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <string.h>
#include <time.h>
#include "gui_internal.h"

/* Global tracker for the currently active tree row button */
static GtkWidget *g_active_tree_row = NULL;

/* Data passed to toggle callback for directory rows */
typedef struct {
    GtkWidget *children_box; /* The collapsible children container */
    GtkWidget *arrow_label;  /* "▶" / "▼" label */
    gboolean   expanded;
} TreeDirData;

static void on_tree_dir_toggle(GtkButton *btn, gpointer user_data) {
    (void)btn;
    TreeDirData *d = (TreeDirData *)user_data;
    d->expanded = !d->expanded;
    gtk_widget_set_visible(d->children_box, d->expanded);
    gtk_label_set_text(GTK_LABEL(d->arrow_label), d->expanded ? "▼" : "▶");
}

static void on_tree_file_clicked(GtkButton *btn, gpointer user_data) {
    /* Clear previous active */
    if (g_active_tree_row && GTK_IS_WIDGET(g_active_tree_row))
        gtk_widget_remove_css_class(g_active_tree_row, "active");
    g_active_tree_row = GTK_WIDGET(btn);
    gtk_widget_add_css_class(GTK_WIDGET(btn), "active");
    extern void zig_open_file(const char *filename);
    zig_open_file((const char *)user_data);
}

/* Forward declaration */
static void tree_build_dir(GtkWidget *parent_box, const char *dir_path, int depth);

/*
 * Build a single file row widget.
 * full_path  – absolute or relative path to the file
 * name       – display name (basename)
 */
static GtkWidget *tree_build_file_row(const char *full_path, const char *name) {
    gboolean is_md = g_str_has_suffix(name, ".md") || g_str_has_suffix(name, ".txt");

    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "tree-row");
    gtk_widget_set_hexpand(btn, TRUE);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_halign(hbox, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(hbox, TRUE);

    /* File icon */
    const char *icon_name = is_md ? "text-x-generic-symbolic" : "text-x-generic-symbolic";
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_widget_add_css_class(icon, is_md ? "tree-icon-md" : "tree-icon");
    gtk_box_append(GTK_BOX(hbox), icon);

    /* Name label */
    GtkWidget *lbl = gtk_label_new(name);
    gtk_widget_add_css_class(lbl, "tree-row-label");
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(hbox), lbl);

    gtk_button_set_child(GTK_BUTTON(btn), hbox);

    /* Store full path on button and connect signal */
    char *path_copy = g_strdup(full_path);
    g_signal_connect_data(btn, "clicked",
                          G_CALLBACK(on_tree_file_clicked),
                          path_copy, (GClosureNotify)g_free, 0);

    /* Store filepath for search */
    g_object_set_data_full(G_OBJECT(btn), "tree_filepath", g_strdup(full_path), g_free);

    return btn;
}

/*
 * Build a directory row with a collapsible children box.
 * Returns the outer wrapper box (row + children).
 */
static GtkWidget *tree_build_dir_row(const char *dir_path, const char *name) {
    GtkWidget *wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Header button row */
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "tree-row");
    gtk_widget_add_css_class(btn, "tree-row-dir");
    gtk_widget_set_hexpand(btn, TRUE);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_hexpand(hbox, TRUE);

    /* Arrow label */
    GtkWidget *arrow = gtk_label_new("▶");
    gtk_widget_add_css_class(arrow, "tree-arrow");
    gtk_box_append(GTK_BOX(hbox), arrow);

    /* Folder icon */
    GtkWidget *icon = gtk_image_new_from_icon_name("folder-symbolic");
    gtk_widget_add_css_class(icon, "tree-icon-folder");
    gtk_box_append(GTK_BOX(hbox), icon);

    /* Name label */
    GtkWidget *lbl = gtk_label_new(name);
    gtk_widget_add_css_class(lbl, "tree-row-label");
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(hbox), lbl);

    gtk_button_set_child(GTK_BUTTON(btn), hbox);
    gtk_box_append(GTK_BOX(wrapper), btn);

    /* Children container (hidden by default) */
    GtkWidget *children_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(children_box, "tree-children");
    gtk_widget_set_visible(children_box, FALSE);
    gtk_box_append(GTK_BOX(wrapper), children_box);

    /* Recursively populate children */
    tree_build_dir(children_box, dir_path, 0);

    /* Toggle data */
    TreeDirData *data = g_new0(TreeDirData, 1);
    data->children_box = children_box;
    data->arrow_label  = arrow;
    data->expanded     = FALSE;

    g_signal_connect_data(btn, "clicked",
                          G_CALLBACK(on_tree_dir_toggle),
                          data, (GClosureNotify)g_free, 0);

    return wrapper;
}

/*
 * String comparison function for qsort — directories first, then alphabetical.
 */
typedef struct { char name[NAME_MAX+1]; gboolean is_dir; } DirEntry;

static int dir_entry_cmp(const void *a, const void *b) {
    const DirEntry *ea = (const DirEntry *)a;
    const DirEntry *eb = (const DirEntry *)b;
    if (ea->is_dir != eb->is_dir)
        return ea->is_dir ? -1 : 1; /* dirs first */
    return strcasecmp(ea->name, eb->name);
}

/*
 * Recursively fill parent_box with tree rows for all entries in dir_path.
 * depth is unused currently but kept for future indent logic.
 */
static void tree_build_dir(GtkWidget *parent_box, const char *dir_path, int depth) {
    (void)depth;

    GError *err = NULL;
    GDir *dir = g_dir_open(dir_path, 0, &err);
    if (!dir) {
        if (err) g_error_free(err);
        return;
    }

    /* Collect entries */
    DirEntry entries[4096];
    int count = 0;
    const char *nm;
    while ((nm = g_dir_read_name(dir)) != NULL && count < 4095) {
        if (nm[0] == '.') continue; /* skip hidden */
        strncpy(entries[count].name, nm, NAME_MAX);
        entries[count].name[NAME_MAX] = '\0';

        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir_path, nm);
        entries[count].is_dir = g_file_test(full, G_FILE_TEST_IS_DIR);
        count++;
    }
    g_dir_close(dir);

    qsort(entries, count, sizeof(DirEntry), dir_entry_cmp);

    for (int i = 0; i < count; i++) {
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir_path, entries[i].name);

        GtkWidget *row_widget;
        if (entries[i].is_dir) {
            row_widget = tree_build_dir_row(full, entries[i].name);
        } else {
            /* Only show relevant text files */
            const char *n = entries[i].name;
            gboolean show = g_str_has_suffix(n, ".md")  ||
                            g_str_has_suffix(n, ".txt") ||
                            g_str_has_suffix(n, ".zig") ||
                            g_str_has_suffix(n, ".c")   ||
                            g_str_has_suffix(n, ".h")   ||
                            g_str_has_suffix(n, ".zon");
            if (!show) continue;
            row_widget = tree_build_file_row(full, entries[i].name);
        }
        gtk_box_append(GTK_BOX(parent_box), row_widget);
    }
}

void populate_explorer(AppGui *gui) {
    if (!gui || !gui->explorer_listbox) return;

    /* The "explorer_listbox" field now points to our tree root GtkBox.
       Clear all children first. */
    GtkWidget *c = gtk_widget_get_first_child(gui->explorer_listbox);
    while (c) {
        GtkWidget *nxt = gtk_widget_get_next_sibling(c);
        gtk_box_remove(GTK_BOX(gui->explorer_listbox), c);
        c = nxt;
    }

    g_active_tree_row = NULL;

    /* Build tree from current working directory */
    tree_build_dir(gui->explorer_listbox, ".", 0);

    /* Update badge count (count top-level children) */
    if (gui->exp_count_label) {
        int count = 0;
        GtkWidget *child = gtk_widget_get_first_child(gui->explorer_listbox);
        while (child) { count++; child = gtk_widget_get_next_sibling(child); }
        char badge[32];
        snprintf(badge, sizeof(badge), "%d items", count);
        gtk_label_set_text(GTK_LABEL(gui->exp_count_label), badge);
    }
}

/* Recursively walk the tree GtkBox and show/hide file rows matching query */
static int tree_filter_walk(GtkWidget *box, const char *query, int *match_count) {
    GtkWidget *child = gtk_widget_get_first_child(box);
    int visible_in_this_box = 0;
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        if (GTK_IS_BUTTON(child)) {
            /* File row */
            const char *fp = g_object_get_data(G_OBJECT(child), "tree_filepath");
            if (fp) {
                const char *basename = g_path_get_basename(fp); /* leaks, but tiny */
                gboolean match = (query == NULL || strlen(query) == 0 ||
                                  (strcasestr(basename, query) != NULL));
                gtk_widget_set_visible(child, match);
                if (match) { visible_in_this_box++; if (match_count) (*match_count)++; }
            }
        } else if (GTK_IS_BOX(child)) {
            /* Could be a dir wrapper or children_box */
            /* Check if it has the tree-children class → recurse into it */
            if (gtk_widget_has_css_class(child, "tree-children")) {
                int sub = tree_filter_walk(child, query, match_count);
                /* Make children_box visible if it has matches and we are searching */
                if (query && strlen(query) > 0) {
                    gtk_widget_set_visible(child, sub > 0);
                    if (sub > 0) visible_in_this_box++;
                } else {
                    /* Reset to collapsed */
                    gtk_widget_set_visible(child, FALSE);
                }
            } else {
                /* Dir wrapper box (contains button + children_box) */
                int sub = tree_filter_walk(child, query, match_count);
                gtk_widget_set_visible(child, (query == NULL || strlen(query) == 0) || sub > 0);
                if (sub > 0) visible_in_this_box++;
            }
        }
        child = next;
    }
    return visible_in_this_box;
}

static guint explorer_search_timeout_id = 0;

static gboolean do_debounced_explorer_search(gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    explorer_search_timeout_id = 0;

    if (!gui || !gui->explorer_listbox) return G_SOURCE_REMOVE;

    const char *query = gtk_editable_get_text(GTK_EDITABLE(gui->exp_search_entry));
    int match_count = 0;
    tree_filter_walk(gui->explorer_listbox, query, &match_count);

    if (gui->exp_count_label) {
        char badge[64];
        if (query && strlen(query) > 0)
            snprintf(badge, sizeof(badge), "Found %d matches", match_count);
        else {
            /* Count top-level items */
            int top = 0;
            GtkWidget *w = gtk_widget_get_first_child(gui->explorer_listbox);
            while (w) { top++; w = gtk_widget_get_next_sibling(w); }
            snprintf(badge, sizeof(badge), "%d items", top);
        }
        gtk_label_set_text(GTK_LABEL(gui->exp_count_label), badge);
    }

    return G_SOURCE_REMOVE;
}

void on_explorer_search_changed(GtkSearchEntry *entry, gpointer user_data) {
    (void)entry;
    if (explorer_search_timeout_id > 0)
        g_source_remove(explorer_search_timeout_id);
    explorer_search_timeout_id = g_timeout_add(150, do_debounced_explorer_search, user_data);
}

void on_file_card_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    extern void zig_open_file(const char *filename);
    zig_open_file((const char *)user_data);
}

int explorer_sort_func(GtkListBoxRow *row1, GtkListBoxRow *row2, gpointer user_data) {
    (void)row1; (void)row2; (void)user_data;
    return 0;
}
