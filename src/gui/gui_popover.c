#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <string.h>
#include <stdio.h>
#include "gui_internal.h"

void apply_paragraph_alignment(GtkTextBuffer *buf, GtkJustification justification) {
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buf);
    GtkTextTag *tag_left = gtk_text_tag_table_lookup(table, "align-left");
    if (!tag_left) {
        tag_left = gtk_text_buffer_create_tag(buf, "align-left", "justification", GTK_JUSTIFY_LEFT, NULL);
    }
    GtkTextTag *tag_center = gtk_text_tag_table_lookup(table, "align-center");
    if (!tag_center) {
        tag_center = gtk_text_buffer_create_tag(buf, "align-center", "justification", GTK_JUSTIFY_CENTER, NULL);
    }
    GtkTextTag *tag_right = gtk_text_tag_table_lookup(table, "align-right");
    if (!tag_right) {
        tag_right = gtk_text_buffer_create_tag(buf, "align-right", "justification", GTK_JUSTIFY_RIGHT, NULL);
    }
    GtkTextTag *tag_justify = gtk_text_tag_table_lookup(table, "align-justify");
    if (!tag_justify) {
        tag_justify = gtk_text_buffer_create_tag(buf, "align-justify", "justification", GTK_JUSTIFY_FILL, NULL);
    }

    GtkTextTag *target_tag = NULL;
    switch (justification) {
        case GTK_JUSTIFY_LEFT:   target_tag = tag_left; break;
        case GTK_JUSTIFY_CENTER: target_tag = tag_center; break;
        case GTK_JUSTIFY_RIGHT:  target_tag = tag_right; break;
        case GTK_JUSTIFY_FILL:   target_tag = tag_justify; break;
        default: return;
    }

    GtkTextIter start, end;
    if (!gtk_text_buffer_get_selection_bounds(buf, &start, &end)) {
        gtk_text_buffer_get_iter_at_mark(buf, &start, gtk_text_buffer_get_insert(buf));
        end = start;
    }

    gint start_line = gtk_text_iter_get_line(&start);
    gint end_line = gtk_text_iter_get_line(&end);

    gtk_text_buffer_begin_user_action(buf);

    for (gint l = start_line; l <= end_line; l++) {
        GtkTextIter line_start, line_end;
        gtk_text_buffer_get_iter_at_line(buf, &line_start, l);
        line_end = line_start;
        gtk_text_iter_forward_to_line_end(&line_end);

        gtk_text_buffer_remove_tag(buf, tag_left, &line_start, &line_end);
        gtk_text_buffer_remove_tag(buf, tag_center, &line_start, &line_end);
        gtk_text_buffer_remove_tag(buf, tag_right, &line_start, &line_end);
        gtk_text_buffer_remove_tag(buf, tag_justify, &line_start, &line_end);

        gtk_text_buffer_apply_tag(buf, target_tag, &line_start, &line_end);
    }

    gtk_text_buffer_end_user_action(buf);
}

typedef struct {
    GtkTextBuffer *buf;
    GtkWidget     *popover;
    gint           saved_start;
    gint           saved_end;
    GtkWidget     *open_submenu; /* currently-open Format/Paragraph flyout, or NULL */
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

/* gtk_text_view_get_iter_at_position() can hit a GTK4 bug
 * (gtk_text_iter_set_visible_line_index() -> "Byte index N is off the end
 * of the line" -> Gtk-ERROR/abort) on lines that contain both an
 * "invisible" tag (our markdown conceal tag) and multi-byte UTF-8
 * characters. Avoid it entirely: find the line under the pointer with
 * gtk_text_view_get_line_at_y() (always returns byte 0 of a line, never
 * crashes), then walk forward comparing per-character pixel locations
 * with gtk_text_view_get_iter_location() (iter -> pixel, also safe). */
static gboolean editor_get_iter_at_widget_point(AppGui *gui, gdouble x, gdouble y, GtkTextIter *iter) {
    if (!gui || !gui->source_view || !iter) return FALSE;
    int bx, by;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(gui->source_view),
                                         GTK_TEXT_WINDOW_WIDGET,
                                         (int)x, (int)y, &bx, &by);

    GtkTextView *view = GTK_TEXT_VIEW(gui->source_view);
    GtkTextIter line_iter;
    int line_top = 0;
    gtk_text_view_get_line_at_y(view, &line_iter, by, &line_top);
    gtk_text_iter_set_line_offset(&line_iter, 0);

    GtkTextIter line_end = line_iter;
    if (!gtk_text_iter_ends_line(&line_end)) {
        gtk_text_iter_forward_to_line_end(&line_end);
    }

    int chars_in_line = gtk_text_iter_get_chars_in_line(&line_iter);
    GdkRectangle rect;
    for (int col = 0; col < chars_in_line; col++) {
        GtkTextIter test = line_iter;
        gtk_text_iter_forward_chars(&test, col);
        gtk_text_view_get_iter_location(view, &test, &rect);
        if (bx < rect.x + rect.width) {
            *iter = test;
            return TRUE;
        }
    }

    *iter = line_end;
    return TRUE;
}

