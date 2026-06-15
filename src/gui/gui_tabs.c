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

static void on_tab_open_in_fm_clicked(GtkButton *btn, gpointer user_data) {
    const char *path = (const char *)user_data;

    GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
    if (popover) gtk_popover_popdown(GTK_POPOVER(popover));

    GFile *file = g_file_new_for_path(path);
    GtkFileLauncher *launcher = gtk_file_launcher_new(file);
    GtkWindow *parent = global_gui ? GTK_WINDOW(global_gui->window) : NULL;
    gtk_file_launcher_open_containing_folder(launcher, parent, NULL, NULL, NULL);
    g_object_unref(launcher);
    g_object_unref(file);
}

/* Right-click context menu on a tab. CAPTURE phase: the tab button's own
 * click gesture would otherwise claim the sequence first. Currently just
 * "Open in File Manager" — more entries to follow. */
static void on_tab_right_click(GtkGestureClick *gesture, gint n_press,
                                gdouble x, gdouble y, gpointer user_data) {
    (void)n_press;
    GtkWidget *tab_btn = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    const char *path = (const char *)user_data;

    GtkWidget *popover = gtk_popover_new();
    gtk_widget_set_parent(popover, tab_btn);
    GdkRectangle rect = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    g_signal_connect(popover, "closed", G_CALLBACK(gtk_widget_unparent), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);

    GtkWidget *item = gtk_button_new_with_label(qirtas_tr("Open in File Manager"));
    gtk_widget_add_css_class(item, "pop-btn");
    gtk_widget_add_css_class(item, "menu-item-btn");
    gtk_widget_set_halign(item, GTK_ALIGN_FILL);
    g_signal_connect(item, "clicked", G_CALLBACK(on_tab_open_in_fm_clicked), (gpointer)path);
    gtk_box_append(GTK_BOX(box), item);

    gtk_popover_set_child(GTK_POPOVER(popover), box);
    gtk_popover_popup(GTK_POPOVER(popover));
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static GtkWidget *tab_item_at_index(AppGui *gui, int index) {
    if (!gui || !gui->tabs.bar_box || index < 0) return NULL;

    GtkWidget *child = gtk_widget_get_first_child(gui->tabs.bar_box);
    for (int i = 0; i < index && child; i++) {
        child = gtk_widget_get_next_sibling(child);
    }
    return child;
}

static void gui_tabs_scroll_active_into_view(AppGui *gui) {
    if (!gui || !gui->tabs.bar_scroll || !gui->tabs.bar_box || gui->tabs.active < 0) {
        return;
    }

    GtkWidget *tab_item = tab_item_at_index(gui, gui->tabs.active);
    if (!tab_item) return;

    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(
        GTK_SCROLLED_WINDOW(gui->tabs.bar_scroll));
    if (!adj) return;

    graphene_rect_t bounds;
    if (!gtk_widget_compute_bounds(tab_item, gui->tabs.bar_box, &bounds)) {
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

    if (!gui || gui->tabs.active < 0 || data->retries > 50) {
        g_free(data);
        return G_SOURCE_REMOVE;
    }

    GtkWidget *tab_item = tab_item_at_index(gui, gui->tabs.active);
    if (!tab_item) {
        g_free(data);
        return G_SOURCE_REMOVE;
    }

    graphene_rect_t bounds;
    if (!gtk_widget_compute_bounds(tab_item, gui->tabs.bar_box, &bounds) || bounds.size.width <= 0.0) {
        data->retries++;
        return G_SOURCE_CONTINUE;
    }

    gui_tabs_scroll_active_into_view(gui);
    g_free(data);
    return G_SOURCE_REMOVE;
}

static void gui_tabs_queue_scroll_active(AppGui *gui) {
    if (!gui || gui->tabs.active < 0) return;
    TabScrollData *data = g_new0(TabScrollData, 1);
    data->gui = gui;
    data->retries = 0;
    g_idle_add(scroll_active_tab_idle, data);
}

static void gui_tabs_scroll_by(AppGui *gui, double delta) {
    if (!gui || !gui->tabs.bar_scroll) return;

    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(
        GTK_SCROLLED_WINDOW(gui->tabs.bar_scroll));
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
    if (!gui || !gui->tabs.bar_scroll) return;

    if (gui->tabs.scroll_left_btn) {
        g_signal_connect(gui->tabs.scroll_left_btn, "clicked",
                         G_CALLBACK(on_tab_scroll_left_clicked), gui);
    }
    if (gui->tabs.scroll_right_btn) {
        g_signal_connect(gui->tabs.scroll_right_btn, "clicked",
                         G_CALLBACK(on_tab_scroll_right_clicked), gui);
    }

    GtkEventController *scroll_ctrl = gtk_event_controller_legacy_new();
    g_signal_connect(scroll_ctrl, "event", G_CALLBACK(on_tab_viewport_scroll_event), gui);
    gtk_widget_add_controller(gui->tabs.bar_scroll, scroll_ctrl);
}

void gui_tabs_close(AppGui *gui, int index) {
    if (!gui || index < 0 || index >= gui->tabs.count) return;

    g_free(gui->tabs.paths[index]);
    g_free(gui->tabs.contents[index]);

    for (int i = index; i < gui->tabs.count - 1; i++) {
        gui->tabs.paths[i] = gui->tabs.paths[i + 1];
        gui->tabs.contents[i] = gui->tabs.contents[i + 1];
        gui->tabs.modified[i] = gui->tabs.modified[i + 1];
    }
    gui->tabs.paths[gui->tabs.count - 1] = NULL;
    gui->tabs.contents[gui->tabs.count - 1] = NULL;
    gui->tabs.modified[gui->tabs.count - 1] = FALSE;
    gui->tabs.count--;

    if (index == gui->tabs.active) {
        if (gui->tabs.count > 0) {
            int new_active = index;
            if (new_active >= gui->tabs.count) {
                new_active = gui->tabs.count - 1;
            }
            gui->tabs.active = new_active;
            extern void zig_open_file(const char *filename);
            zig_open_file(gui->tabs.paths[new_active]);
        } else {
            gui->tabs.active = -1;
            gui_set_text("", 0);
            gui_set_title("Qirtas");
        }
    } else if (index < gui->tabs.active) {
        gui->tabs.active--;
    }

    gui_tabs_refresh(gui);
}

static void on_tab_close_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    const char *path = (const char *)user_data;
    AppGui *gui = global_gui;
    if (gui) {
        for (int i = 0; i < gui->tabs.count; i++) {
            if (strcmp(gui->tabs.paths[i], path) == 0) {
                gui_tabs_close(gui, i);
                break;
            }
        }
    }
}

void gui_tabs_refresh(AppGui *gui) {
    if (!gui || !gui->tabs.bar_box) return;

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(gui->tabs.bar_box))) {
        gtk_box_remove(GTK_BOX(gui->tabs.bar_box), child);
    }

    if (gui->source_view) {
        gtk_text_view_set_editable(GTK_TEXT_VIEW(gui->source_view), (gui->tabs.active != -1));
    }

    for (int i = 0; i < gui->tabs.count; i++) {
        GtkWidget *tab_item = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_add_css_class(tab_item, "tab-item");
        if (i == gui->tabs.active) {
            gtk_widget_add_css_class(tab_item, "active-tab");
        }

        char *basename = g_path_get_basename(gui->tabs.paths[i]);
        GtkWidget *tab_btn = gtk_button_new();
        gtk_widget_add_css_class(tab_btn, "tab-btn");

        GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *lbl = gtk_label_new(basename);
        g_free(basename);
        gtk_box_append(GTK_BOX(btn_box), lbl);

        gboolean is_modified = FALSE;
        if (i == gui->tabs.active && gui->source_view) {
            GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
            if (buf) {
                is_modified = gtk_text_buffer_get_modified(buf);
                gui->tabs.modified[i] = is_modified;
            } else {
                is_modified = gui->tabs.modified[i];
            }
        } else {
            is_modified = gui->tabs.modified[i];
        }

        if (is_modified) {
            GtkWidget *dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
            gtk_widget_set_halign(dot, GTK_ALIGN_CENTER);
            gtk_widget_add_css_class(dot, "tab-dirty-dot");
            gtk_box_append(GTK_BOX(btn_box), dot);
        }

        gtk_button_set_child(GTK_BUTTON(tab_btn), btn_box);
        g_signal_connect(tab_btn, "clicked", G_CALLBACK(on_tab_button_clicked), gui->tabs.paths[i]);

        GtkGesture *tab_rc = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(tab_rc), GDK_BUTTON_SECONDARY);
        gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(tab_rc), GTK_PHASE_CAPTURE);
        g_signal_connect(tab_rc, "pressed", G_CALLBACK(on_tab_right_click), gui->tabs.paths[i]);
        gtk_widget_add_controller(tab_btn, GTK_EVENT_CONTROLLER(tab_rc));

        gtk_box_append(GTK_BOX(tab_item), tab_btn);

        GtkWidget *close_btn = gtk_button_new_with_label("×");
        gtk_widget_add_css_class(close_btn, "tab-close");
        gtk_accessible_update_property(GTK_ACCESSIBLE(close_btn),
                                       GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       "Close tab", -1);
        g_signal_connect(close_btn, "clicked", G_CALLBACK(on_tab_close_clicked), gui->tabs.paths[i]);
        gtk_box_append(GTK_BOX(tab_item), close_btn);

        gtk_box_append(GTK_BOX(gui->tabs.bar_box), tab_item);
    }

    gui_tabs_queue_scroll_active(gui);
}

