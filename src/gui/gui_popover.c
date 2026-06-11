#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <string.h>
#include <stdio.h>
#include "gui_internal.h"

typedef struct {
    GtkTextBuffer *buf;
    GtkWidget     *popover;
    gint           saved_start;
    gint           saved_end;
} PopoverData;

typedef struct {
    GtkTextBuffer *buf;
    char          *prefix;
    char          *suffix;
    gint           saved_start;
    gint           saved_end;
    gboolean       is_paragraph;
} IdleFormatData;

typedef struct {
    GtkWidget *popover;
    GtkWidget *box_actions;
    GtkWidget *box_input;
    GtkWidget *entry_name;
    AppGui *gui;
} AddPopoverWidgets;

static Position iter_to_position(GtkTextIter *iter) {
    Position pos = { 0, 0 };
    if (!global_gui || !iter) return pos;
    pos.line = global_gui->viewport_start_line + gtk_text_iter_get_line(iter);
    pos.col = gtk_text_iter_get_line_offset(iter);
    return pos;
}

static Position advance_position(Position pos, const char *text) {
    if (!text) return pos;
    const char *p = text;
    while (*p) {
        gunichar c = g_utf8_get_char(p);
        if (c == '\n') {
            pos.line += 1;
            pos.col = 0;
        } else {
            pos.col += 1;
        }
        p = g_utf8_next_char(p);
    }
    return pos;
}

static void select_position_range(AppGui *gui, Position start, Position end) {
    if (!gui || !gui->source_view) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
    GtkTextIter start_iter, end_iter;

    int rel_start_line = start.line - gui->viewport_start_line;
    int rel_end_line = end.line - gui->viewport_start_line;
    if (rel_start_line < 0) rel_start_line = 0;
    if (rel_end_line < 0) rel_end_line = 0;

    gtk_text_buffer_get_iter_at_line(buf, &start_iter, rel_start_line);
    gtk_text_buffer_get_iter_at_line(buf, &end_iter, rel_end_line);

    for (int i = 0; i < start.col; i++) {
        if (gtk_text_iter_ends_line(&start_iter) || gtk_text_iter_is_end(&start_iter)) break;
        gtk_text_iter_forward_char(&start_iter);
    }
    for (int i = 0; i < end.col; i++) {
        if (gtk_text_iter_ends_line(&end_iter) || gtk_text_iter_is_end(&end_iter)) break;
        gtk_text_iter_forward_char(&end_iter);
    }

    gtk_text_buffer_select_range(buf, &start_iter, &end_iter);
}

static char *transform_paragraph_block(const char *block, const char *prefix, gint start_line) {
    if (!block) return g_strdup("");

    GString *result = g_string_new("");
    const char *line_start = block;
    gint line_index = 0;

    while (line_start && *line_start) {
        const char *line_end = line_start;
        while (*line_end && *line_end != '\n') {
            line_end++;
        }

        gboolean has_newline = (*line_end == '\n');
        gsize line_len = (gsize)(line_end - line_start);
        gchar *line = g_strndup(line_start, line_len);

        int ws = 0;
        while (line[ws] == ' ' || line[ws] == '\t') ws++;
        char *content = line + ws;

        int strip_len = 0;
        if (strncmp(content, "- [ ] ", 6) == 0 || strncmp(content, "- [x] ", 6) == 0 ||
            strncmp(content, "* [ ] ", 6) == 0 || strncmp(content, "* [x] ", 6) == 0) {
            strip_len = ws + 6;
        } else if (strncmp(content, "- ", 2) == 0 || strncmp(content, "* ", 2) == 0 ||
                   strncmp(content, "+ ", 2) == 0) {
            strip_len = ws + 2;
        } else if (content[0] == '#') {
            int h = 0;
            while (content[h] == '#') h++;
            if (content[h] == ' ') strip_len = ws + h + 1;
        } else {
            int num = 0;
            while (content[num] >= '0' && content[num] <= '9') num++;
            if (num > 0 && content[num] == '.' && content[num + 1] == ' ') {
                strip_len = ws + num + 2;
            }
        }

        g_string_append_len(result, line, ws);
        if (prefix && strlen(prefix) > 0) {
            char final_prefix[64];
            if (strcmp(prefix, "1. ") == 0) {
                snprintf(final_prefix, sizeof(final_prefix), "%d. ", line_index + 1);
            } else {
                snprintf(final_prefix, sizeof(final_prefix), "%s", prefix);
            }
            g_string_append(result, final_prefix);
        }
        g_string_append(result, content + strip_len);
        if (has_newline) {
            g_string_append_c(result, '\n');
            line_end++;
        }

        g_free(line);
        line_index++;
        if (!has_newline) break;
        line_start = line_end;
    }

    return g_string_free(result, FALSE);
}