static void apply_format_with_saved(GtkTextBuffer *buf, const char *prefix, const char *suffix,
                                    gint saved_start, gint saved_end) {
    gboolean has_selection = (saved_start != saved_end);
    AppGui *gui = global_gui;
    if (!has_selection) {
        GtkTextIter cursor_iter;
        gtk_text_buffer_get_iter_at_offset(buf, &cursor_iter, saved_start);
        Position start_pos = iter_to_position(&cursor_iter);
        char *wrapped = g_strconcat(prefix, suffix, NULL);
        zig_insert_text(start_pos, wrapped);
        gui_reload_full_buffer();
        Position cursor_pos = advance_position(start_pos, prefix);
        gui_set_cursor_position(cursor_pos.line + 1, cursor_pos.col);
        zig_undo_commit();
        g_free(wrapped);
    } else {
        GtkTextIter start, end;
        gtk_text_buffer_get_iter_at_offset(buf, &start, saved_start);
        gtk_text_buffer_get_iter_at_offset(buf, &end,   saved_end);
        gchar *text  = gtk_text_buffer_get_text(buf, &start, &end, TRUE);
        size_t tlen  = strlen(text);
        size_t plen  = strlen(prefix);
        size_t slen  = strlen(suffix);

        /* Toggle off: same marker already wraps the text — strip it instead
         * of nesting (no more ****word**** or **`word`** from a re-apply). */

        /* (a) markers sit inside the selection. */
        if (tlen >= plen + slen &&
            strncmp(text, prefix, plen) == 0 &&
            strcmp(text + tlen - slen, suffix) == 0) {
            gchar *inner = g_strndup(text + plen, tlen - plen - slen);
            Position sp = iter_to_position(&start);
            Position ep = iter_to_position(&end);
            zig_replace_range(sp, ep, inner);
            gui_reload_full_buffer();
            select_position_range(gui, sp, advance_position(sp, inner));
            zig_undo_commit();
            g_free(inner);
            g_free(text);
            return;
        }

        /* (b) markers sit immediately OUTSIDE the selection (user selected
         * just the inner word). Strip those surrounding markers. */
        GtkTextIter bstart = start;
        GtkTextIter aend   = end;
        if (gtk_text_iter_backward_chars(&bstart, (int)plen) &&
            gtk_text_iter_forward_chars(&aend, (int)slen)) {
            gchar *before = gtk_text_buffer_get_text(buf, &bstart, &start, TRUE);
            gchar *after  = gtk_text_buffer_get_text(buf, &end, &aend, TRUE);
            if (strcmp(before, prefix) == 0 && strcmp(after, suffix) == 0) {
                Position sp = iter_to_position(&bstart);
                Position ep = iter_to_position(&aend);
                zig_replace_range(sp, ep, text);
                gui_reload_full_buffer();
                select_position_range(gui, sp, advance_position(sp, text));
                zig_undo_commit();
                g_free(before);
                g_free(after);
                g_free(text);
                return;
            }
            g_free(before);
            g_free(after);
        }

        /* (c) default: wrap. */
        gchar *new_text = g_strconcat(prefix, text, suffix, NULL);
        Position start_pos = iter_to_position(&start);
        Position end_pos = iter_to_position(&end);
        zig_replace_range(start_pos, end_pos, new_text);
        gui_reload_full_buffer();
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
    gui_reload_full_buffer();
    gui_set_cursor_position(end_line + 1, G_MAXINT);
    zig_undo_commit();

    g_free(block_dup);
    g_free(new_block);
}

static void apply_paragraph_format_with_saved(GtkTextBuffer *buf, const char *prefix,
                                              gint saved_start, gint saved_end) {
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_offset(buf, &start, saved_start);
    gtk_text_buffer_get_iter_at_offset(buf, &end,   saved_end);
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
    return G_SOURCE_REMOVE;
}

/* Generic "native menu row": icon + left-aligned label, reuses the
 * .pop-btn/.menu-item-btn styling already used by the status-bar action
 * menu (gui.c status_menu_item) for the long/thin native look. */
static GtkWidget *ctx_menu_item(const char *icon, const char *label, GCallback cb, gpointer user_data) {
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "pop-btn");
    gtk_widget_add_css_class(btn, "menu-item-btn");
    gtk_widget_add_css_class(btn, "ctx-item");
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(hbox), gtk_image_new_from_icon_name(icon));
    GtkWidget *lbl = gtk_label_new(label);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_box_append(GTK_BOX(hbox), lbl);
    gtk_button_set_child(GTK_BUTTON(btn), hbox);
    if (cb) g_signal_connect(btn, "clicked", cb, user_data);
    return btn;
}

