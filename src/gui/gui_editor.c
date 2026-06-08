#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <string.h>
#include <stdlib.h>
#include <sqlite3.h>
#include "gui_internal.h"

/* Forward declare externally defined helper functions from other modules if needed */
extern void gui_set_sync_status(const char *status);
extern char* gui_get_text(void);
extern void gui_set_text(const char *text, int len);
extern void gui_set_title(const char *title);
extern void gui_trigger_autosave(void);

void duplicate_current_line(GtkTextBuffer *buf) {
    GtkTextIter cursor_iter;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor_iter, gtk_text_buffer_get_insert(buf));
    gint line_num = gtk_text_iter_get_line(&cursor_iter);
    gint col = gtk_text_iter_get_line_offset(&cursor_iter);

    gtk_text_buffer_begin_user_action(buf);

    GtkTextIter line_start, line_end;
    gtk_text_buffer_get_iter_at_line(buf, &line_start, line_num);
    line_end = line_start;
    gtk_text_iter_forward_to_line_end(&line_end);

    gchar *line_text = gtk_text_buffer_get_text(buf, &line_start, &line_end, TRUE);

    gchar *dup_text = g_strconcat("\n", line_text, NULL);
    gtk_text_buffer_insert(buf, &line_end, dup_text, -1);

    GtkTextIter final_cursor;
    gtk_text_buffer_get_iter_at_line(buf, &final_cursor, line_num);
    for (int i = 0; i < col; i++) {
        if (gtk_text_iter_ends_line(&final_cursor)) break;
        gtk_text_iter_forward_char(&final_cursor);
    }
    gtk_text_buffer_place_cursor(buf, &final_cursor);

    g_free(line_text);
    g_free(dup_text);

    gtk_text_buffer_end_user_action(buf);
}

void delete_current_line(GtkTextBuffer *buf) {
    GtkTextIter cursor_iter;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor_iter, gtk_text_buffer_get_insert(buf));
    gint line_num = gtk_text_iter_get_line(&cursor_iter);

    gtk_text_buffer_begin_user_action(buf);

    GtkTextIter line_start, line_end;
    gtk_text_buffer_get_iter_at_line(buf, &line_start, line_num);
    line_end = line_start;

    gint total_lines = gtk_text_buffer_get_line_count(buf);
    if (line_num < total_lines - 1) {
        gtk_text_iter_forward_line(&line_end);
        gtk_text_buffer_delete(buf, &line_start, &line_end);
    } else if (line_num > 0) {
        gtk_text_buffer_get_iter_at_line(buf, &line_start, line_num - 1);
        gtk_text_iter_forward_to_line_end(&line_start);
        gtk_text_iter_forward_to_line_end(&line_end);
        gtk_text_buffer_delete(buf, &line_start, &line_end);
    } else {
        gtk_text_iter_forward_to_line_end(&line_end);
        gtk_text_buffer_delete(buf, &line_start, &line_end);
    }

    gtk_text_buffer_end_user_action(buf);
}

void move_current_line(GtkTextBuffer *buf, gboolean up) {
    GtkTextIter cursor_iter;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor_iter, gtk_text_buffer_get_insert(buf));
    gint line_num = gtk_text_iter_get_line(&cursor_iter);
    gint col = gtk_text_iter_get_line_offset(&cursor_iter);
    gint total_lines = gtk_text_buffer_get_line_count(buf);

    if (up && line_num == 0) return;
    if (!up && line_num >= total_lines - 1) return;

    gint target_line = up ? line_num - 1 : line_num + 1;

    gtk_text_buffer_begin_user_action(buf);

    GtkTextIter line1_start, line1_end;
    gtk_text_buffer_get_iter_at_line(buf, &line1_start, up ? target_line : line_num);
    line1_end = line1_start;
    gtk_text_iter_forward_to_line_end(&line1_end);

    GtkTextIter line2_start, line2_end;
    gtk_text_buffer_get_iter_at_line(buf, &line2_start, up ? line_num : target_line);
    line2_end = line2_start;
    gtk_text_iter_forward_to_line_end(&line2_end);

    gchar *line1_text = gtk_text_buffer_get_text(buf, &line1_start, &line1_end, TRUE);
    gchar *line2_text = gtk_text_buffer_get_text(buf, &line2_start, &line2_end, TRUE);

    gtk_text_buffer_delete(buf, &line2_start, &line2_end);
    GtkTextIter insert_iter2;
    gtk_text_buffer_get_iter_at_line(buf, &insert_iter2, up ? line_num : target_line);
    gtk_text_buffer_insert(buf, &insert_iter2, line1_text, -1);

    gtk_text_buffer_get_iter_at_line(buf, &line1_start, up ? target_line : line_num);
    line1_end = line1_start;
    gtk_text_iter_forward_to_line_end(&line1_end);
    gtk_text_buffer_delete(buf, &line1_start, &line1_end);
    GtkTextIter insert_iter1;
    gtk_text_buffer_get_iter_at_line(buf, &insert_iter1, up ? target_line : line_num);
    gtk_text_buffer_insert(buf, &insert_iter1, line2_text, -1);

    GtkTextIter final_cursor;
    gtk_text_buffer_get_iter_at_line(buf, &final_cursor, target_line);
    for (int i = 0; i < col; i++) {
        if (gtk_text_iter_ends_line(&final_cursor)) break;
        gtk_text_iter_forward_char(&final_cursor);
    }
    gtk_text_buffer_place_cursor(buf, &final_cursor);

    g_free(line1_text);
    g_free(line2_text);

    gtk_text_buffer_end_user_action(buf);
}