/* Drop every open tab. Called when switching vaults: the old tabs reference
 * files in the previous vault, and clicking one after the chdir resolved its
 * (now-relative) path inside the NEW vault, creating an empty file there —
 * data loss. Clear before the new vault loads so only the new vault's file
 * opens. */
void gui_tabs_close_all(AppGui *gui) {
    if (!gui) return;
    for (int i = 0; i < gui->tabs.count; i++) {
        g_free(gui->tabs.paths[i]);
        gui->tabs.paths[i] = NULL;
        g_free(gui->tabs.contents[i]);
        gui->tabs.contents[i] = NULL;
        gui->tabs.modified[i] = FALSE;
    }
    gui->tabs.count = 0;
    gui->tabs.active = -1;
    gui_tabs_refresh(gui);
}

void gui_tabs_add_or_select(AppGui *gui, const char *filepath) {
    if (!gui || !filepath || filepath[0] == '\0') return;
    if (strcmp(filepath, "Qirtas") == 0 || strcmp(filepath, "(No open file)") == 0) return;

    int existing_index = -1;
    for (int i = 0; i < gui->tabs.count; i++) {
        if (strcmp(gui->tabs.paths[i], filepath) == 0) {
            existing_index = i;
            break;
        }
    }

    if (existing_index != -1) {
        gui->tabs.active = existing_index;
    } else {
        if (gui->tabs.count < 20) {
            gui->tabs.paths[gui->tabs.count] = g_strdup(filepath);
            gui->tabs.contents[gui->tabs.count] = NULL;
            gui->tabs.modified[gui->tabs.count] = FALSE;
            gui->tabs.active = gui->tabs.count;
            gui->tabs.count++;
        }
    }

    gui_tabs_refresh(gui);
}

