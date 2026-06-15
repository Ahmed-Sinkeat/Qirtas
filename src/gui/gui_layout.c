#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <math.h>
#include <string.h>
#include "gui_internal.h"

/* Editor layout & display preferences: paper-column sizing, editor border,
 * focus/read/compact modes, layout dividers, column-resize drag, and the
 * settings callbacks that drive them. Extracted from gui.c.
 * Cross-file entry points are declared in gui_internal.h. */

#define QIRTAS_READ_MODE_MAX_WIDTH 760
#define QIRTAS_RESIZE_HOTZONE 6

/* Last observed paper width signature; -1 forces paper_column_tick to recompute. */
static int s_last_paper_width = -1;

void reorder_main_layout(AppGui *gui) {
    if (!gui || !gui->main_vertical_box || !gui->bottom_bar_widget || !gui->sidebar_editor_box) return;

    gboolean status_bar_is_top = FALSE; // Default to Bottom
    if (!gui->enable_focus_mode && gui->sb_pos_dropdown) {
        guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(gui->sb_pos_dropdown));
        if (selected == 1) status_bar_is_top = TRUE;
    }

    if (gui->tab_strip)
        gtk_box_reorder_child_after(GTK_BOX(gui->main_vertical_box), gui->tab_strip, NULL);

    GtkWidget *anchor = gui->tab_strip;

    if (status_bar_is_top) {
        gtk_box_reorder_child_after(GTK_BOX(gui->main_vertical_box), gui->bottom_bar_widget, anchor);
        gtk_box_reorder_child_after(GTK_BOX(gui->main_vertical_box), gui->sidebar_editor_box, gui->bottom_bar_widget);
    } else {
        gtk_box_reorder_child_after(GTK_BOX(gui->main_vertical_box), gui->sidebar_editor_box, anchor);
        gtk_box_reorder_child_after(GTK_BOX(gui->main_vertical_box), gui->bottom_bar_widget, gui->sidebar_editor_box);
    }
}

void apply_editor_border(AppGui *gui) {
    GtkWidget *card = (gui && gui->editor_card) ? gui->editor_card : (gui ? gui->scrolled : NULL);
    if (!card) return;

    gtk_widget_set_hexpand(card, TRUE);
    gtk_widget_set_halign(card, GTK_ALIGN_FILL);
    gtk_widget_set_size_request(card, -1, -1);

    if (gui->enable_focus_mode) {
        gtk_widget_remove_css_class(card, "focus-mode");
        gtk_widget_set_margin_start(card, gui->desk_gap);
        gtk_widget_set_margin_end(card, gui->desk_gap);
        gtk_widget_set_margin_top(card, 28);
        gtk_widget_set_margin_bottom(card, 24);
        return;
    }

    if (gui->enable_editor_border) {
        gtk_widget_remove_css_class(card, "focus-mode");
        int top = gui->compact_mode ? 10 : 28;
        int bot = gui->compact_mode ?  8 : 24;
        gtk_widget_set_margin_start(card, gui->desk_gap);
        gtk_widget_set_margin_end(card, gui->desk_gap);
        gtk_widget_set_margin_top(card, top);
        gtk_widget_set_margin_bottom(card, bot);
    } else {
        gtk_widget_add_css_class(card, "focus-mode");
        gtk_widget_set_margin_start(card, 0);
        gtk_widget_set_margin_end(card, 0);
        gtk_widget_set_margin_top(card, 0);
        gtk_widget_set_margin_bottom(card, 0);
    }
}

