#include <gtk/gtk.h>
#include <string.h>
#include "gui_internal.h"

static void on_tab_button_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    const char *path = (const char *)user_data;
    extern void zig_open_file(const char *filename);
    zig_open_file(path);
}

static void on_tab_close_clicked(GtkButton *btn, gpointer user_data);

void gui_tabs_close(AppGui *gui, int index) {
    if (!gui || index < 0 || index >= gui->num_tabs) return;

    g_free(gui->open_tabs[index]);
    g_free(gui->tab_contents[index]);

    for (int i = index; i < gui->num_tabs - 1; i++) {
        gui->open_tabs[i] = gui->open_tabs[i + 1];
        gui->tab_contents[i] = gui->tab_contents[i + 1];
        gui->tab_modified[i] = gui->tab_modified[i + 1];
    }
    gui->open_tabs[gui->num_tabs - 1] = NULL;
    gui->tab_contents[gui->num_tabs - 1] = NULL;
    gui->tab_modified[gui->num_tabs - 1] = FALSE;
    gui->num_tabs--;

    if (index == gui->active_tab_index) {
        if (gui->num_tabs > 0) {
            int new_active = index;
            if (new_active >= gui->num_tabs) {
                new_active = gui->num_tabs - 1;
            }
            gui->active_tab_index = new_active;
            extern void zig_open_file(const char *filename);
            zig_open_file(gui->open_tabs[new_active]);
        } else {
            gui->active_tab_index = -1;
            gui_set_text("", 0);
            gui_set_title("Qirtas");
        }
    } else if (index < gui->active_tab_index) {
        gui->active_tab_index--;
    }

    gui_tabs_refresh(gui);
}

static void on_tab_close_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    const char *path = (const char *)user_data;
    AppGui *gui = global_gui;
    if (gui) {
        for (int i = 0; i < gui->num_tabs; i++) {
            if (strcmp(gui->open_tabs[i], path) == 0) {
                gui_tabs_close(gui, i);
                break;
            }
        }
    }
}

void gui_tabs_refresh(AppGui *gui) {
    if (!gui || !gui->tab_bar_box) return;

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(gui->tab_bar_box))) {
        gtk_box_remove(GTK_BOX(gui->tab_bar_box), child);
    }

    if (gui->source_view) {
        gtk_text_view_set_editable(GTK_TEXT_VIEW(gui->source_view), (gui->active_tab_index != -1));
    }

    for (int i = 0; i < gui->num_tabs; i++) {
        GtkWidget *tab_item = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_add_css_class(tab_item, "tab-item");
        if (i == gui->active_tab_index) {
            gtk_widget_add_css_class(tab_item, "active-tab");
        }

        char *basename = g_path_get_basename(gui->open_tabs[i]);
        GtkWidget *tab_btn = gtk_button_new();
        gtk_widget_add_css_class(tab_btn, "tab-btn");
        
        GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *lbl = gtk_label_new(basename);
        g_free(basename);
        gtk_box_append(GTK_BOX(btn_box), lbl);
        
        gboolean is_modified = FALSE;
        if (i == gui->active_tab_index && gui->source_view) {
            GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
            if (buf) {
                is_modified = gtk_text_buffer_get_modified(buf);
                gui->tab_modified[i] = is_modified;
            } else {
                is_modified = gui->tab_modified[i];
            }
        } else {
            is_modified = gui->tab_modified[i];
        }
        
        if (is_modified) {
            GtkWidget *dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
            gtk_widget_set_halign(dot, GTK_ALIGN_CENTER);
            gtk_widget_add_css_class(dot, "tab-dirty-dot");
            gtk_box_append(GTK_BOX(btn_box), dot);
        }
        
        gtk_button_set_child(GTK_BUTTON(tab_btn), btn_box);
        g_signal_connect(tab_btn, "clicked", G_CALLBACK(on_tab_button_clicked), gui->open_tabs[i]);
        gtk_box_append(GTK_BOX(tab_item), tab_btn);

        GtkWidget *close_btn = gtk_button_new_with_label("×");
        gtk_widget_add_css_class(close_btn, "tab-close");
        g_signal_connect(close_btn, "clicked", G_CALLBACK(on_tab_close_clicked), gui->open_tabs[i]);
        gtk_box_append(GTK_BOX(tab_item), close_btn);

        gtk_box_append(GTK_BOX(gui->tab_bar_box), tab_item);
    }
}

void gui_tabs_add_or_select(AppGui *gui, const char *filepath) {
    if (!gui || !filepath || filepath[0] == '\0') return;
    if (strcmp(filepath, "Qirtas") == 0 || strcmp(filepath, "(No open file)") == 0) return;

    int existing_index = -1;
    for (int i = 0; i < gui->num_tabs; i++) {
        if (strcmp(gui->open_tabs[i], filepath) == 0) {
            existing_index = i;
            break;
        }
    }

    if (existing_index != -1) {
        gui->active_tab_index = existing_index;
    } else {
        if (gui->num_tabs < 20) {
            gui->open_tabs[gui->num_tabs] = g_strdup(filepath);
            gui->tab_contents[gui->num_tabs] = NULL;
            gui->tab_modified[gui->num_tabs] = FALSE;
            gui->active_tab_index = gui->num_tabs;
            gui->num_tabs++;
        }
    }

    gui_tabs_refresh(gui);
}