/* ── Tab content cache + full-buffer reload (full-buffer-editor-v2) ── */

void gui_tabs_save_active_to_cache(void) {
    AppGui *gui = global_gui;
    if (!gui) return;
    int idx = gui->tabs.active;
    if (idx != -1 && idx < gui->tabs.count) {
        if (strcmp(gui->tabs.paths[idx], "Untitled") == 0) {
            extern const char *zig_get_document_text(void);
            extern void zig_free_document_text(const char *ptr);
            const char *text = zig_get_document_text();
            g_free(gui->tabs.contents[idx]);
            gui->tabs.contents[idx] = text ? g_strdup(text) : NULL;
            zig_free_document_text(text);
        } else {
            extern int zig_save_document(void);
            zig_save_document();
        }
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
        gui->tabs.modified[idx] = gtk_text_buffer_get_modified(buf);
    }
}

void gui_tabs_restore_active_from_cache(void) {
    AppGui *gui = global_gui;
    if (!gui) return;
    int idx = gui->tabs.active;
    if (idx != -1 && idx < gui->tabs.count) {
        if (strcmp(gui->tabs.paths[idx], "Untitled") == 0 && gui->tabs.contents[idx] != NULL) {
            gui_set_text(gui->tabs.contents[idx], -1);
            GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
            gtk_text_buffer_set_modified(buf, gui->tabs.modified[idx]);
        }
    }
}

/* Reset GTK's internal preferred-x (column memory for vertical caret moves)
 * by re-placing the cursor at its current position. */