gboolean paper_column_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
    (void)clock;
    AppGui *gui = data;
    if (!gui || !gui->source_view) return G_SOURCE_CONTINUE;
    int width = gtk_widget_get_width(widget);
    if (width <= 1) return G_SOURCE_CONTINUE;

    GtkSourceView *sv = GTK_SOURCE_VIEW(gui->source_view);
    gboolean ln = gui->show_line_numbers;
    GtkWidget *gutter = GTK_WIDGET(gtk_source_view_get_gutter(sv, GTK_TEXT_WINDOW_LEFT));
    int gw = (ln && gutter) ? gtk_widget_get_width(gutter) : 0;

    int sig = width ^ (ln ? 0x40000000 : 0) ^ (gw << 8);
    if (sig == s_last_paper_width) return G_SOURCE_CONTINUE;
    s_last_paper_width = sig;

    int text_w = width - QIRTAS_CARD_CHROME;
    if (text_w < QIRTAS_TEXT_COLUMN_MIN) text_w = QIRTAS_TEXT_COLUMN_MIN;
    /* Centered mode: cap at the user's chosen column width (slider). Falls back
     * to the old fixed max if unset. */
    int centered_max = (gui->centered_text_width > 0) ? gui->centered_text_width : QIRTAS_TEXT_COLUMN_MAX;
    if (!gui->text_width_full_page && text_w > centered_max)
        text_w = centered_max;
    if (gui->read_mode && !gui->text_width_full_page && text_w > QIRTAS_READ_MODE_MAX_WIDTH)
        text_w = QIRTAS_READ_MODE_MAX_WIDTH;
    gui->text_column_width = text_w;

    int margin = (width - text_w) / 2;
    if (margin < 8) margin = 8;

    if (ln) {
        const int GAP = 8;
        int gutter_shift = margin - gw - GAP;
        if (gutter_shift < 0) gutter_shift = 0;
        if (gutter) gtk_widget_set_margin_start(gutter, gutter_shift);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(gui->source_view), GAP);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(gui->source_view), margin);
    } else {
        if (gutter) gtk_widget_set_margin_start(gutter, 0);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(gui->source_view), margin);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(gui->source_view), margin);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean restore_read_mode_scroll_cb(gpointer user_data) {
    ReadModeScrollData *d = user_data;
    if (d->generation == d->gui->buffer_generation && d->gui->source_view) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->gui->source_view));
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(d->gui->source_view), d->mark, 0.0, TRUE, 0.0, 0.0);
        gtk_text_buffer_delete_mark(buf, d->mark);
    }
    g_free(d);
    return G_SOURCE_REMOVE;
}

void apply_focus_mode(AppGui *gui) {
    if (!gui || !gui->scrolled || !gui->sidebar || !gui->main_vertical_box ||
        !gui->bottom_bar_widget || !gui->sidebar_editor_box) return;

    if (gui->enable_focus_mode) {
        gtk_widget_set_visible(gui->sidebar, FALSE);
        reorder_main_layout(gui);
        apply_editor_border(gui);
        s_last_paper_width = -1;
        if (gui->editor_header) gtk_widget_set_visible(gui->editor_header, FALSE);
        if (gui->sb_pos_dropdown) gtk_widget_set_sensitive(gui->sb_pos_dropdown, FALSE);
        if (gui->sb_side_dropdown) gtk_widget_set_sensitive(gui->sb_side_dropdown, FALSE);
        if (gui->divider_chk) gtk_widget_set_sensitive(gui->divider_chk, FALSE);
        if (gui->btn_sidebar_toggle) gtk_widget_set_sensitive(gui->btn_sidebar_toggle, TRUE);
    } else {
        gtk_widget_set_visible(gui->sidebar, TRUE);
        reorder_main_layout(gui);
        if (gui->editor_header) gtk_widget_set_visible(gui->editor_header, TRUE);
        if (gui->editor_thread) gtk_widget_set_visible(gui->editor_thread, TRUE);
        apply_editor_border(gui);
        if (gui->sb_pos_dropdown) gtk_widget_set_sensitive(gui->sb_pos_dropdown, TRUE);
        if (gui->sb_side_dropdown) gtk_widget_set_sensitive(gui->sb_side_dropdown, TRUE);
        if (gui->divider_chk) gtk_widget_set_sensitive(gui->divider_chk, TRUE);
        if (gui->btn_sidebar_toggle) gtk_widget_set_sensitive(gui->btn_sidebar_toggle, TRUE);
    }

    if (gui->focus_chk) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(gui->focus_chk), gui->enable_focus_mode);
    }
}