static gboolean editor_get_iter_at_widget_point(AppGui *gui, gdouble x, gdouble y, GtkTextIter *iter) {
    if (!gui || !gui->source_view || !iter) return FALSE;
    int bx, by;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(gui->source_view),
                                         GTK_TEXT_WINDOW_WIDGET,
                                         (int)x, (int)y, &bx, &by);
    int trailing = 0;
    return gtk_text_view_get_iter_at_position(GTK_TEXT_VIEW(gui->source_view), iter, &trailing, bx, by);
}

static void apply_format_with_saved(GtkTextBuffer *buf, const char *prefix, const char *suffix,
                                    gint saved_start, gint saved_end) {
    gboolean has_selection = (saved_start != saved_end);
    AppGui *gui = global_gui;
    if (!has_selection) {
        GtkTextIter cursor_iter;
        debug_get_iter_at_offset(buf, &cursor_iter, saved_start, "apply_format_with_saved_cursor");
        Position start_pos = iter_to_position(&cursor_iter);
        char *wrapped = g_strconcat(prefix, suffix, NULL);
        zig_insert_text(start_pos, wrapped);
        load_viewport_page(gui, gui->viewport_start_line);
        Position cursor_pos = advance_position(start_pos, prefix);
        gui_set_cursor_position(cursor_pos.line + 1, cursor_pos.col);
        zig_undo_commit();
        g_free(wrapped);
    } else {
        GtkTextIter start, end;
        debug_get_iter_at_offset(buf, &start, saved_start, "apply_format_with_saved_start");
        debug_get_iter_at_offset(buf, &end,   saved_end,   "apply_format_with_saved_end");
        gchar *text     = gtk_text_buffer_get_text(buf, &start, &end, TRUE);
        gchar *new_text = g_strconcat(prefix, text, suffix, NULL);
        Position start_pos = iter_to_position(&start);
        Position end_pos = iter_to_position(&end);
        zig_replace_range(start_pos, end_pos, new_text);
        load_viewport_page(gui, gui->viewport_start_line);
        select_position_range(gui, start_pos, advance_position(start_pos, new_text));
        zig_undo_commit();
        g_free(text);
        g_free(new_text);
    }
}

void apply_format(GtkTextBuffer *buf, const char *prefix, const char *suffix) {
    GtkTextIter start, end;
    gint saved_start, saved_end;
    if (gtk_text_buffer_get_selection_bounds(buf, &start, &end)) {
        saved_start = gtk_text_iter_get_offset(&start);
        saved_end   = gtk_text_iter_get_offset(&end);
    } else {
        GtkTextMark *insert_mark = gtk_text_buffer_get_insert(buf);
        GtkTextIter cursor_iter;
        gtk_text_buffer_get_iter_at_mark(buf, &cursor_iter, insert_mark);
        saved_start = saved_end = gtk_text_iter_get_offset(&cursor_iter);
    }
    apply_format_with_saved(buf, prefix, suffix, saved_start, saved_end);
}

static void apply_paragraph_format_core(GtkTextBuffer *buf, const char *prefix,
                                        gint start_line, gint end_line) {
    (void)buf;
    int len = 0;
    extern const char *zig_get_text_for_line_range(int start_line, int end_line, int *out_len);
    const char *block_text = zig_get_text_for_line_range(start_line, end_line + 1, &len);
    if (!block_text) return;

    char *block_dup = g_strndup(block_text, len);
    char *new_block = transform_paragraph_block(block_dup, prefix, start_line);
    Position start_pos = { start_line, 0 };
    Position end_pos = { end_line + 1, 0 };
    zig_replace_range(start_pos, end_pos, new_block);
    load_viewport_page(global_gui, global_gui->viewport_start_line);
    gui_set_cursor_position(end_line + 1, G_MAXINT);
    zig_undo_commit();

    g_free(block_dup);
    g_free(new_block);
}

static void apply_paragraph_format_with_saved(GtkTextBuffer *buf, const char *prefix,
                                              gint saved_start, gint saved_end) {
    GtkTextIter start, end;
    debug_get_iter_at_offset(buf, &start, saved_start, "apply_paragraph_format_with_saved_start");
    debug_get_iter_at_offset(buf, &end,   saved_end,   "apply_paragraph_format_with_saved_end");
    gint start_line = gtk_text_iter_get_line(&start);
    gint end_line   = gtk_text_iter_get_line(&end);
    apply_paragraph_format_core(buf, prefix, start_line, end_line);
}