static void gui_reset_preferred_x(AppGui *gui) {
    if (!gui || !gui->source_view) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
    GtkTextIter iter;
    GtkTextMark *insert = gtk_text_buffer_get_insert(buf);
    gtk_text_buffer_get_iter_at_mark(buf, &iter, insert);
    gtk_text_buffer_place_cursor(buf, &iter);
}

/* Re-decoration + scroll restore deferred off gui_reload_full_buffer so the new
 * text + cursor paint instantly; the O(document) passes (HR widgets, RTL
 * paragraph direction, wiki links, outline) and the scroll restore run on the
 * next idle, once GTK has re-laid-out. This is what stops undo / formatting
 * from feeling like a full file reload. */
static gboolean reload_finalize_idle(gpointer data) {
    int top_line = *(int *)data;
    g_free(data);
    if (!global_gui || !global_gui->source_view) return G_SOURCE_REMOVE;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_gui->source_view));
    parse_and_render_hrs(buf, global_gui);
    update_all_paragraphs_direction(buf);
    apply_wiki_link_tags(buf);
    gui_outline_refresh(global_gui);
    /* Restore the viewport by LINE, not pixel offset: conceal/heading reflow
     * changes line heights, so the old pixel scroll value lands on the wrong
     * line and the page appears to jump (e.g. on Ctrl+Z). Scrolling the saved
     * top line back to the top is reflow-stable. */
    GtkTextView *tv = GTK_TEXT_VIEW(global_gui->source_view);
    int total = gtk_text_buffer_get_line_count(buf);
    if (top_line < 0) top_line = 0;
    if (top_line > total - 1) top_line = total - 1;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line(buf, &it, top_line);
    GtkTextMark *m = gtk_text_buffer_create_mark(buf, NULL, &it, TRUE);
    gtk_text_view_scroll_to_mark(tv, m, 0.0, TRUE, 0.0, 0.0);
    gtk_text_buffer_delete_mark(buf, m);
    return G_SOURCE_REMOVE;
}

void gui_reload_full_buffer(void) {
    if (!global_gui || !global_gui->source_view) return;

    int line = 1, col = 0;
    gui_get_cursor_position(&line, &col);

    /* Capture the buffer line currently at the top of the viewport, so we can
     * restore the view by line (reflow-stable) rather than by pixel offset. */
    int top_line = 0;
    {
        GtkTextView *tv = GTK_TEXT_VIEW(global_gui->source_view);
        GdkRectangle vr;
        gtk_text_view_get_visible_rect(tv, &vr);
        GtkTextIter top_it;
        gtk_text_view_get_iter_at_location(tv, &top_it, vr.x, vr.y);
        top_line = gtk_text_iter_get_line(&top_it);
    }

    extern const char *zig_get_document_text(void);
    extern void zig_free_document_text(const char *ptr);

    const char *text = zig_get_document_text();
    if (text) {
        global_gui->loading_viewport = TRUE;
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_gui->source_view));
        /* Cancel deferred passes queued against the pre-reload content. */
        global_gui->buffer_generation++;
        gtk_text_buffer_set_text(buf, text, -1);
        global_gui->loading_viewport = FALSE;
        zig_free_document_text(text);
        /* Wholesale replace bypassed the insert/delete handlers — drop the
         * incremental word-count cache so the next stats pass recomputes it. */
        gui_word_count_invalidate();
    }

    gui_set_cursor_position(line, col);

    int *tl = g_new(int, 1);
    *tl = top_line;
    g_idle_add_full(G_PRIORITY_HIGH_IDLE, reload_finalize_idle, tl, NULL);
}

void gui_prepare_tab_switch(void) {
    if (!global_gui) return;
    if (global_gui->source_view) {
        /* Blank the view during the switch rather than parking a literal
         * "Loading…" string — a blank buffer is immediately editable even
         * before the first save. */
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_gui->source_view));
        /* Cancel deferred passes queued against the outgoing tab's content. */
        global_gui->buffer_generation++;
        gtk_text_buffer_set_text(buf, "", -1);
    }

    while (g_main_context_pending(NULL)) {
        g_main_context_iteration(NULL, FALSE);
    }

    if (global_gui->vadjustment) {
        gtk_adjustment_set_value(global_gui->vadjustment, 0.0);
    }
    gui_reset_preferred_x(global_gui);
}