/* Same as ctx_menu_item but with a trailing "go-next" arrow, marking the
 * row as one that opens a flyout submenu. */
static GtkWidget *ctx_submenu_row(const char *icon, const char *label) {
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "pop-btn");
    gtk_widget_add_css_class(btn, "menu-item-btn");
    gtk_widget_add_css_class(btn, "ctx-item");
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(hbox), gtk_image_new_from_icon_name(icon));
    GtkWidget *lbl = gtk_label_new(label);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_box_append(GTK_BOX(hbox), lbl);
    gtk_box_append(GTK_BOX(hbox), gtk_image_new_from_icon_name("go-next-symbolic"));
    gtk_button_set_child(GTK_BUTTON(btn), hbox);
    return btn;
}

/* Plain text menu row used inside the Format/Paragraph flyouts, with
 * "prefix"/"suffix" object data for on_format_clicked/on_para_clicked. */
static GtkWidget *flyout_item(const char *label, const char *prefix, const char *suffix,
                              GCallback cb, gpointer user_data) {
    GtkWidget *btn = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(btn, "pop-btn");
    gtk_widget_add_css_class(btn, "menu-item-btn");
    gtk_widget_set_halign(gtk_button_get_child(GTK_BUTTON(btn)), GTK_ALIGN_START);
    if (prefix) g_object_set_data(G_OBJECT(btn), "prefix", (gpointer)prefix);
    if (suffix) g_object_set_data(G_OBJECT(btn), "suffix", (gpointer)suffix);
    g_signal_connect(btn, "clicked", cb, user_data);
    return btn;
}

/* Closes/unparents the currently-open Format/Paragraph flyout, if any. */
static void on_submenu_closed(GtkPopover *sub, gpointer user_data) {
    PopoverData *pd = (PopoverData *)user_data;
    if (pd->open_submenu == GTK_WIDGET(sub)) pd->open_submenu = NULL;
    gtk_widget_unparent(GTK_WIDGET(sub));
}

static void close_open_submenu(PopoverData *pd) {
    if (!pd->open_submenu) return;
    GtkWidget *sub = pd->open_submenu;
    pd->open_submenu = NULL;
    g_signal_handlers_disconnect_by_func(sub, on_submenu_closed, pd);
    gtk_popover_popdown(GTK_POPOVER(sub));
    gtk_widget_unparent(sub);
}

/* Opens `content` as a right-pointing flyout anchored to `anchor`, closing
 * any previously-open flyout first. */
static void open_submenu(PopoverData *pd, GtkWidget *anchor, GtkWidget *content) {
    if (pd->open_submenu) {
        if (gtk_widget_get_parent(pd->open_submenu) == anchor) return;
        close_open_submenu(pd);
    }
    GtkWidget *sub = gtk_popover_new();
    gtk_widget_add_css_class(sub, "context-submenu");
    gtk_popover_set_has_arrow(GTK_POPOVER(sub), FALSE);
    gtk_popover_set_position(GTK_POPOVER(sub), GTK_POS_RIGHT);
    gtk_widget_set_parent(sub, anchor);
    gtk_popover_set_child(GTK_POPOVER(sub), content);
    g_signal_connect(sub, "closed", G_CALLBACK(on_submenu_closed), pd);
    pd->open_submenu = sub;
    gtk_popover_popup(GTK_POPOVER(sub));
}

static void on_format_clicked(GtkButton *btn, gpointer user_data) {
    PopoverData *pd = (PopoverData *)user_data;
    const char *prefix = g_object_get_data(G_OBJECT(btn), "prefix");
    const char *suffix = g_object_get_data(G_OBJECT(btn), "suffix");
    close_open_submenu(pd);
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
    close_open_submenu(pd);
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

static void on_insert_hr_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    PopoverData *pd = (PopoverData *)user_data;
    GtkTextBuffer *buf = pd->buf;
    close_open_submenu(pd);
    gtk_popover_popdown(GTK_POPOVER(pd->popover));
    if (global_source_view) gtk_widget_grab_focus(global_source_view);

    /* Restore the click position as the cursor, then insert the rule. */
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_offset(buf, &iter, pd->saved_start);
    gtk_text_buffer_place_cursor(buf, &iter);
    insert_horizontal_rule(buf);
}

