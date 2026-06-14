#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <string.h>
#include <stdlib.h>
#include <sqlite3.h>
#include "gui_internal.h"

/* Forward declare externally defined helper functions from other modules if needed */
extern void gui_set_text(const char *text, int len);
extern void gui_set_title(const char *title);
extern void gui_trigger_autosave(void);

void duplicate_current_line(GtkTextBuffer *buf) {
    (void)buf;
    GtkTextIter cursor_iter;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor_iter, gtk_text_buffer_get_insert(buf));
    int abs_line = gtk_text_iter_get_line(&cursor_iter);
    int col = gtk_text_iter_get_line_offset(&cursor_iter);

    int len = 0;
    extern const char *zig_get_text_for_line_range(int start_line, int end_line, int *out_len);
    const char *line_text = zig_get_text_for_line_range(abs_line, abs_line + 1, &len);
    if (!line_text) return;

    char *line_text_dup = g_strndup(line_text, len);

    Position insert_pos = { abs_line + 1, 0 };
    zig_insert_text(insert_pos, line_text_dup);
    g_free(line_text_dup);

    gui_reload_full_buffer();
    gui_set_cursor_position(abs_line + 1, col);
    zig_undo_commit();
}

void delete_current_line(GtkTextBuffer *buf) {
    (void)buf;
    GtkTextIter cursor_iter;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor_iter, gtk_text_buffer_get_insert(buf));
    int abs_line = gtk_text_iter_get_line(&cursor_iter);
    int col = gtk_text_iter_get_line_offset(&cursor_iter);

    Position start = { abs_line, 0 };
    Position end = { abs_line + 1, 0 };

    zig_delete_range(start, end);

    gui_reload_full_buffer();
    gui_set_cursor_position(abs_line + 1, col);
    zig_undo_commit();
}

void move_current_line(GtkTextBuffer *buf, gboolean up) {
    (void)buf;
    GtkTextIter cursor_iter;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor_iter, gtk_text_buffer_get_insert(buf));
    int abs_line = gtk_text_iter_get_line(&cursor_iter);
    int col = gtk_text_iter_get_line_offset(&cursor_iter);
    int total_lines = global_gui->document_total_lines;

    if (up && abs_line == 0) return;
    if (!up && abs_line >= total_lines - 1) return;

    int target_line = up ? abs_line - 1 : abs_line + 1;

    extern const char *zig_get_text_for_line_range(int start_line, int end_line, int *out_len);
    int len_curr = 0, len_other = 0;
    const char *curr_text = zig_get_text_for_line_range(abs_line, abs_line + 1, &len_curr);
    const char *other_text = zig_get_text_for_line_range(target_line, target_line + 1, &len_other);

    if (!curr_text || !other_text) return;

    char *curr_dup = g_strndup(curr_text, len_curr);
    char *other_dup = g_strndup(other_text, len_other);

    Position start = { up ? target_line : abs_line, 0 };
    Position end = { up ? abs_line + 1 : target_line + 1, 0 };

    char *combined = up ? g_strconcat(curr_dup, other_dup, NULL) : g_strconcat(other_dup, curr_dup, NULL);

    zig_replace_range(start, end, combined);

    g_free(curr_dup);
    g_free(other_dup);
    g_free(combined);

    gui_reload_full_buffer();
    gui_set_cursor_position(target_line + 1, col);
    zig_undo_commit();
}