void apply_paragraph_format(GtkTextBuffer *buf, const char *prefix) {
    GtkTextIter start, end;
    if (!gtk_text_buffer_get_selection_bounds(buf, &start, &end)) {
        gtk_text_buffer_get_iter_at_mark(buf, &start, gtk_text_buffer_get_insert(buf));
        end = start;
    }
    gint start_line = gtk_text_iter_get_line(&start);
    gint end_line   = gtk_text_iter_get_line(&end);
    apply_paragraph_format_core(buf, prefix, start_line, end_line);
}


static gboolean do_idle_format(gpointer user_data) {
    IdleFormatData *ifd = (IdleFormatData *)user_data;
    g_print("IDLE_CALLBACK_START do_idle_format paragraph=%d prefix=%s suffix=%s start=%d end=%d\n",
            ifd->is_paragraph, ifd->prefix ? ifd->prefix : "null", ifd->suffix ? ifd->suffix : "null", ifd->saved_start, ifd->saved_end);
    GtkTextBuffer *buf = ifd->buf;

    if (global_gui) {
        g_signal_handlers_block_by_func(buf, on_mark_set,      global_gui);
        g_signal_handlers_block_by_func(buf, on_buffer_changed, global_gui);
    }

    if (ifd->is_paragraph) {
        apply_paragraph_format_with_saved(buf, ifd->prefix, ifd->saved_start, ifd->saved_end);
    } else {
        apply_format_with_saved(buf, ifd->prefix, ifd->suffix, ifd->saved_start, ifd->saved_end);
    }

    if (global_gui) {
        g_signal_handlers_unblock_by_func(buf, on_mark_set,      global_gui);
        g_signal_handlers_unblock_by_func(buf, on_buffer_changed, global_gui);
    }

    update_conceal_markdown_all(buf);

    if (global_gui && global_gui->source_view &&
        gtk_widget_get_realized(global_gui->source_view)) {
        GtkTextIter insert_iter;
        gtk_text_buffer_get_iter_at_mark(buf, &insert_iter, gtk_text_buffer_get_insert(buf));

        ScrollToCursorData *d = g_new(ScrollToCursorData, 1);
        d->gui  = global_gui;
        d->offset = gtk_text_iter_get_offset(&insert_iter);
        d->generation = global_gui->buffer_generation;
        g_idle_add(idle_scroll_to_cursor, d);
    }

    g_free(ifd->prefix);
    g_free(ifd->suffix);
    g_free(ifd);
    g_print("IDLE_CALLBACK_END do_idle_format SUCCESS\n");
    return G_SOURCE_REMOVE;
}

static void on_format_clicked(GtkButton *btn, gpointer user_data) {
    PopoverData *pd = (PopoverData *)user_data;
    const char *prefix = g_object_get_data(G_OBJECT(btn), "prefix");
    const char *suffix = g_object_get_data(G_OBJECT(btn), "suffix");
    gtk_popover_popdown(GTK_POPOVER(pd->popover));
    if (global_source_view) gtk_widget_grab_focus(global_source_view);
    IdleFormatData *ifd = g_new(IdleFormatData, 1);
    ifd->buf = pd->buf;
    ifd->prefix = prefix ? g_strdup(prefix) : NULL;
    ifd->suffix = suffix ? g_strdup(suffix) : NULL;
    ifd->saved_start = pd->saved_start;
    ifd->saved_end = pd->saved_end;
    ifd->is_paragraph = FALSE;
    g_idle_add(do_idle_format, ifd);
}

static void on_para_clicked(GtkButton *btn, gpointer user_data) {
    PopoverData *pd = (PopoverData *)user_data;
    const char *prefix = g_object_get_data(G_OBJECT(btn), "prefix");
    gtk_popover_popdown(GTK_POPOVER(pd->popover));
    if (global_source_view) gtk_widget_grab_focus(global_source_view);
    IdleFormatData *ifd = g_new(IdleFormatData, 1);
    ifd->buf = pd->buf;
    ifd->prefix = prefix ? g_strdup(prefix) : NULL;
    ifd->suffix = NULL;
    ifd->saved_start = pd->saved_start;
    ifd->saved_end = pd->saved_end;
    ifd->is_paragraph = TRUE;
    g_idle_add(do_idle_format, ifd);
}

static void on_popover_destroy(GtkWidget *widget, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (gui && gui->active_popover == widget) {
        gui->active_popover = NULL;
    }
}

static void on_editor_popover_closed(GtkPopover *popover, gpointer user_data) {
    (void)user_data;
    (void)popover;
}