void toggle_read_mode(AppGui *gui) {
    if (!gui || !gui->source_view) return;

    GtkTextView *tv = GTK_TEXT_VIEW(gui->source_view);
    GtkTextBuffer *buf = gtk_text_view_get_buffer(tv);

    GdkRectangle visible;
    gtk_text_view_get_visible_rect(tv, &visible);
    GtkTextIter top_iter;
    gtk_text_view_get_iter_at_location(tv, &top_iter, visible.x, visible.y);
    GtkTextMark *scroll_anchor = gtk_text_buffer_create_mark(buf, NULL, &top_iter, TRUE);

    gui->read_mode = !gui->read_mode;

    gtk_text_view_set_editable(tv, !gui->read_mode);
    gtk_text_view_set_cursor_visible(tv, !gui->read_mode);

    if (gui->editor_card) {
        if (gui->read_mode) gtk_widget_add_css_class(gui->editor_card, "read-mode");
        else gtk_widget_remove_css_class(gui->editor_card, "read-mode");
    }
    if (gui->btn_read_mode) {
        if (gui->read_mode) gtk_widget_add_css_class(gui->btn_read_mode, "active");
        else gtk_widget_remove_css_class(gui->btn_read_mode, "active");
    }

    s_last_paper_width = -1;
    if (gui->editor_card) paper_column_tick(gui->editor_card, NULL, gui);
    update_conceal_markdown_all_sync(buf);

    ReadModeScrollData *d = g_new(ReadModeScrollData, 1);
    d->gui = gui;
    d->mark = scroll_anchor;
    d->generation = gui->buffer_generation;
    g_idle_add_full(G_PRIORITY_LOW, restore_read_mode_scroll_cb, d, NULL);
}

void apply_compact_mode(AppGui *gui) {
    if (!gui || !gui->window) return;
    if (gui->compact_mode)
        gtk_widget_add_css_class(gui->window, "compact-ui");
    else
        gtk_widget_remove_css_class(gui->window, "compact-ui");
    apply_editor_border(gui);
}

void on_editor_border_toggled(GtkCheckButton *chk, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gboolean active = gtk_check_button_get_active(chk);
    gui->enable_editor_border = active;
    zig_set_editor_border(active ? 1 : 0);
    apply_editor_border(gui);
}

gboolean paper_column_timeout_wrapper(gpointer data) {
    AppGui *gui = data;
    if (!gui || !gui->editor_card) return G_SOURCE_CONTINUE;
    paper_column_tick(gui->editor_card, NULL, gui);
    return G_SOURCE_CONTINUE;
}

void on_outline_close_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppGui *gui = (AppGui *)user_data;
    if (!gui || !gui->outline_panel) return;
    gui->outline_panel_visible = FALSE;
    gtk_widget_set_visible(gui->outline_panel, FALSE);
    qirtas_pref_set_int("outline_panel_visible", 0);
}

void on_compact_mode_toggled(GtkCheckButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gui->compact_mode = gtk_check_button_get_active(btn);
    qirtas_pref_set_int("compact_mode", gui->compact_mode ? 1 : 0);
    apply_compact_mode(gui);
}

void on_highlight_line_toggled(GtkCheckButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gui->highlight_current_line = gtk_check_button_get_active(btn);
    qirtas_pref_set_int("highlight_current_line", gui->highlight_current_line ? 1 : 0);
    apply_editor_prefs(gui);
}

void on_line_numbers_toggled(GtkCheckButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gui->show_line_numbers = gtk_check_button_get_active(btn);
    qirtas_pref_set_int("show_line_numbers", gui->show_line_numbers ? 1 : 0);
    apply_editor_prefs(gui);
}

void on_restore_session_toggled(GtkCheckButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gui->restore_session = gtk_check_button_get_active(btn);
    qirtas_pref_set_int("restore_session", gui->restore_session ? 1 : 0);
}

