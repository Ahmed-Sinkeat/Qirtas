#include "gui_internal.h"
#include <string.h>

/* ============================================================
 * BUFFER / UNDO CORE
 *
 * Insert/delete/replace signal wiring, undo push/commit glue,
 * the per-keystroke -> per-pause debounce chain (word/char stats,
 * conceal pass, autosave), and bracket/quote pairing + comment
 * toggling that ride the same insert/delete signals.
 * ============================================================ */

extern const char *zig_get_text_for_line_range(int start_line, int end_line, int *out_len);

void gui_push_undo_snapshot(void) {
    if (!global_gui || global_gui->loading_viewport) return;

    int line = 0;
    int col = 0;
    gui_get_cursor_position(&line, &col);
    zig_undo_push(line, col);
}

void gui_set_buffer_modified(gboolean modified) {
    if (!global_source_view) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));
    gtk_text_buffer_set_modified(buf, modified);
}

int gui_get_buffer_modified(void) {
    if (!global_source_view) return 0;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));
    return gtk_text_buffer_get_modified(buf) ? 1 : 0;
}

/* Convert ASCII digits to Eastern Arabic numerals (٠١٢٣…). */
void arabize_digits(const char *in, char *out, size_t out_size) {
    size_t o = 0;
    for (const char *p = in; *p != '\0' && o + 3 < out_size; p++) {
        if (*p >= '0' && *p <= '9') {
            out[o++] = (char)0xD9;
            out[o++] = (char)(0xA0 + (*p - '0'));
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

/* Render a non-negative count as Eastern Arabic numerals. */
static void arabic_number(long n, char *out, size_t out_size) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%ld", n);
    arabize_digits(tmp, out, out_size);
}

/* Grammatically correct Arabic count phrase (تمييز العدد). Numbers 3-10 follow
 * gender polarity (feminine noun → number without ة). n < 0 marks the
 * "too big to count" case (—). Caller passes the four noun forms. */
static void arabic_count_forms(long n, gboolean feminine,
                               const char *singular, const char *one,
                               const char *two, const char *plural,
                               char *out, size_t out_size) {
    static const char *fem_3_10[] = { "", "", "", "ثلاث", "أربع", "خمس",
                                      "ست", "سبع", "ثماني", "تسع", "عشر" };
    static const char *mas_3_10[] = { "", "", "", "ثلاثة", "أربعة", "خمسة",
                                      "ستة", "سبعة", "ثمانية", "تسعة", "عشرة" };
    if (n < 0)  { snprintf(out, out_size, "— %s", singular); return; }
    if (n == 1) { snprintf(out, out_size, "%s", one); return; }
    if (n == 2) { snprintf(out, out_size, "%s", two); return; }
    if (n >= 3 && n <= 10) {
        snprintf(out, out_size, "%s %s", feminine ? fem_3_10[n] : mas_3_10[n], plural);
        return;
    }
    /* 0, then 11 and up: Eastern digits + singular (تمييز مفرد منصوب). */
    char d[32];
    arabic_number(n, d, sizeof(d));
    snprintf(out, out_size, "%s %s", d, singular);
}

/*  كلمة (word, feminine): 1 كلمة واحدة، 2 كلمتان، 3-10 ست كلمات، 11+ ٢٠ كلمة
 *  حرف  (char, masculine): 1 حرف واحد، 2 حرفان، 3-10 ستة حروف، 11+ ٢٠ حرف
 *  سطر  (line, masculine): 1 سطر واحد، 2 سطران، 3-10 ثلاثة أسطر، 11+ ٢٠ سطر */
void arabic_count_phrase(long n, gboolean feminine, char *out, size_t out_size) {
    if (feminine)
        arabic_count_forms(n, TRUE, "كلمة", "كلمة واحدة", "كلمتان", "كلمات", out, out_size);
    else
        arabic_count_forms(n, FALSE, "حرف", "حرف واحد", "حرفان", "حروف", out, out_size);
}

void arabic_lines_phrase(long n, char *out, size_t out_size) {
    arabic_count_forms(n, FALSE, "سطر", "سطر واحد", "سطران", "أسطر", out, out_size);
}

/* Position <-> GtkTextIter helpers, shared by the formatting transforms
 * below — all of them edit through Zig's Position-addressed buffer API
 * then reload and re-select. */
Position iter_to_position(GtkTextIter *iter) {
    Position pos = { 0, 0 };
    if (!global_gui || !iter) return pos;
    pos.line = gtk_text_iter_get_line(iter);
    pos.col = gtk_text_iter_get_line_offset(iter);
    return pos;
}

Position advance_position(Position pos, const char *text) {
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

void select_position_range(AppGui *gui, Position start, Position end) {
    if (!gui || !gui->source_view) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
    GtkTextIter start_iter, end_iter;

    int rel_start_line = start.line;
    int rel_end_line = end.line;
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

static guint buffer_stats_timeout_id = 0;
static guint autosave_debounce_id = 0;

/* Edits live in RAM only (see update_document_content_in_memory in main.zig),
 * so save 2.5s after typing stops. The 30s autosave thread stays as the
 * backstop for continuous typers. */
static gboolean autosave_debounce_cb(gpointer user_data) {
    (void)user_data;
    autosave_debounce_id = 0;
    gui_trigger_autosave();
    return G_SOURCE_REMOVE;
}

static gboolean buffer_stats_timeout_cb(gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    buffer_stats_timeout_id = 0;
    if (!gui || !gui->source_view) return G_SOURCE_REMOVE;
    QIRTAS_PERF_BEGIN;

    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));

    /* Char count is free (buffer-maintained); only the word count needs a
     * full text copy, so skip it past 500k chars. */
    glong char_count = (glong)gtk_text_buffer_get_char_count(buf);

    glong line_count = (glong)gtk_text_buffer_get_line_count(buf);

    glong word_count = -1; /* -1 = skipped (document too large) */
    if (char_count <= 500000) {
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds(buf, &start, &end);
        gchar *text = gtk_text_buffer_get_text(buf, &start, &end, TRUE);
        word_count = 0;
        gboolean in_word = FALSE;
        char *p = text;
        while (*p) {
            gunichar c = g_utf8_get_char(p);
            if (g_unichar_isspace(c) || g_unichar_ispunct(c)) {
                in_word = FALSE;
            } else {
                if (!in_word) { word_count++; in_word = TRUE; }
            }
            p = g_utf8_next_char(p);
        }
        g_free(text);
    }

    if (qirtas_app_language == 1) {
        /* Arabic: grammatical count phrases (كلمة fem, حرف/سطر masc). */
        char w_ar[96], c_ar[96], l_ar[96];
        arabic_count_phrase((long)word_count, TRUE, w_ar, sizeof(w_ar));
        arabic_count_phrase((long)char_count, FALSE, c_ar, sizeof(c_ar));
        arabic_lines_phrase((long)line_count, l_ar, sizeof(l_ar));
        if (gui->lbl_words) gtk_label_set_text(GTK_LABEL(gui->lbl_words), w_ar);
        if (gui->lbl_chars) gtk_label_set_text(GTK_LABEL(gui->lbl_chars), c_ar);
        if (gui->lbl_lines) gtk_label_set_text(GTK_LABEL(gui->lbl_lines), l_ar);
    } else {
        char w_buf[64], c_buf[64], l_buf[64];
        if (word_count < 0) snprintf(w_buf, sizeof(w_buf), "— %s", qirtas_tr("words"));
        else                snprintf(w_buf, sizeof(w_buf), "%ld %s", word_count, qirtas_tr("words"));
        snprintf(c_buf, sizeof(c_buf), "%ld %s", char_count, qirtas_tr("chars"));
        snprintf(l_buf, sizeof(l_buf), "%ld %s", line_count, qirtas_tr("lines"));
        if (gui->lbl_words) gtk_label_set_text(GTK_LABEL(gui->lbl_words), w_buf);
        if (gui->lbl_chars) gtk_label_set_text(GTK_LABEL(gui->lbl_chars), c_buf);
        if (gui->lbl_lines) gtk_label_set_text(GTK_LABEL(gui->lbl_lines), l_buf);
    }

    /* Full conceal pass runs here (already deferred past the 'changed'
     * signal, so Pango's line-layout cache is stable — avoids the
     * "Byte index N is off the end of the line" abort). */
    update_conceal_markdown_all(buf);
    gui_outline_refresh(gui);

    /* Typing pause = word-grain undo boundary. No-op if nothing pending. */
    zig_undo_commit();
    QIRTAS_PERF_END("buffer_stats_timeout_cb");
    return G_SOURCE_REMOVE;
}

void on_buffer_changed(GtkTextBuffer *buf, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (gui && gui->loading_viewport) return;

    check_and_insert_hr(buf, gui);

    /* Word/char count + full-buffer conceal are O(document) — debounce
     * so fast typing in big files costs one pass per pause, not one per
     * keystroke. Local conceal around the cursor still runs instantly
     * via on_mark_set. */
    if (buffer_stats_timeout_id) g_source_remove(buffer_stats_timeout_id);
    buffer_stats_timeout_id = g_timeout_add(220, buffer_stats_timeout_cb, gui);

    if (autosave_debounce_id) g_source_remove(autosave_debounce_id);
    autosave_debounce_id = g_timeout_add(2500, autosave_debounce_cb, NULL);
}

void on_insert_text_before(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (gui && gui->loading_viewport) return;
    if (len != 1) return;

    char c = text[0];
    char closing = 0;
    if (c == '"') closing = '"';
    else if (c == '[') closing = ']';
    else if (c == '(') closing = ')';
    else if (c == '{') closing = '}';
    else if (c == '*') closing = '*';
    else if (c == '_') closing = '_';
    else if (c == '`') closing = '`';

    if (closing != 0) {
        GtkTextIter next_iter = *location;
        gunichar next_char = gtk_text_iter_get_char(&next_iter);
        if ((c == '"' || c == '*' || c == '_' || c == '`') && (gunichar)c == next_char) {
            gtk_text_iter_forward_char(&next_iter);
            gtk_text_buffer_place_cursor(buf, &next_iter);
            g_signal_stop_emission_by_name(buf, "insert-text");
            return;
        }

        gchar both_chars[3] = { c, closing, 0 };
        Position pos = iter_to_position(location);
        zig_insert_text(pos, both_chars);
        gui_reload_full_buffer();
        gui_set_cursor_position(pos.line + 1, pos.col + 1);
        zig_undo_commit();
        g_signal_stop_emission_by_name(buf, "insert-text");
    } else if (c == '"' || c == ']' || c == ')' || c == '}' || c == '*' || c == '_' || c == '`') {
        GtkTextIter next_iter = *location;
        gunichar next_char = gtk_text_iter_get_char(&next_iter);
        if ((gunichar)c == next_char) {
            gtk_text_iter_forward_char(&next_iter);
            gtk_text_buffer_place_cursor(buf, &next_iter);
            g_signal_stop_emission_by_name(buf, "insert-text");
            return;
        }
    }
}

void clear_selection_formatting(GtkTextBuffer *buf) {
    GtkTextIter start, end;
    if (!gtk_text_buffer_get_selection_bounds(buf, &start, &end)) {
        return;
    }

    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buf);
    GtkTextTag *tag_left = gtk_text_tag_table_lookup(table, "align-left");
    GtkTextTag *tag_center = gtk_text_tag_table_lookup(table, "align-center");
    GtkTextTag *tag_right = gtk_text_tag_table_lookup(table, "align-right");
    GtkTextTag *tag_justify = gtk_text_tag_table_lookup(table, "align-justify");
    if (tag_left) gtk_text_buffer_remove_tag(buf, tag_left, &start, &end);
    if (tag_center) gtk_text_buffer_remove_tag(buf, tag_center, &start, &end);
    if (tag_right) gtk_text_buffer_remove_tag(buf, tag_right, &start, &end);
    if (tag_justify) gtk_text_buffer_remove_tag(buf, tag_justify, &start, &end);

    gchar *text = gtk_text_buffer_get_text(buf, &start, &end, TRUE);
    if (text && strlen(text) > 0) {
        GString *cleaned = g_string_new("");
        gchar *p = text;
        while (*p) {
            if (strncmp(p, "**", 2) == 0) {
                p += 2;
            } else if (strncmp(p, "~~", 2) == 0) {
                p += 2;
            } else if (strncmp(p, "==", 2) == 0) {
                p += 2;
            } else if (strncmp(p, "<u>", 3) == 0) {
                p += 3;
            } else if (strncmp(p, "</u>", 4) == 0) {
                p += 4;
            } else if (*p == '*' || *p == '`' || *p == '$') {
                p++;
            } else {
                g_string_append_c(cleaned, *p);
                p++;
            }
        }

        Position start_pos = iter_to_position(&start);
        Position end_pos = iter_to_position(&end);
        zig_replace_range(start_pos, end_pos, cleaned->str);
        gui_reload_full_buffer();

        Position select_end = advance_position(start_pos, cleaned->str);
        select_position_range(global_gui, start_pos, select_end);
        zig_undo_commit();

        g_string_free(cleaned, TRUE);
    }
    g_free(text);
}

void insert_text_pair(GtkTextBuffer *buf, const char *open, const char *close) {
    GtkTextIter start, end;
    AppGui *gui = global_gui;

    if (gtk_text_buffer_get_selection_bounds(buf, &start, &end)) {
        gchar *selected = gtk_text_buffer_get_text(buf, &start, &end, TRUE);
        gchar *wrapped = g_strconcat(open, selected, close, NULL);
        Position start_pos = iter_to_position(&start);
        Position end_pos = iter_to_position(&end);
        zig_replace_range(start_pos, end_pos, wrapped);
        gui_reload_full_buffer();
        select_position_range(gui, start_pos, advance_position(start_pos, wrapped));
        zig_undo_commit();

        g_free(selected);
        g_free(wrapped);
    } else {
        GtkTextIter cursor;
        gtk_text_buffer_get_iter_at_mark(buf, &cursor, gtk_text_buffer_get_insert(buf));
        gchar *pair = g_strconcat(open, close, NULL);
        Position cursor_pos = iter_to_position(&cursor);
        zig_insert_text(cursor_pos, pair);
        gui_reload_full_buffer();
        Position after_open = advance_position(cursor_pos, open);
        gui_set_cursor_position(after_open.line + 1, after_open.col);
        zig_undo_commit();
        g_free(pair);
    }
}

gboolean maybe_skip_closing_pair(GtkTextBuffer *buf, const char *close) {
    GtkTextIter cursor, next;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor, gtk_text_buffer_get_insert(buf));
    next = cursor;
    if (!gtk_text_iter_forward_chars(&next, g_utf8_strlen(close, -1))) return FALSE;

    gchar *next_text = gtk_text_buffer_get_text(buf, &cursor, &next, TRUE);
    gboolean matches = (strcmp(next_text, close) == 0);
    g_free(next_text);

    if (matches) {
        gtk_text_buffer_place_cursor(buf, &next);
        return TRUE;
    }
    return FALSE;
}