static GtkWidget *build_format_submenu_box(PopoverData *pd) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(box, 4);
    gtk_widget_set_margin_end(box, 4);
    gtk_widget_set_margin_top(box, 4);
    gtk_widget_set_margin_bottom(box, 4);

    char *f_labels[]   = { "Bold", "Italic", "Strikethrough", "Highlight", "Code", "Comment", "Math", "Quote" };
    char *f_prefixes[] = { "**", "*", "~~", "==", "`", "<!-- ", "$", "> " };
    char *f_suffixes[] = { "**", "*", "~~", "==", "`", " -->", "$", "" };
    for (int i = 0; i < 8; i++) {
        GtkWidget *btn;
        if (i == 7) {
            btn = flyout_item(f_labels[i], "> ", NULL, G_CALLBACK(on_para_clicked), pd);
        } else {
            btn = flyout_item(f_labels[i], f_prefixes[i], f_suffixes[i], G_CALLBACK(on_format_clicked), pd);
        }
        gtk_box_append(GTK_BOX(box), btn);
    }
    return box;
}

static GtkWidget *build_paragraph_submenu_box(PopoverData *pd) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(box, 4);
    gtk_widget_set_margin_end(box, 4);
    gtk_widget_set_margin_top(box, 4);
    gtk_widget_set_margin_bottom(box, 4);

    GtkWidget *h_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_set_homogeneous(GTK_BOX(h_box), TRUE);
    char *h_labels[]   = { "H1", "H2", "H3", "H4", "H5", "H6" };
    char *h_prefixes[] = { "# ", "## ", "### ", "#### ", "##### ", "###### " };
    for (int i = 0; i < 6; i++) {
        GtkWidget *h_btn = gtk_button_new_with_label(h_labels[i]);
        gtk_widget_add_css_class(h_btn, "pop-btn");
        g_object_set_data(G_OBJECT(h_btn), "prefix", h_prefixes[i]);
        g_signal_connect(h_btn, "clicked", G_CALLBACK(on_para_clicked), pd);
        gtk_box_append(GTK_BOX(h_box), h_btn);
    }
    gtk_box_append(GTK_BOX(box), h_box);

    char *p_labels[]   = { "Body", "Bullet List", "Task List", "Numbered List" };
    char *p_prefixes[] = { "", "- ", "- [ ] ", "1. " };
    for (int i = 0; i < 4; i++) {
        gtk_box_append(GTK_BOX(box), flyout_item(p_labels[i], p_prefixes[i], NULL, G_CALLBACK(on_para_clicked), pd));
    }

    gtk_box_append(GTK_BOX(box), flyout_item(qirtas_tr("Horizontal Rule"), NULL, NULL, G_CALLBACK(on_insert_hr_clicked), pd));
    return box;
}

static void on_format_row_enter(GtkEventControllerMotion *ctrl, gdouble x, gdouble y, gpointer user_data) {
    (void)x; (void)y;
    PopoverData *pd = (PopoverData *)user_data;
    GtkWidget *anchor = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));
    open_submenu(pd, anchor, build_format_submenu_box(pd));
}

static void on_format_row_clicked(GtkButton *btn, gpointer user_data) {
    PopoverData *pd = (PopoverData *)user_data;
    open_submenu(pd, GTK_WIDGET(btn), build_format_submenu_box(pd));
}

static void on_paragraph_row_enter(GtkEventControllerMotion *ctrl, gdouble x, gdouble y, gpointer user_data) {
    (void)x; (void)y;
    PopoverData *pd = (PopoverData *)user_data;
    GtkWidget *anchor = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));
    open_submenu(pd, anchor, build_paragraph_submenu_box(pd));
}

static void on_paragraph_row_clicked(GtkButton *btn, gpointer user_data) {
    PopoverData *pd = (PopoverData *)user_data;
    open_submenu(pd, GTK_WIDGET(btn), build_paragraph_submenu_box(pd));
}

static void on_ctx_cut_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    PopoverData *pd = (PopoverData *)user_data;
    close_open_submenu(pd);
    gtk_popover_popdown(GTK_POPOVER(pd->popover));
    if (global_source_view) gtk_widget_grab_focus(global_source_view);
    GdkClipboard *clipboard = gtk_widget_get_clipboard(global_source_view);
    gtk_text_buffer_cut_clipboard(pd->buf, clipboard, TRUE);
}

