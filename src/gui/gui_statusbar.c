#include <gtk/gtk.h>
#include <adwaita.h>
#include <string.h>
#include <unistd.h>
#include "gui_internal.h"

/* Status-bar action buttons and the status â® overflow menu (open/new/save/
 * save-as/find-replace/copy-file/export-pdf/fullscreen/read-mode/settings/
 * shortcuts/restart/quit), plus the popdown helper and the menu-row builder.
 * Extracted from gui.c. Entry points declared in gui_internal.h. */

void on_status_menu_shortcuts(GtkButton *btn, gpointer user_data) {
    popdown_ancestor_popover(GTK_WIDGET(btn));
    show_keybindings_window((AppGui *)user_data);
}

void on_status_menu_settings(GtkButton *btn, gpointer user_data) {
    popdown_ancestor_popover(GTK_WIDGET(btn));
    on_settings_btn_clicked(NULL, (AppGui *)user_data);
}

void popdown_ancestor_popover(GtkWidget *w) {
    GtkWidget *pop = gtk_widget_get_ancestor(w, GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
}

void on_status_menu_quit(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    popdown_ancestor_popover(GTK_WIDGET(btn));
    gui_save_window_geometry(global_gui);
    g_application_quit(g_application_get_default());
}

void on_status_bar_open_file_clicked(GtkButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, qirtas_tr("Open Existing File"));
    gtk_file_dialog_open(dialog, GTK_WINDOW(gui->window), NULL, on_open_dialog_response, gui);
}

void on_status_bar_new_file_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));

    extern void zig_open_file(const char *filename);
    zig_open_file("Untitled");
}

void on_status_menu_find_replace(GtkButton *btn, gpointer user_data) {
    popdown_ancestor_popover(GTK_WIDGET(btn));
    AppGui *gui = (AppGui *)user_data;
    if (!gui->search_visible) toggle_search(gui);
}

void on_status_bar_save_file_clicked(GtkButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    gui_manual_save(gui);
}

void on_status_menu_fullscreen(GtkButton *btn, gpointer user_data) {
    popdown_ancestor_popover(GTK_WIDGET(btn));
    toggle_fullscreen((AppGui *)user_data);
}

/* Open the version history for the file in the active tab. (Also reachable by
 * right-clicking a file in the explorer; this surfaces it where users look.) */
void on_status_menu_history(GtkButton *btn, gpointer user_data) {
    popdown_ancestor_popover(GTK_WIDGET(btn));
    AppGui *gui = (AppGui *)user_data;
    if (!gui) return;
    if (gui->tabs.active >= 0 && gui->tabs.active < gui->tabs.count) {
        const char *path = gui->tabs.paths[gui->tabs.active];
        if (path && *path) show_file_history(gui, path);
    }
}

void on_restart_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    const char *appimage = g_getenv("APPIMAGE");
    char exe[1024] = {0};
    if (appimage) {
        strncpy(exe, appimage, sizeof(exe) - 1);
    } else {
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) exe[n] = '\0';
    }
    if (exe[0]) {
        gchar *argv[] = { exe, NULL };
        /* envp=NULL → child inherits the parent environment, so QIRTAS_DATA_DIR
         * and every other env var survive the restart unchanged. */
        g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL);
    }
    gui_save_window_geometry(global_gui);
    g_application_quit(g_application_get_default());
}

void on_status_menu_copy_file(GtkButton *btn, gpointer user_data) {
    popdown_ancestor_popover(GTK_WIDGET(btn));
    AppGui *gui = (AppGui *)user_data;
    if (!gui || gui->tabs.active == -1 || gui->tabs.active >= gui->tabs.count) return;
    const char *path = gui->tabs.paths[gui->tabs.active];
    if (!path || strcmp(path, "Untitled") == 0) return;
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) return;

    /* Put the file itself on the clipboard (text/uri-list) so pasting in
     * a file manager copies the .md file. */
    GFile *file = g_file_new_for_path(path);
    GdkFileList *flist = gdk_file_list_new_from_array(&file, 1);
    GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
    gdk_clipboard_set(cb, GDK_TYPE_FILE_LIST, flist);
    g_boxed_free(GDK_TYPE_FILE_LIST, flist);
    g_object_unref(file);
}

void on_status_bar_export_pdf_clicked(GtkButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));

    qirtas_export_to_pdf(gui);
}

GtkWidget *status_menu_item(const char *icon, const char *label,
                                   const char *hint,
                                   GCallback cb, gpointer user_data) {
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "pop-btn");
    gtk_widget_add_css_class(btn, "menu-item-btn");
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *img = gtk_image_new_from_icon_name(icon);
    GtkWidget *lbl = gtk_label_new(label);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_box_append(GTK_BOX(hbox), img);
    gtk_box_append(GTK_BOX(hbox), lbl);
    if (hint) {
        GtkWidget *hint_lbl = gtk_label_new(hint);
        gtk_widget_add_css_class(hint_lbl, "menu-item-hint");
        gtk_widget_set_halign(hint_lbl, GTK_ALIGN_END);
        gtk_box_append(GTK_BOX(hbox), hint_lbl);
    }
    gtk_button_set_child(GTK_BUTTON(btn), hbox);
    g_signal_connect(btn, "clicked", cb, user_data);
    return btn;
}

void on_status_menu_save_as(GtkButton *btn, gpointer user_data) {
    popdown_ancestor_popover(GTK_WIDGET(btn));
    trigger_save_as((AppGui *)user_data);
}

void on_read_mode_toggle_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    toggle_read_mode((AppGui *)user_data);
}