void on_text_width_mode_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    AppGui *gui = (AppGui *)user_data;
    GtkDropDown *dropdown = GTK_DROP_DOWN(gobject);
    guint selected = gtk_drop_down_get_selected(dropdown);
    gui->text_width_full_page = (selected == 1);
    qirtas_pref_set_int("text_width_full_page", gui->text_width_full_page ? 1 : 0);
    if (gui->editor_card) {
        s_last_paper_width = -1; /* force the column tick to recompute */
        paper_column_tick(gui->editor_card, NULL, gui);
    }
}

void on_width_slider_changed(GtkRange *range, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gui->centered_text_width = (int)gtk_range_get_value(range);
    qirtas_pref_set_int("centered_text_width", gui->centered_text_width);
    if (gui->editor_card) {
        s_last_paper_width = -1;
        paper_column_tick(gui->editor_card, NULL, gui);
    }
}

void on_card_gap_slider_changed(GtkRange *range, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    int g = (int)gtk_range_get_value(range);
    if (g < QIRTAS_DESK_GAP_MIN) g = QIRTAS_DESK_GAP_MIN;
    if (g > QIRTAS_DESK_GAP_MAX) g = QIRTAS_DESK_GAP_MAX;
    gui->desk_gap = g;
    qirtas_pref_set_int("desk_gap", g);
    apply_editor_border(gui);
    if (gui->editor_card) {
        s_last_paper_width = -1;
        paper_column_tick(gui->editor_card, NULL, gui);
    }
}

void on_focus_mode_toggled(GtkCheckButton *chk, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gboolean active = gtk_check_button_get_active(chk);
    if (gui->enable_focus_mode == active) return;
    gui->enable_focus_mode = active;
    zig_set_focus_mode(active ? 1 : 0);
    apply_focus_mode(gui);
}

void apply_editor_prefs(AppGui *gui) {
    if (!gui || !gui->source_view) return;
    GtkTextView   *view = GTK_TEXT_VIEW(gui->source_view);
    GtkSourceView *sv   = GTK_SOURCE_VIEW(gui->source_view);

    gtk_text_view_set_wrap_mode(view, gui->wrap_lines ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);

    /* Flip the RTL-paragraph left-justify override (see update_paragraph_direction)
     * to match the new wrap state — avoids the right-side blank gap on Arabic
     * text when wrap is off. */
    {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(view);
        GtkTextTag *rtl_tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buf), "rtl-tag");
        if (rtl_tag) {
            g_object_set(rtl_tag,
                          "justification", GTK_JUSTIFY_LEFT,
                          "justification-set", !gui->wrap_lines,
                          NULL);
        }
    }

    gtk_source_view_set_show_line_numbers(sv, gui->show_line_numbers);
    gtk_source_view_set_highlight_current_line(sv, gui->highlight_current_line);
    gtk_source_view_set_show_right_margin(sv, gui->show_right_margin);
    if (gui->right_margin_pos < 20)  gui->right_margin_pos = 20;
    if (gui->right_margin_pos > 200) gui->right_margin_pos = 200;
    gtk_source_view_set_right_margin_position(sv, (guint)gui->right_margin_pos);
    if (gui->source_map) gtk_widget_set_visible(gui->source_map, gui->show_overview_map);

    /* Force an immediate paper-column recompute. Toggling wrap changes the
     * usable text width but does NOT change the gutter, so the column tick's
     * width signature wouldn't change and stale margins would leave dead space
     * on the right until something else (e.g. line numbers) forced a relayout. */
    s_last_paper_width = -1;
    if (gui->editor_card) paper_column_tick(gui->editor_card, NULL, gui);
}

void apply_layout_dividers(AppGui *gui) {
    if (!gui || !gui->main_vertical_box || !gui->sidebar_editor_box) return;

    if (gui->show_layout_dividers) {
        gtk_widget_add_css_class(gui->main_vertical_box, "layout-dividers-on");
        gtk_widget_remove_css_class(gui->main_vertical_box, "layout-dividers-off");
        gtk_widget_add_css_class(gui->sidebar_editor_box, "layout-dividers-on");
        gtk_widget_remove_css_class(gui->sidebar_editor_box, "layout-dividers-off");
    } else {
        gtk_widget_add_css_class(gui->main_vertical_box, "layout-dividers-off");
        gtk_widget_remove_css_class(gui->main_vertical_box, "layout-dividers-on");
        gtk_widget_add_css_class(gui->sidebar_editor_box, "layout-dividers-off");
        gtk_widget_remove_css_class(gui->sidebar_editor_box, "layout-dividers-on");
    }
}