static void on_ctx_copy_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    PopoverData *pd = (PopoverData *)user_data;
    close_open_submenu(pd);
    gtk_popover_popdown(GTK_POPOVER(pd->popover));
    if (global_source_view) gtk_widget_grab_focus(global_source_view);
    GdkClipboard *clipboard = gtk_widget_get_clipboard(global_source_view);
    gtk_text_buffer_copy_clipboard(pd->buf, clipboard);
}

static void on_ctx_paste_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    PopoverData *pd = (PopoverData *)user_data;
    close_open_submenu(pd);
    gtk_popover_popdown(GTK_POPOVER(pd->popover));
    if (global_source_view) gtk_widget_grab_focus(global_source_view);
    if (pd->saved_start == pd->saved_end) {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_offset(pd->buf, &it, pd->saved_start);
        gtk_text_buffer_place_cursor(pd->buf, &it);
    }
    GdkClipboard *clipboard = gtk_widget_get_clipboard(global_source_view);
    gtk_text_buffer_paste_clipboard(pd->buf, clipboard, NULL, TRUE);
}

static void on_popover_destroy(GtkWidget *widget, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (gui && gui->active_popover == widget) {
        gui->active_popover = NULL;
    }
}

/* Make sure an open Format/Paragraph flyout doesn't outlive the main
 * popover (e.g. user clicks outside / presses Escape without picking
 * an item). Must run before the g_free(pd) destroy handler below. */
static void on_main_popover_destroy_submenu(GtkWidget *popover, gpointer user_data) {
    (void)popover;
    close_open_submenu((PopoverData *)user_data);
}

static void on_editor_popover_closed(GtkPopover *popover, gpointer user_data) {
    (void)user_data;
    (void)popover;
}

/* Opens the Settings window from the context menu — the only reliable way
 * in focus mode, where the card header (and its ⋮ menu) are hidden. */
static void on_ctx_settings_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    PopoverData *pd = (PopoverData *)user_data;
    if (pd && pd->popover) gtk_popover_popdown(GTK_POPOVER(pd->popover));
    if (global_gui && global_gui->settings_window)
        gtk_window_present(GTK_WINDOW(global_gui->settings_window));
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
    PopoverData *pd = g_new0(PopoverData, 1);
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
    g_signal_connect(popover, "destroy", G_CALLBACK(on_main_popover_destroy_submenu), pd);
    g_signal_connect_swapped(popover, "destroy", G_CALLBACK(g_free), pd);

    /* Single narrow vertical column — long/thin like a native OS context
     * menu. Cut/Copy/Paste first, then Format/Paragraph rows that open
     * flyout submenus on hover or click. */
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(main_box, 4);
    gtk_widget_set_margin_end(main_box, 4);
    gtk_widget_set_margin_top(main_box, 4);
    gtk_widget_set_margin_bottom(main_box, 4);

    gtk_box_append(GTK_BOX(main_box), ctx_menu_item("edit-cut-symbolic", qirtas_tr("Cut"), G_CALLBACK(on_ctx_cut_clicked), pd));
    gtk_box_append(GTK_BOX(main_box), ctx_menu_item("edit-copy-symbolic", qirtas_tr("Copy"), G_CALLBACK(on_ctx_copy_clicked), pd));
    gtk_box_append(GTK_BOX(main_box), ctx_menu_item("edit-paste-symbolic", qirtas_tr("Paste"), G_CALLBACK(on_ctx_paste_clicked), pd));

    gtk_box_append(GTK_BOX(main_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    /* Format / Paragraph open their flyout only on CLICK — no hover-enter
     * auto-open (it fired the instant the menu popped under the cursor). */
    GtkWidget *format_row = ctx_submenu_row("format-text-bold-symbolic", qirtas_tr("Format"));
    g_signal_connect(format_row, "clicked", G_CALLBACK(on_format_row_clicked), pd);
    gtk_box_append(GTK_BOX(main_box), format_row);

    GtkWidget *paragraph_row = ctx_submenu_row("format-justify-fill-symbolic", qirtas_tr("Paragraph"));
    g_signal_connect(paragraph_row, "clicked", G_CALLBACK(on_paragraph_row_clicked), pd);
    gtk_box_append(GTK_BOX(main_box), paragraph_row);

    gtk_box_append(GTK_BOX(main_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(main_box),
        ctx_menu_item("preferences-system-symbolic", qirtas_tr("Settings"),
                      G_CALLBACK(on_ctx_settings_clicked), pd));

    gtk_popover_set_child(GTK_POPOVER(popover), main_box);
    gtk_popover_popup(GTK_POPOVER(popover));
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}
