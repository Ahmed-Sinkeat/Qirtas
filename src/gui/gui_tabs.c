#include <gtk/gtk.h>
#include <glib.h>
#include <math.h>
#include <string.h>
#include "gui_internal.h"

#define TAB_SCROLL_STEP 120.0

static void on_tab_button_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    const char *path = (const char *)user_data;
    extern void zig_open_file(const char *filename);
    zig_open_file(path);
}

static void on_tab_close_clicked(GtkButton *btn, gpointer user_data);

static GtkWidget *tab_item_at_index(AppGui *gui, int index) {
    if (!gui || !gui->tab_bar_box || index < 0) return NULL;

    GtkWidget *child = gtk_widget_get_first_child(gui->tab_bar_box);
    for (int i = 0; i < index && child; i++) {
        child = gtk_widget_get_next_sibling(child);
    }
    return child;
}

static void gui_tabs_scroll_active_into_view(AppGui *gui) {
    if (!gui || !gui->tab_bar_scroll || !gui->tab_bar_box || gui->active_tab_index < 0) {
        return;
    }

    GtkWidget *tab_item = tab_item_at_index(gui, gui->active_tab_index);
    if (!tab_item) return;

    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(
        GTK_SCROLLED_WINDOW(gui->tab_bar_scroll));
    if (!adj) return;

    graphene_rect_t bounds;
    if (!gtk_widget_compute_bounds(tab_item, gui->tab_bar_box, &bounds)) {
        return;
    }

    double page = gtk_adjustment_get_page_size(adj);
    double value = gtk_adjustment_get_value(adj);
    double upper = gtk_adjustment_get_upper(adj);
    double lower = gtk_adjustment_get_lower(adj);
    double tab_left = bounds.origin.x;
    double tab_right = tab_left + bounds.size.width;
    double max_val = MAX(lower, upper - page);

    if (tab_left < value) {
        gtk_adjustment_set_value(adj, CLAMP(tab_left, lower, max_val));
    } else if (tab_right > value + page) {
        gtk_adjustment_set_value(adj, CLAMP(tab_right - page, lower, max_val));
    }
}

typedef struct {
    AppGui *gui;
    int retries;
} TabScrollData;

static gboolean scroll_active_tab_idle(gpointer user_data) {
    TabScrollData *data = (TabScrollData *)user_data;
    AppGui *gui = data->gui;

    if (!gui || gui->active_tab_index < 0 || data->retries > 50) {
        g_free(data);
        return G_SOURCE_REMOVE;
    }

    GtkWidget *tab_item = tab_item_at_index(gui, gui->active_tab_index);
    if (!tab_item) {
        g_free(data);
        return G_SOURCE_REMOVE;
    }

    graphene_rect_t bounds;
    if (!gtk_widget_compute_bounds(tab_item, gui->tab_bar_box, &bounds) || bounds.size.width <= 0.0) {
        data->retries++;
        return G_SOURCE_CONTINUE;
    }

    gui_tabs_scroll_active_into_view(gui);
    g_free(data);
    return G_SOURCE_REMOVE;
}

static void gui_tabs_queue_scroll_active(AppGui *gui) {
    if (!gui || gui->active_tab_index < 0) return;
    TabScrollData *data = g_new0(TabScrollData, 1);
    data->gui = gui;
    data->retries = 0;
    g_idle_add(scroll_active_tab_idle, data);
}

static void gui_tabs_scroll_by(AppGui *gui, double delta) {
    if (!gui || !gui->tab_bar_scroll) return;

    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(
        GTK_SCROLLED_WINDOW(gui->tab_bar_scroll));
    if (!adj) return;

    double page = gtk_adjustment_get_page_size(adj);
    double upper = gtk_adjustment_get_upper(adj);
    double lower = gtk_adjustment_get_lower(adj);
    double max_val = MAX(lower, upper - page);
    double next = gtk_adjustment_get_value(adj) + delta;

    gtk_adjustment_set_value(adj, CLAMP(next, lower, max_val));
}

static void on_tab_scroll_left_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    gui_tabs_scroll_by((AppGui *)user_data, -TAB_SCROLL_STEP);
}

static void on_tab_scroll_right_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    gui_tabs_scroll_by((AppGui *)user_data, TAB_SCROLL_STEP);
}

static gboolean on_tab_viewport_scroll_event(GtkEventController *controller,
                                             GdkEvent *event,
                                             gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    (void)controller;

    if (!gui || gdk_event_get_event_type(event) != GDK_SCROLL) {
        return FALSE;
    }

    double delta = 0.0;

    switch (gdk_scroll_event_get_direction(event)) {
    case GDK_SCROLL_UP:
        delta = -TAB_SCROLL_STEP;
        break;
    case GDK_SCROLL_DOWN:
        delta = TAB_SCROLL_STEP;
        break;
    case GDK_SCROLL_LEFT:
        delta = TAB_SCROLL_STEP;
        break;
    case GDK_SCROLL_RIGHT:
        delta = -TAB_SCROLL_STEP;
        break;
    case GDK_SCROLL_SMOOTH: {
        double dx = 0.0;
        double dy = 0.0;
        gdk_scroll_event_get_deltas(event, &dx, &dy);
        if (fabs(dx) > 0.0) {
            gui_tabs_scroll_by(gui, -dx * TAB_SCROLL_STEP);
            return TRUE;
        }
        if (fabs(dy) > 0.0) {
            gui_tabs_scroll_by(gui, dy * TAB_SCROLL_STEP);
            return TRUE;
        }
        return FALSE;
    }
    default:
        return FALSE;
    }

    gui_tabs_scroll_by(gui, delta);
    return TRUE;
}

void gui_tabs_setup_viewport(AppGui *gui) {
    if (!gui || !gui->tab_bar_scroll) return;

    if (gui->btn_tab_scroll_left) {
        g_signal_connect(gui->btn_tab_scroll_left, "clicked",
                         G_CALLBACK(on_tab_scroll_left_clicked), gui);
    }
    if (gui->btn_tab_scroll_right) {
        g_signal_connect(gui->btn_tab_scroll_right, "clicked",
                         G_CALLBACK(on_tab_scroll_right_clicked), gui);
    }

    GtkEventController *scroll_ctrl = gtk_event_controller_legacy_new();
    g_signal_connect(scroll_ctrl, "event", G_CALLBACK(on_tab_viewport_scroll_event), gui);
    gtk_widget_add_controller(gui->tab_bar_scroll, scroll_ctrl);
}

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
        gtk_accessible_update_property(GTK_ACCESSIBLE(close_btn),
                                       GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       "Close tab", -1);
        g_signal_connect(close_btn, "clicked", G_CALLBACK(on_tab_close_clicked), gui->open_tabs[i]);
        gtk_box_append(GTK_BOX(tab_item), close_btn);

        gtk_box_append(GTK_BOX(gui->tab_bar_box), tab_item);
    }

    gui_tabs_queue_scroll_active(gui);
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