void on_editor_right_click(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data) {
    (void)n_press;
    AppGui *gui = (AppGui *)user_data;
    if (gui->active_popover) {
        gtk_widget_unparent(gui->active_popover);
    }
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
    GtkWidget *popover = gtk_popover_new();
    gtk_widget_set_parent(popover, gui->source_view);
    gui->active_popover = popover;
    g_signal_connect(popover, "destroy", G_CALLBACK(on_popover_destroy), gui);
    g_signal_connect(popover, "closed", G_CALLBACK(on_editor_popover_closed), NULL);
    GdkRectangle rect = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    PopoverData *pd = g_new(PopoverData, 1);
    pd->buf = buf;
    pd->popover = popover;
    {
        GtkTextIter sel_start, sel_end;
        if (gtk_text_buffer_get_selection_bounds(buf, &sel_start, &sel_end)) {
            pd->saved_start = gtk_text_iter_get_offset(&sel_start);
            pd->saved_end   = gtk_text_iter_get_offset(&sel_end);
        } else {
            GtkTextIter cursor;
            if (!editor_get_iter_at_widget_point(gui, x, y, &cursor)) {
                GtkTextMark *ins = gtk_text_buffer_get_insert(buf);
                gtk_text_buffer_get_iter_at_mark(buf, &cursor, ins);
            }
            pd->saved_start = pd->saved_end = gtk_text_iter_get_offset(&cursor);
        }
    }
    g_signal_connect_swapped(popover, "destroy", G_CALLBACK(g_free), pd);
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(main_box, 8);
    gtk_widget_set_margin_end(main_box, 8);
    gtk_widget_set_margin_top(main_box, 8);
    gtk_widget_set_margin_bottom(main_box, 8);
    GtkWidget *format_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *lbl_format = gtk_label_new("FORMAT");
    gtk_widget_add_css_class(lbl_format, "pop-section-label");
    gtk_widget_set_halign(lbl_format, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(format_box), lbl_format);
    char *f_labels[] = { "Bold", "Italic", "Strikethrough", "Highlight", "Code", "Comment", "Math", "Quote" };
    char *f_prefixes[] = { "**", "*", "~~", "==", "`", "<!-- ", "$", "> " };
    char *f_suffixes[] = { "**", "*", "~~", "==", "`", " -->", "$", "" };
    for (int i = 0; i < 8; i++) {
        GtkWidget *btn = gtk_button_new_with_label(f_labels[i]);
        gtk_widget_add_css_class(btn, "pop-btn");
        gtk_widget_set_halign(btn, GTK_ALIGN_FILL);
        if (i == 7) {
            g_object_set_data(G_OBJECT(btn), "prefix", "> ");
            g_signal_connect(btn, "clicked", G_CALLBACK(on_para_clicked), pd);
        } else {
            g_object_set_data(G_OBJECT(btn), "prefix", f_prefixes[i]);
            g_object_set_data(G_OBJECT(btn), "suffix", f_suffixes[i]);
            g_signal_connect(btn, "clicked", G_CALLBACK(on_format_clicked), pd);
        }
        gtk_box_append(GTK_BOX(format_box), btn);
    }
    GtkWidget *para_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *lbl_para = gtk_label_new("PARAGRAPH");
    gtk_widget_add_css_class(lbl_para, "pop-section-label");
    gtk_widget_set_halign(lbl_para, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(para_box), lbl_para);
    GtkWidget *h_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_set_homogeneous(GTK_BOX(h_box), TRUE);
    char *h_labels[] = { "H1", "H2", "H3", "H4", "H5", "H6" };
    char *h_prefixes[] = { "# ", "## ", "### ", "#### ", "##### ", "###### " };
    for (int i = 0; i < 6; i++) {
        GtkWidget *h_btn = gtk_button_new_with_label(h_labels[i]);
        gtk_widget_add_css_class(h_btn, "pop-btn");
        g_object_set_data(G_OBJECT(h_btn), "prefix", h_prefixes[i]);
        g_signal_connect(h_btn, "clicked", G_CALLBACK(on_para_clicked), pd);
        gtk_box_append(GTK_BOX(h_box), h_btn);
    }
    gtk_box_append(GTK_BOX(para_box), h_box);
    char *p_labels[] = { "Body", "Bullet List", "Task List", "Numbered List" };
    char *p_prefixes[] = { "", "- ", "- [ ] ", "1. " };
    for (int i = 0; i < 4; i++) {
        GtkWidget *btn = gtk_button_new_with_label(p_labels[i]);
        gtk_widget_add_css_class(btn, "pop-btn");
        gtk_widget_set_halign(btn, GTK_ALIGN_FILL);
        g_object_set_data(G_OBJECT(btn), "prefix", p_prefixes[i]);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_para_clicked), pd);
        gtk_box_append(GTK_BOX(para_box), btn);
    }
    gtk_box_append(GTK_BOX(main_box), format_box);
    gtk_box_append(GTK_BOX(main_box), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(main_box), para_box);
    gtk_popover_set_child(GTK_POPOVER(popover), main_box);
    gtk_popover_popup(GTK_POPOVER(popover));
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}