gboolean maybe_delete_empty_pair(GtkTextBuffer *buf) {
    GtkTextIter cursor, prev, next;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor, gtk_text_buffer_get_insert(buf));

    prev = cursor;
    next = cursor;
    if (!gtk_text_iter_backward_char(&prev) || !gtk_text_iter_forward_char(&next)) return FALSE;

    gchar *around = gtk_text_buffer_get_text(buf, &prev, &next, TRUE);
    gboolean is_pair =
        strcmp(around, "()") == 0 ||
        strcmp(around, "[]") == 0 ||
        strcmp(around, "{}") == 0 ||
        strcmp(around, "\"\"") == 0 ||
        strcmp(around, "``") == 0;

    if (is_pair) {
        Position start_pos = iter_to_position(&prev);
        Position end_pos = iter_to_position(&next);
        zig_delete_range(start_pos, end_pos);
        gui_reload_full_buffer();
        gui_set_cursor_position(start_pos.line + 1, start_pos.col);
        zig_undo_commit();
    }

    g_free(around);
    return is_pair;
}

void toggle_comment_current_line(GtkTextBuffer *buf) {
    (void)buf;
    GtkTextIter cursor_iter;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor_iter, gtk_text_buffer_get_insert(buf));
    int abs_line = gtk_text_iter_get_line(&cursor_iter);
    int col = gtk_text_iter_get_line_offset(&cursor_iter);

    int len = 0;
    const char *line_text = zig_get_text_for_line_range(abs_line, abs_line + 1, &len);
    if (!line_text) return;

    char *line_dup = g_strndup(line_text, len);

    int ws = 0;
    while (line_dup[ws] == ' ' || line_dup[ws] == '\t') ws++;

    char *content = line_dup + ws;
    gboolean is_commented = FALSE;
    size_t content_len = strlen(content);
    while (content_len > 0 && (content[content_len - 1] == '\n' || content[content_len - 1] == '\r')) {
        content[content_len - 1] = '\0';
        content_len--;
    }

    if (strncmp(content, "<!--", 4) == 0) {
        if (content_len >= 7 && strcmp(content + content_len - 3, "-->") == 0) {
            is_commented = TRUE;
        }
    }

    char *new_text = NULL;
    if (is_commented) {
        char *inner = g_strndup(content + 4, content_len - 7);
        char *start_p = inner;
        if (start_p[0] == ' ') start_p++;
        size_t inner_len = strlen(start_p);
        if (inner_len > 0 && start_p[inner_len - 1] == ' ') {
            start_p[inner_len - 1] = '\0';
        }
        char *indent = g_strndup(line_dup, ws);
        new_text = g_strconcat(indent, start_p, "\n", NULL);
        g_free(inner);
        g_free(indent);
    } else {
        char *indent = g_strndup(line_dup, ws);
        new_text = g_strconcat(indent, "<!-- ", content, " -->\n", NULL);
        g_free(indent);
    }

    Position start = { abs_line, 0 };
    Position end = { abs_line + 1, 0 };
    zig_replace_range(start, end, new_text);
    g_free(line_dup);
    g_free(new_text);

    // Reload viewport
    gui_reload_full_buffer();

    // Place cursor
    gui_set_cursor_position(abs_line + 1, col);
    zig_undo_commit();
}