void gui_manual_save(AppGui *gui) {
    if (!gui) return;
    if (gui->active_tab_index != -1 && strcmp(gui->open_tabs[gui->active_tab_index], "Untitled") == 0) {
        trigger_save_as(gui);
    } else {
        gui_trigger_autosave();
    }
}

void toggle_fullscreen(AppGui *gui) {
    static gboolean is_fullscreen = FALSE;
    extern void zig_set_focus_mode(int enabled);
    extern void apply_focus_mode(AppGui *gui);
    if (is_fullscreen) {
        gtk_window_unfullscreen(GTK_WINDOW(gui->window));
        is_fullscreen = FALSE;
        gui->enable_focus_mode = FALSE;
        zig_set_focus_mode(0);
        apply_focus_mode(gui);
    } else {
        gtk_window_fullscreen(GTK_WINDOW(gui->window));
        is_fullscreen = TRUE;
        gui->enable_focus_mode = TRUE;
        zig_set_focus_mode(1);
        apply_focus_mode(gui);
    }
}

static void apply_paragraph_alignment(GtkTextBuffer *buf, GtkJustification justification) {
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

static void on_paste_plain_text_received(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GdkClipboard *clipboard = GDK_CLIPBOARD(source_object);
    GError *error = NULL;
    char *text = gdk_clipboard_read_text_finish(clipboard, res, &error);
    if (error) {
        g_warning("Failed to read text from clipboard: %s", error->message);
        g_clear_error(&error);
        return;
    }
    if (text) {
        AppGui *gui = (AppGui *)user_data;
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
        GtkTextIter start, end;
        gtk_text_buffer_begin_user_action(buf);
        if (gtk_text_buffer_get_selection_bounds(buf, &start, &end)) {
            gtk_text_buffer_delete(buf, &start, &end);
        }
        gtk_text_buffer_insert_at_cursor(buf, text, -1);
        gtk_text_buffer_end_user_action(buf);
        g_free(text);
    }
}

gboolean on_editor_key_pressed(GtkEventControllerKey *ctrl,
                               guint keyval, guint keycode,
                               GdkModifierType state, gpointer user_data) {
    (void)ctrl;
    AppGui *gui = (AppGui *)user_data;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));

    gboolean ctrl_held  = (state & GDK_CONTROL_MASK) != 0;
    gboolean shift_held = (state & GDK_SHIFT_MASK)   != 0;

    /* ── Bold ── */
    if (match_app_shortcut("bold", keyval, keycode, state)) {
        apply_format(buf, "**", "**");
        return TRUE;
    }
    /* ── Italic ── */
    if (match_app_shortcut("italic", keyval, keycode, state)) {
        apply_format(buf, "*", "*");
        return TRUE;
    }
    /* ── Underline ── */
    if (match_app_shortcut("underline", keyval, keycode, state)) {
        apply_format(buf, "<u>", "</u>");
        return TRUE;
    }
    /* ── Strikethrough ── */
    if (match_app_shortcut("strikethrough", keyval, keycode, state)) {
        apply_format(buf, "~~", "~~");
        return TRUE;
    }
    /* ── Center Align ── */
    if (match_app_shortcut("center_align", keyval, keycode, state)) {
        apply_paragraph_alignment(buf, GTK_JUSTIFY_CENTER);
        return TRUE;
    }
    /* ── Left Align ── */
    if (match_app_shortcut("left_align", keyval, keycode, state)) {
        apply_paragraph_alignment(buf, GTK_JUSTIFY_LEFT);
        return TRUE;
    }
    /* ── Right Align ── */
    if (match_app_shortcut("right_align", keyval, keycode, state)) {
        apply_paragraph_alignment(buf, GTK_JUSTIFY_RIGHT);
        return TRUE;
    }
    /* ── Justify ── */
    if (match_app_shortcut("justify", keyval, keycode, state)) {
        apply_paragraph_alignment(buf, GTK_JUSTIFY_FILL);
        return TRUE;
    }
    /* ── Clear Formatting ── */
    if (match_app_shortcut("clear_format", keyval, keycode, state)) {
        clear_selection_formatting(buf);
        return TRUE;
    }
    /* ── Paste Plain Text ── */
    if (match_app_shortcut("paste_plain", keyval, keycode, state)) {
        GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(gui->source_view));
        gdk_clipboard_read_text_async(clipboard, NULL, on_paste_plain_text_received, gui);
        return TRUE;
    }
    /* ── Duplicate Line ── */
    if (match_app_shortcut("duplicate_line", keyval, keycode, state) ||
        (ctrl_held && (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter))) {
        duplicate_current_line(buf);
        return TRUE;
    }
    /* ── Move Line Up ── */
    if (match_app_shortcut("move_line_up", keyval, keycode, state)) {
        move_current_line(buf, TRUE);
        return TRUE;
    }
    /* ── Move Line Down ── */
    if (match_app_shortcut("move_line_down", keyval, keycode, state)) {
        move_current_line(buf, FALSE);
        return TRUE;
    }
    /* ── Toggle Comment ── */
    if (match_app_shortcut("toggle_comment", keyval, keycode, state)) {
        toggle_comment_current_line(buf);
        return TRUE;
    }
    /* ── Delete Line ── */
    if (match_app_shortcut("delete_line", keyval, keycode, state)) {
        delete_current_line(buf);
        return TRUE;
    }
    /* ── Inline code ── */
    if (match_app_shortcut("inline_code", keyval, keycode, state)) {
        apply_format(buf, "`", "`");
        return TRUE;
    }
    /* ── Highlight ── */
    if (match_app_shortcut("highlight", keyval, keycode, state)) {
        apply_format(buf, "==", "==");
        return TRUE;
    }
    /* ── Blockquote ── */
    if (match_app_shortcut("blockquote", keyval, keycode, state)) {
        apply_paragraph_format(buf, "> ");
        return TRUE;
    }
    /* ── Math ── */
    if (match_app_shortcut("math", keyval, keycode, state)) {
        apply_format(buf, "$", "$");
        return TRUE;
    }
    /* ── Ordered list ── */
    if (match_app_shortcut("ordered_list", keyval, keycode, state)) {
        apply_paragraph_format(buf, "1. ");
        return TRUE;
    }
    /* ── Task list ── */
    if (match_app_shortcut("task_list", keyval, keycode, state)) {
        apply_paragraph_format(buf, "- [ ] ");
        return TRUE;
    }
    /* ── Headings Ctrl+1..6 ── */
    if (ctrl_held && !shift_held) {
        const char *h_prefix = NULL;
        if      (keyval == GDK_KEY_1) h_prefix = "# ";
        else if (keyval == GDK_KEY_2) h_prefix = "## ";
        else if (keyval == GDK_KEY_3) h_prefix = "### ";
        else if (keyval == GDK_KEY_4) h_prefix = "#### ";
        else if (keyval == GDK_KEY_5) h_prefix = "##### ";
        else if (keyval == GDK_KEY_6) h_prefix = "###### ";
        else if (keyval == GDK_KEY_0) h_prefix = ""; /* remove prefix */
        if (h_prefix != NULL) {
            apply_paragraph_format(buf, h_prefix);
            return TRUE;
        }
    }
    /* ── Force Save ── */
    if (match_app_shortcut("save_file", keyval, keycode, state)) {
        gui_manual_save(gui);
        return TRUE;
    }
    /* ── Undo & Redo ── */
    if (match_app_shortcut("undo", keyval, keycode, state)) {
        if (gtk_text_buffer_get_enable_undo(buf)) {
            gtk_text_buffer_undo(buf);
            return TRUE;
        }
    }
    if (match_app_shortcut("redo", keyval, keycode, state) ||
        (ctrl_held && shift_held && (keyval == GDK_KEY_z || keyval == GDK_KEY_Z || keycode_matches_latin_keyval(keycode, GDK_KEY_z)))) {
        if (gtk_text_buffer_get_enable_undo(buf)) {
            gtk_text_buffer_redo(buf);
            return TRUE;
        }
    }
    /* ── Copy, Cut, Paste ── */
    if (match_app_shortcut("copy", keyval, keycode, state)) {
        g_signal_emit_by_name(gui->source_view, "copy-clipboard");
        return TRUE;
    }
    if (match_app_shortcut("cut", keyval, keycode, state)) {
        g_signal_emit_by_name(gui->source_view, "cut-clipboard");
        return TRUE;
    }
    if (match_app_shortcut("paste", keyval, keycode, state)) {
        g_signal_emit_by_name(gui->source_view, "paste-clipboard");
        return TRUE;
    }
    /* ── Select All ── */
    if (match_app_shortcut("select_all", keyval, keycode, state)) {
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds(buf, &start, &end);
        gtk_text_buffer_select_range(buf, &start, &end);
        return TRUE;
    }
    /* ── Delete Word Previous/Next ── */
    if (match_app_shortcut("delete_prev_word", keyval, keycode, state)) {
        GtkTextIter start, end;
        gtk_text_buffer_get_iter_at_mark(buf, &end, gtk_text_buffer_get_insert(buf));
        start = end;
        gtk_text_iter_backward_word_start(&start);
        gtk_text_buffer_delete_interactive(buf, &start, &end, TRUE);
        return TRUE;
    }
    if (match_app_shortcut("delete_next_word", keyval, keycode, state)) {
        GtkTextIter start, end;
        gtk_text_buffer_get_iter_at_mark(buf, &start, gtk_text_buffer_get_insert(buf));
        end = start;
        gtk_text_iter_forward_word_end(&end);
        gtk_text_buffer_delete_interactive(buf, &start, &end, TRUE);
        return TRUE;
    }
    /* ── Cursor Movements ── */
    if (match_app_shortcut("move_word_left", keyval, keycode, state)) {
        GtkTextIter iter;
        gtk_text_buffer_get_iter_at_mark(buf, &iter, gtk_text_buffer_get_insert(buf));
        gtk_text_iter_backward_word_start(&iter);
        gtk_text_buffer_place_cursor(buf, &iter);
        gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(gui->source_view), gtk_text_buffer_get_insert(buf));
        return TRUE;
    }
    if (match_app_shortcut("move_word_right", keyval, keycode, state)) {
        GtkTextIter iter;
        gtk_text_buffer_get_iter_at_mark(buf, &iter, gtk_text_buffer_get_insert(buf));
        gtk_text_iter_forward_word_end(&iter);
        gtk_text_buffer_place_cursor(buf, &iter);
        gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(gui->source_view), gtk_text_buffer_get_insert(buf));
        return TRUE;
    }
    if (match_app_shortcut("move_line_start", keyval, keycode, state)) {
        GtkTextIter iter;
        gtk_text_buffer_get_iter_at_mark(buf, &iter, gtk_text_buffer_get_insert(buf));
        gtk_text_iter_set_line_offset(&iter, 0);
        gtk_text_buffer_place_cursor(buf, &iter);
        gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(gui->source_view), gtk_text_buffer_get_insert(buf));
        return TRUE;
    }
    if (match_app_shortcut("move_line_end", keyval, keycode, state)) {
        GtkTextIter iter;
        gtk_text_buffer_get_iter_at_mark(buf, &iter, gtk_text_buffer_get_insert(buf));
        gtk_text_iter_forward_to_line_end(&iter);
        gtk_text_buffer_place_cursor(buf, &iter);
        gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(gui->source_view), gtk_text_buffer_get_insert(buf));
        return TRUE;
    }
    if (match_app_shortcut("move_doc_start", keyval, keycode, state)) {
        GtkTextIter iter;
        gtk_text_buffer_get_start_iter(buf, &iter);
        gtk_text_buffer_place_cursor(buf, &iter);
        gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(gui->source_view), gtk_text_buffer_get_insert(buf));
        return TRUE;
    }
    if (match_app_shortcut("move_doc_end", keyval, keycode, state)) {
        GtkTextIter iter;
        gtk_text_buffer_get_end_iter(buf, &iter);
        gtk_text_buffer_place_cursor(buf, &iter);
        gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(gui->source_view), gtk_text_buffer_get_insert(buf));
        return TRUE;
    }

    if (!keypress_has_text_modifier(state)) {
        if (keyval == GDK_KEY_parenleft) {
            insert_text_pair(buf, "(", ")");
            return TRUE;
        }
        if (keyval == GDK_KEY_bracketleft) {
            insert_text_pair(buf, "[", "]");
            return TRUE;
        }
        if (keyval == GDK_KEY_braceleft) {
            insert_text_pair(buf, "{", "}");
            return TRUE;
        }
        if (keyval == GDK_KEY_quotedbl) {
            insert_text_pair(buf, "\"", "\"");
            return TRUE;
        }
        if (keyval == GDK_KEY_grave) {
            insert_text_pair(buf, "`", "`");
            return TRUE;
        }
        if (keyval == GDK_KEY_parenright && maybe_skip_closing_pair(buf, ")")) return TRUE;
        if (keyval == GDK_KEY_bracketright && maybe_skip_closing_pair(buf, "]")) return TRUE;
        if (keyval == GDK_KEY_braceright && maybe_skip_closing_pair(buf, "}")) return TRUE;
        if (keyval == GDK_KEY_BackSpace && maybe_delete_empty_pair(buf)) return TRUE;
    }

    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        GtkTextIter insert_iter;
        gtk_text_buffer_get_iter_at_mark(buf, &insert_iter, gtk_text_buffer_get_insert(buf));
        
        GtkTextIter sel_start, sel_end;
        if (gtk_text_buffer_get_selection_bounds(buf, &sel_start, &sel_end)) {
            return FALSE;
        }

        gint line_num = gtk_text_iter_get_line(&insert_iter);
        GtkTextIter line_start;
        gtk_text_buffer_get_iter_at_line(buf, &line_start, line_num);
        
        gchar *line_text = gtk_text_buffer_get_text(buf, &line_start, &insert_iter, TRUE);
        
        int ws_len = 0;
        while (line_text[ws_len] == ' ' || line_text[ws_len] == '\t') {
            ws_len++;
        }
        
        char *content = line_text + ws_len;
        char *bullet_prefix = NULL;
        char prefix_buf[32] = "";
        
        if (strncmp(content, "- [ ] ", 6) == 0 || strncmp(content, "- [x] ", 6) == 0) {
            bullet_prefix = "- [ ] ";
        } else if (strncmp(content, "* [ ] ", 6) == 0 || strncmp(content, "* [x] ", 6) == 0) {
            bullet_prefix = "* [ ] ";
        } else if (strncmp(content, "- ", 2) == 0) {
            bullet_prefix = "- ";
        } else if (strncmp(content, "* ", 2) == 0) {
            bullet_prefix = "* ";
        } else if (strncmp(content, "+ ", 2) == 0) {
            bullet_prefix = "+ ";
        } else {
            int num_len = 0;
            while (content[num_len] >= '0' && content[num_len] <= '9') {
                num_len++;
            }
            if (num_len > 0 && content[num_len] == '.' && content[num_len+1] == ' ') {
                char num_str[16];
                if (num_len < 15) {
                    strncpy(num_str, content, num_len);
                    num_str[num_len] = '\0';
                    int val = atoi(num_str);
                    snprintf(prefix_buf, sizeof(prefix_buf), "%d. ", val + 1);
                    bullet_prefix = prefix_buf;
                }
            }
        }
        
        if (bullet_prefix != NULL) {
            gboolean is_empty_bullet = TRUE;
            char *p = content;
            if (strncmp(p, "- [ ] ", 6) == 0 || strncmp(p, "- [x] ", 6) == 0 || strncmp(p, "* [ ] ", 6) == 0 || strncmp(p, "* [x] ", 6) == 0) {
                is_empty_bullet = (p[6] == '\0' || p[6] == '\n' || p[6] == '\r');
            } else if (strncmp(p, "- ", 2) == 0 || strncmp(p, "* ", 2) == 0 || strncmp(p, "+ ", 2) == 0) {
                is_empty_bullet = (p[2] == '\0' || p[2] == '\n' || p[2] == '\r');
            } else {
                int num_len = 0;
                while (p[num_len] >= '0' && p[num_len] <= '9') num_len++;
                is_empty_bullet = (p[num_len+2] == '\0' || p[num_len+2] == '\n' || p[num_len+2] == '\r');
            }
            
            if (is_empty_bullet) {
                GtkTextIter line_end = insert_iter;
                gtk_text_buffer_delete(buf, &line_start, &line_end);
                gtk_text_buffer_insert(buf, &line_start, "\n", 1);
                g_free(line_text);
                return TRUE;
            }
            
            gchar *indent = g_strndup(line_text, ws_len);
            gchar *newline_and_bullet = g_strconcat("\n", indent, bullet_prefix, NULL);
            
            gtk_text_buffer_insert(buf, &insert_iter, newline_and_bullet, -1);
            
            g_free(indent);
            g_free(newline_and_bullet);
            g_free(line_text);
            return TRUE;
        }
        g_free(line_text);
    }

    return FALSE;
}