int paper_edge_margin(AppGui *gui, GtkWidget *overlay) {
    (void)overlay;
    return gui->desk_gap;
}

void on_column_resize_motion(GtkEventControllerMotion *ctrl, gdouble x, gdouble y, gpointer user_data) {
    (void)y;
    AppGui *gui = user_data;
    GtkWidget *overlay = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));
    if (gui->resizing_text_column) return;
    int width = gtk_widget_get_width(overlay);
    int margin = paper_edge_margin(gui, overlay);
    gboolean near_edge = (fabs(x - margin) < QIRTAS_RESIZE_HOTZONE) ||
                         (fabs(x - (width - margin)) < QIRTAS_RESIZE_HOTZONE);
    GdkCursor *cursor = near_edge ? gdk_cursor_new_from_name("col-resize", NULL) : NULL;
    gtk_widget_set_cursor(overlay, cursor);
    if (cursor) g_object_unref(cursor);
}

void on_column_resize_begin(GtkGestureDrag *gesture, gdouble x, gdouble y, gpointer user_data) {
    (void)y;
    AppGui *gui = user_data;
    GtkWidget *overlay = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    int width = gtk_widget_get_width(overlay);
    int margin = paper_edge_margin(gui, overlay);

    if (fabs(x - margin) < QIRTAS_RESIZE_HOTZONE) {
        gui->resize_drag_edge = -1;
    } else if (fabs(x - (width - margin)) < QIRTAS_RESIZE_HOTZONE) {
        gui->resize_drag_edge = 1;
    } else {
        gui->resize_drag_edge = 0;
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }

    gui->resizing_text_column = TRUE;
    gui->resize_drag_start_gap = gui->desk_gap;

    /* Run the margin recompute at full frame rate only for the duration of
     * the drag; steady state relies on the low-frequency timeout instead. */
    if (gui->editor_card && gui->resize_column_tick_id == 0) {
        gui->resize_column_tick_id =
            gtk_widget_add_tick_callback(gui->editor_card, paper_column_tick, gui, NULL);
    }
}

void on_column_resize_update(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y, gpointer user_data) {
    (void)gesture; (void)offset_y;
    AppGui *gui = user_data;
    if (!gui->resizing_text_column || gui->resize_drag_edge == 0) return;

    /* Dragging an edge outward (away from the desk centre) shrinks that
     * edge's gap; dragging inward grows it. resize_drag_edge flips the sign
     * so either edge behaves the same way relative to its own side. */
    int new_gap = gui->resize_drag_start_gap - gui->resize_drag_edge * (int)offset_x;
    if (new_gap < QIRTAS_DESK_GAP_MIN) new_gap = QIRTAS_DESK_GAP_MIN;
    if (new_gap > QIRTAS_DESK_GAP_MAX) new_gap = QIRTAS_DESK_GAP_MAX;
    if (new_gap != gui->desk_gap) {
        gui->desk_gap = new_gap;
        s_last_paper_width = -1;
        apply_editor_border(gui); /* live-resize the visible paper card */
    }
}

void on_column_resize_end(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y, gpointer user_data) {
    (void)gesture; (void)offset_x; (void)offset_y;
    AppGui *gui = user_data;
    if (!gui->resizing_text_column) return;
    gui->resizing_text_column = FALSE;
    gui->resize_drag_edge = 0;

    if (gui->editor_card && gui->resize_column_tick_id != 0) {
        gtk_widget_remove_tick_callback(gui->editor_card, gui->resize_column_tick_id);
        gui->resize_column_tick_id = 0;
        /* Final recompute so the margins land at their settled value
         * immediately rather than waiting for the next timeout tick. */
        s_last_paper_width = -1;
        paper_column_tick(gui->editor_card, NULL, gui);
    }

    qirtas_pref_set_int("desk_gap", gui->desk_gap);
}