void on_insert_text_after(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (gui && gui->loading_viewport) return;

    int end_offset = gtk_text_iter_get_offset(location);
    int char_len = g_utf8_strlen(text, len);
    int start_offset = end_offset - char_len;
    if (start_offset < 0) start_offset = 0;

    GtkTextIter start_iter;
    gtk_text_buffer_get_iter_at_offset(buf, &start_iter, start_offset);

    int start_line = gtk_text_iter_get_line(&start_iter);
    int start_col = gtk_text_iter_get_line_offset(&start_iter);

    Position pos = { start_line, start_col };
    gchar *text_dup = g_strndup(text, len);
    zig_insert_text(pos, text_dup);

    /* Word-grain undo: mark uncommitted edits, seal when a boundary char
     * (space / newline / punct) is typed. Idle seal in the stats debounce
     * covers pauses. */
    gui_push_undo_snapshot();
    gboolean boundary = FALSE;
    for (const char *q = text_dup; *q; q = g_utf8_next_char(q)) {
        gunichar uc = g_utf8_get_char(q);
        if (g_unichar_isspace(uc) || g_unichar_ispunct(uc)) { boundary = TRUE; break; }
    }
    if (boundary) zig_undo_commit();
    g_free(text_dup);

    gui_set_sync_state(QIRTAS_SYNC_NOT_SYNCED);
    update_paragraph_direction_lines(buf, start_line, gtk_text_iter_get_line(location));

    apply_wiki_link_tags_local(buf);
}

void on_delete_range_before(GtkTextBuffer *buf, GtkTextIter *start, GtkTextIter *end, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (gui && gui->loading_viewport) return;

    int start_line = gtk_text_iter_get_line(start);
    int start_col = gtk_text_iter_get_line_offset(start);

    int end_line = gtk_text_iter_get_line(end);
    int end_col = gtk_text_iter_get_line_offset(end);

    Position p_start = { start_line, start_col };
    Position p_end = { end_line, end_col };

    zig_delete_range(p_start, p_end);
    gui_push_undo_snapshot();
}

void on_delete_range_after(GtkTextBuffer *buf, GtkTextIter *start, GtkTextIter *end, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (gui && gui->loading_viewport) return;
    (void)end;
    gui_set_sync_state(QIRTAS_SYNC_NOT_SYNCED);
    update_paragraph_direction_lines(buf, gtk_text_iter_get_line(start), gtk_text_iter_get_line(start));
    apply_wiki_link_tags_local(buf);
}