void insert_horizontal_rule(GtkTextBuffer *buf) {
    GtkTextIter cursor_iter;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor_iter, gtk_text_buffer_get_insert(buf));
    int abs_line = gtk_text_iter_get_line(&cursor_iter);

    Position insert_pos = { abs_line + 1, 0 };
    zig_insert_text(insert_pos, "---\n");

    gui_reload_full_buffer();
    gui_set_cursor_position(abs_line + 3, 0);
    zig_undo_commit();
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
        if (!gtk_text_buffer_get_selection_bounds(buf, &start, &end)) {
            gtk_text_buffer_get_iter_at_mark(buf, &start, gtk_text_buffer_get_insert(buf));
            end = start;
        }

        Position start_pos = iter_to_position(&start);
        Position end_pos = iter_to_position(&end);
        zig_replace_range(start_pos, end_pos, text);
        gui_reload_full_buffer();

        Position cursor_pos = advance_position(start_pos, text);
        gui_set_cursor_position(cursor_pos.line + 1, cursor_pos.col);
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

    /* ── Toggle read mode ── */
    if (match_app_shortcut("toggle_read_mode", keyval, keycode, state)) {
        toggle_read_mode(gui);
        return TRUE;
    }
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
    /* ── Insert Horizontal Rule ── */
    if (match_app_shortcut("insert_hr", keyval, keycode, state)) {
        insert_horizontal_rule(buf);
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
        zig_undo();
        gtk_text_buffer_set_modified(buf, TRUE);
        return TRUE;
    }
    if (match_app_shortcut("redo", keyval, keycode, state) ||
        (ctrl_held && shift_held && (keyval == GDK_KEY_z || keyval == GDK_KEY_Z || keycode_matches_latin_keyval(keycode, GDK_KEY_z)))) {
        zig_redo();
        gtk_text_buffer_set_modified(buf, TRUE);
        return TRUE;
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

        int start_line = gtk_text_iter_get_line(&start);
        int start_col = gtk_text_iter_get_line_offset(&start);
        int end_line = gtk_text_iter_get_line(&end);
        int end_col = gtk_text_iter_get_line_offset(&end);

        Position p_start = { start_line, start_col };
        Position p_end = { end_line, end_col };
        zig_delete_range(p_start, p_end);

        gui_reload_full_buffer();
        gui_set_cursor_position(start_line + 1, start_col);
        zig_undo_commit();
        return TRUE;
    }
    if (match_app_shortcut("delete_next_word", keyval, keycode, state)) {
        GtkTextIter start, end;
        gtk_text_buffer_get_iter_at_mark(buf, &start, gtk_text_buffer_get_insert(buf));
        end = start;
        gtk_text_iter_forward_word_end(&end);

        int start_line = gtk_text_iter_get_line(&start);
        int start_col = gtk_text_iter_get_line_offset(&start);
        int end_line = gtk_text_iter_get_line(&end);
        int end_col = gtk_text_iter_get_line_offset(&end);

        Position p_start = { start_line, start_col };
        Position p_end = { end_line, end_col };
        zig_delete_range(p_start, p_end);

        gui_reload_full_buffer();
        gui_set_cursor_position(start_line + 1, start_col);
        zig_undo_commit();
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

    /* ── List indent / outdent with Tab ── */
    if (keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) {
        gboolean outdent = (keyval == GDK_KEY_ISO_Left_Tab) || shift_held;
        GtkTextIter ins_it;
        gtk_text_buffer_get_iter_at_mark(buf, &ins_it, gtk_text_buffer_get_insert(buf));
        gint line_num = gtk_text_iter_get_line(&ins_it);
        gint col = gtk_text_iter_get_line_offset(&ins_it);
        GtkTextIter ls, le;
        gtk_text_buffer_get_iter_at_line(buf, &ls, line_num);
        le = ls;
        if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
        gchar *line_text = gtk_text_buffer_get_text(buf, &ls, &le, TRUE);

        int lt_ws = 0;
        while (line_text[lt_ws] == ' ' || line_text[lt_ws] == '\t') lt_ws++;
        const char *lt_content = line_text + lt_ws;
        gboolean is_list = (strncmp(lt_content, "- ", 2) == 0 ||
                            strncmp(lt_content, "* ", 2) == 0 ||
                            strncmp(lt_content, "+ ", 2) == 0);
        if (!is_list) {
            int nd = 0;
            while (lt_content[nd] >= '0' && lt_content[nd] <= '9') nd++;
            if (nd > 0 && lt_content[nd] == '.' && lt_content[nd + 1] == ' ') is_list = TRUE;
        }

        if (is_list) {
            if (!outdent) {
                Position p = { line_num, 0 };
                zig_insert_text(p, "  ");
                gui_reload_full_buffer();
                gui_set_cursor_position(line_num + 1, col + 2);
                zig_undo_commit();
            } else if (lt_ws > 0) {
                int remove = lt_ws >= 2 ? 2 : 1;
                Position p0 = { line_num, 0 };
                Position p1 = { line_num, remove };
                zig_delete_range(p0, p1);
                gui_reload_full_buffer();
                int new_col = col - remove;
                if (new_col < 0) new_col = 0;
                gui_set_cursor_position(line_num + 1, new_col);
                zig_undo_commit();
            }
            g_free(line_text);
            return TRUE;
        }
        g_free(line_text);
    }

    if (!keypress_has_text_modifier(state)) {
        /* ── Wrap selection instead of replacing it ──
         * ( [ { " ` already wrap via insert_text_pair below; * and _
         * normally route through on_insert_text_before, which replaces. */
        {
            GtkTextIter sel_s, sel_e;
            if (gtk_text_buffer_get_selection_bounds(buf, &sel_s, &sel_e)) {
                if (keyval == GDK_KEY_asterisk) {
                    insert_text_pair(buf, "*", "*");
                    return TRUE;
                }
                if (keyval == GDK_KEY_underscore) {
                    insert_text_pair(buf, "_", "_");
                    return TRUE;
                }
            }
        }
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
            
            /* Edit the GTK buffer directly: the on_insert/on_delete signals
             * sync the change to Zig, so we avoid the full gui_reload_full_buffer
             * (which made every list Enter jump/shiver the whole document). */
            if (is_empty_bullet) {
                /* Empty item + Enter = leave the list: strip the marker, land
                 * the cursor on the now-blank line. */
                gtk_text_buffer_delete(buf, &line_start, &insert_iter);
                gtk_text_buffer_place_cursor(buf, &line_start);
                zig_undo_commit();
                g_free(line_text);
                return TRUE;
            }

            gchar *indent = g_strndup(line_text, ws_len);
            gchar *newline_and_bullet = g_strconcat("\n", indent, bullet_prefix, NULL);
            gtk_text_buffer_insert(buf, &insert_iter, newline_and_bullet, -1);
            /* gtk_text_buffer_insert advances the insert mark past the new text,
             * so the cursor already sits after the bullet. */
            zig_undo_commit();

            g_free(indent);
            g_free(newline_and_bullet);
            g_free(line_text);
            return TRUE;
        }
        g_free(line_text);
    }

    /* Word-grain undo: seal the pending snapshot on boundaries (space,
     * Enter, punctuation, deletions), not on every keystroke — Ctrl+Z then
     * removes a word/phrase, not one character. zig_undo() also seals
     * before undoing, so nothing typed since the last boundary is lost. */
    {
        gboolean boundary = (keyval == GDK_KEY_space || keyval == GDK_KEY_Return ||
                             keyval == GDK_KEY_KP_Enter || keyval == GDK_KEY_Tab ||
                             keyval == GDK_KEY_BackSpace || keyval == GDK_KEY_Delete);
        if (!boundary) {
            gunichar uc = gdk_keyval_to_unicode(keyval);
            if (uc != 0 && g_unichar_ispunct(uc)) boundary = TRUE;
        }
        if (boundary) zig_undo_commit();
    }
    return FALSE;
}
