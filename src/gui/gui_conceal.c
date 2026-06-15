#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <string.h>
#include "gui_internal.h"

extern void gui_tabs_refresh(AppGui *gui);

static int get_line_height(GtkWidget *text_view) {
    PangoContext *ctx = gtk_widget_get_pango_context(text_view);
    PangoFontMetrics *metrics = pango_context_get_metrics(ctx, NULL, NULL);
    int pango_height = (pango_font_metrics_get_ascent(metrics) + pango_font_metrics_get_descent(metrics)) / PANGO_SCALE;
    pango_font_metrics_unref(metrics);

    PangoLayout *layout = gtk_widget_create_pango_layout(text_view, "Ag");
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    pango_layout_set_width(layout, -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    int width = 0;
    pango_layout_get_pixel_size(layout, &width, NULL);
    g_object_unref(layout);

    int above = gtk_text_view_get_pixels_above_lines(GTK_TEXT_VIEW(text_view));
    int below = gtk_text_view_get_pixels_below_lines(GTK_TEXT_VIEW(text_view));
    return pango_height + above + below;
}

gboolean idle_scroll_to_cursor(gpointer user_data) {
    ScrollToCursorData *d = (ScrollToCursorData *)user_data;
    if (d->gui) d->gui->scroll_queued = FALSE;
    if (d->gui && d->generation != d->gui->buffer_generation) {
        g_free(d);
        return G_SOURCE_REMOVE;
    }
    AppGui *gui = d->gui;
    gint offset = d->offset;
    g_free(d);

    if (!gui || !gui->source_view)
        return G_SOURCE_REMOVE;
    if (gui->primary_button_down && !gui->mouse_dragging)
        return G_SOURCE_REMOVE;
    if (!gtk_widget_get_realized(gui->source_view))
        return G_SOURCE_REMOVE;

    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));

    /* offset == -1 means "scroll to wherever the insert mark is NOW" —
     * always current even if more cursor moves landed since queuing. */
    if (offset < 0) {
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(gui->source_view),
                                     gtk_text_buffer_get_insert(buf),
                                     0.12, FALSE, 0.0, 0.0);
        return G_SOURCE_REMOVE;
    }

    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_offset(buf, &iter, offset);

    /* The source view is a native scrollable now — let GtkTextView do
     * the scroll math. within_margin keeps a cushion of context around
     * the cursor (old behavior: 3–5 lines). */
    GtkTextMark *mark = gtk_text_buffer_create_mark(buf, NULL, &iter, FALSE);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(gui->source_view), mark,
                                 0.12, FALSE, 0.0, 0.0);
    gtk_text_buffer_delete_mark(buf, mark);
    return G_SOURCE_REMOVE;
}

/* Paragraph base direction by FIRST STRONG character of the CONTENT —
 * leading markdown syntax (#, -, *, +, >, digits, checkbox brackets,
 * whitespace) is neutral and must be skipped, otherwise "# عنوان" or
 * "- عنصر" latches onto the '#'/'-' and renders the Arabic line LTR.
 * Previously this returned RTL if ANY Arabic char appeared anywhere in
 * the line, which broke mixed lines like "see ملاحظة for details". */
static gboolean detect_rtl(const gchar *text) {
    if (!text) return FALSE;
    const gchar *p = text;

    /* skip leading markdown structure */
    while (*p) {
        gunichar c = g_utf8_get_char(p);
        if (c == ' ' || c == '\t' || c == '#' || c == '>' ||
            c == '-' || c == '*' || c == '+' ||
            c == '[' || c == ']' || c == '.' || c == ')' ||
            (c >= '0' && c <= '9')) {
            p = g_utf8_next_char(p);
            continue;
        }
        /* checkbox "- [x] " — the x is syntax, not content */
        if ((c == 'x' || c == 'X') && p[1] == ']') {
            p = g_utf8_next_char(p);
            continue;
        }
        break;
    }

    /* first strong directional character decides */
    for (; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if ((c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFEFC))
            return TRUE; /* Hebrew/Arabic blocks → RTL */
        if (g_unichar_isalpha(c))
            return FALSE; /* any other letter → LTR */
    }
    return FALSE;
}

static void update_paragraph_direction(GtkTextBuffer *buf, GtkTextIter *iter) {
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buf);
    GtkTextTag *rtl_tag = gtk_text_tag_table_lookup(table, "rtl-tag");
    if (!rtl_tag) {
        rtl_tag = gtk_text_buffer_create_tag(buf, "rtl-tag", "direction", GTK_TEXT_DIR_RTL, NULL);
    }
    GtkTextTag *ltr_tag = gtk_text_tag_table_lookup(table, "ltr-tag");
    if (!ltr_tag) {
        ltr_tag = gtk_text_buffer_create_tag(buf, "ltr-tag", "direction", GTK_TEXT_DIR_LTR, NULL);
    }

    gint line_num = gtk_text_iter_get_line(iter);
    GtkTextIter start;
    gtk_text_buffer_get_iter_at_line(buf, &start, line_num);
    GtkTextIter end = start;
    if (!gtk_text_iter_ends_line(&end)) {
        gtk_text_iter_forward_to_line_end(&end);
    }
    gchar *text = gtk_text_iter_get_text(&start, &end);
    if (text) {
        if (detect_rtl(text)) {
            gtk_text_buffer_remove_tag(buf, ltr_tag, &start, &end);
            gtk_text_buffer_apply_tag(buf, rtl_tag, &start, &end);
        } else {
            gtk_text_buffer_remove_tag(buf, rtl_tag, &start, &end);
            gtk_text_buffer_apply_tag(buf, ltr_tag, &start, &end);
        }
        g_free(text);
    }
}

/* Compile each conceal pattern at most once, ever. The conceal pass runs a
 * fixed set of ~10 patterns; compiling them fresh per call (the old `else`
 * branch did) showed up as avoidable cost in the full-buffer pass. Cache is
 * keyed by pattern content and never freed — the pattern set is closed and
 * lives for the whole process. */
static GRegex *get_cached_regex(const gchar *pattern) {
    enum { MAX_CACHED = 16 };
    static const gchar *keys[MAX_CACHED];
    static GRegex      *vals[MAX_CACHED];
    static int          count = 0;

    for (int i = 0; i < count; i++) {
        if (strcmp(keys[i], pattern) == 0) return vals[i];
    }
    GRegex *re = g_regex_new(pattern, G_REGEX_OPTIMIZE, 0, NULL);
    if (re && count < MAX_CACHED) {
        keys[count] = g_strdup(pattern);
        vals[count] = re;
        count++;
    }
    return re;
}

/* Some lines make GTK's text layout abort when conceal (scale) tags are applied
 * over them — inline HTML, tables, and very long lines (seen with the
 * markdown-test-file). Concealing markdown markers inside those isn't meaningful
 * anyway, so skip them: the markers render raw but the editor stays crash-safe.
 * Keyed off the buffer line containing `char_off`. */
static gboolean conceal_line_hostile(GtkTextBuffer *buf, gint char_off) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf, &it, char_off);
    gint line = gtk_text_iter_get_line(&it);
    GtkTextIter ls, le;
    gtk_text_buffer_get_iter_at_line(buf, &ls, line);
    le = ls;
    if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
    if (gtk_text_iter_get_offset(&le) - gtk_text_iter_get_offset(&ls) > 2000) return TRUE;
    gchar *t = gtk_text_buffer_get_text(buf, &ls, &le, TRUE);
    gboolean hostile = t && (strchr(t, '<') != NULL || strchr(t, '|') != NULL);
    g_free(t);
    return hostile;
}

static void apply_regex_conceal(GtkTextBuffer *buf, const gchar *text, const gchar *pattern, gint cursor_char, gint delim_len, GtkTextTag *conceal_tag) {
    GRegex *regex = get_cached_regex(pattern);
    if (!regex) return;

    GError *error = NULL;
    GMatchInfo *match_info = NULL;
    gboolean has_match = g_regex_match(regex, text, 0, &match_info);
    while (has_match) {
        gint start_byte = 0;
        gint end_byte = 0;
        if (g_match_info_fetch_pos(match_info, 0, &start_byte, &end_byte)) {
            gint start_char = g_utf8_pointer_to_offset(text, text + start_byte);
            gint end_char = g_utf8_pointer_to_offset(text, text + end_byte);
            gboolean cursor_inside = (cursor_char >= start_char && cursor_char <= end_char);
            if (!cursor_inside && !conceal_line_hostile(buf, start_char)) {
                GtkTextIter start_iter, end_iter;
                gtk_text_buffer_get_iter_at_offset(buf, &start_iter, start_char);
                gtk_text_buffer_get_iter_at_offset(buf, &end_iter, start_char + delim_len);
                gtk_text_buffer_apply_tag(buf, conceal_tag, &start_iter, &end_iter);

                gtk_text_buffer_get_iter_at_offset(buf, &start_iter, end_char - delim_len);
                gtk_text_buffer_get_iter_at_offset(buf, &end_iter, end_char);
                gtk_text_buffer_apply_tag(buf, conceal_tag, &start_iter, &end_iter);
            }
        }
        has_match = g_match_info_next(match_info, &error);
    }
    g_match_info_free(match_info);
}

static void apply_regex_conceal_local(GtkTextBuffer *buf, const gchar *text, gint range_start_offset, const gchar *pattern, gint cursor_char, gint delim_len, GtkTextTag *conceal_tag) {
    GRegex *regex = get_cached_regex(pattern);
    if (!regex) return;

    GError *error = NULL;
    GMatchInfo *match_info = NULL;
    gboolean has_match = g_regex_match(regex, text, 0, &match_info);
    while (has_match) {
        gint start_byte = 0;
        gint end_byte = 0;
        if (g_match_info_fetch_pos(match_info, 0, &start_byte, &end_byte)) {
            gint start_char_local = g_utf8_pointer_to_offset(text, text + start_byte);
            gint end_char_local = g_utf8_pointer_to_offset(text, text + end_byte);

            gint start_char = range_start_offset + start_char_local;
            gint end_char = range_start_offset + end_char_local;
            gboolean cursor_inside = (cursor_char >= start_char && cursor_char <= end_char);
            if (!cursor_inside && !conceal_line_hostile(buf, start_char)) {
                gint total_chars = gtk_text_buffer_get_char_count(buf);

                gint c_start1 = start_char;
                if (c_start1 < 0) c_start1 = 0;
                if (c_start1 > total_chars) c_start1 = total_chars;

                gint c_end1 = start_char + delim_len;
                if (c_end1 < 0) c_end1 = 0;
                if (c_end1 > total_chars) c_end1 = total_chars;

                gint c_start2 = end_char - delim_len;
                if (c_start2 < 0) c_start2 = 0;
                if (c_start2 > total_chars) c_start2 = total_chars;

                gint c_end2 = end_char;
                if (c_end2 < 0) c_end2 = 0;
                if (c_end2 > total_chars) c_end2 = total_chars;

                GtkTextIter start_iter, end_iter;
                gtk_text_buffer_get_iter_at_offset(buf, &start_iter, c_start1);
                gtk_text_buffer_get_iter_at_offset(buf, &end_iter, c_end1);
                gtk_text_buffer_apply_tag(buf, conceal_tag, &start_iter, &end_iter);

                gtk_text_buffer_get_iter_at_offset(buf, &start_iter, c_start2);
                gtk_text_buffer_get_iter_at_offset(buf, &end_iter, c_end2);
                gtk_text_buffer_apply_tag(buf, conceal_tag, &start_iter, &end_iter);
            }
        }
        has_match = g_match_info_next(match_info, &error);
    }
    g_match_info_free(match_info);
}

static gboolean local_conceal_queued = FALSE;
static gboolean global_conceal_queued = FALSE;

/* NOTE: conceal must NOT use the "invisible" tag property. GTK4's
 * visible-line-index bookkeeping is broken for lines mixing invisible
 * segments with multi-byte UTF-8 (Arabic!) — internal pixel->iter
 * conversions (mouse clicks, vertical cursor motion) then abort with
 * "Byte index N is off the end of the line". Shrinking to 1% scale with
 * fully transparent ink hides the markers without creating invisible
 * text, so that GTK code path is never taken. */
static void update_conceal_markdown_all_impl(GtkTextBuffer *buf) {
    if (global_gui && global_gui->in_conceal_update) return;
    if (global_gui) global_gui->in_conceal_update = TRUE;

    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buf);
    GtkTextTag *conceal_tag = gtk_text_tag_table_lookup(table, "conceal");
    if (!conceal_tag) {
        conceal_tag = gtk_text_buffer_create_tag(buf, "conceal",
                                                 "scale", 0.01,
                                                 "foreground", "rgba(0,0,0,0)",
                                                 NULL);
    }
    /* Conceal must outrank heading/syntax tags or their scale wins. Only
     * reassign when actually wrong — set_priority dirties the buffer, and this
     * runs on every cursor move / edit, so skipping the no-op avoids needless
     * redraw churn. */
    {
        const int want = gtk_text_tag_table_get_size(table) - 1;
        if (gtk_text_tag_get_priority(conceal_tag) != want)
            gtk_text_tag_set_priority(conceal_tag, want);
    }
    GtkTextTag *h1_tag = gtk_text_buffer_get_tag_table(buf);
    GtkTextTag *h1_tag_lookup = gtk_text_tag_table_lookup(table, "heading1");
    if (!h1_tag_lookup) h1_tag_lookup = gtk_text_buffer_create_tag(buf, "heading1", "scale", 2.0, NULL);
    GtkTextTag *h2_tag_lookup = gtk_text_tag_table_lookup(table, "heading2");
    if (!h2_tag_lookup) h2_tag_lookup = gtk_text_buffer_create_tag(buf, "heading2", "scale", 1.6, NULL);
    GtkTextTag *h3_tag_lookup = gtk_text_tag_table_lookup(table, "heading3");
    if (!h3_tag_lookup) h3_tag_lookup = gtk_text_buffer_create_tag(buf, "heading3", "scale", 1.3, NULL);
    GtkTextTag *h4_tag_lookup = gtk_text_tag_table_lookup(table, "heading4");
    if (!h4_tag_lookup) h4_tag_lookup = gtk_text_buffer_create_tag(buf, "heading4", "scale", 1.1, NULL);

    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    gtk_text_buffer_remove_tag(buf, conceal_tag, &start, &end);
    gtk_text_buffer_remove_tag(buf, h1_tag_lookup, &start, &end);
    gtk_text_buffer_remove_tag(buf, h2_tag_lookup, &start, &end);
    gtk_text_buffer_remove_tag(buf, h3_tag_lookup, &start, &end);
    gtk_text_buffer_remove_tag(buf, h4_tag_lookup, &start, &end);

    gchar *text = gtk_text_buffer_get_text(buf, &start, &end, TRUE);
    if (!text || strlen(text) == 0) {
        g_free(text);
        if (global_gui) global_gui->in_conceal_update = FALSE;
        return;
    }

    GtkTextMark *insert_mark = gtk_text_buffer_get_insert(buf);
    GtkTextIter cursor_iter;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor_iter, insert_mark);
    gint cursor_char = gtk_text_iter_get_offset(&cursor_iter);

    /* Read mode: no caret to reveal syntax markers near, so conceal
     * everything unconditionally — pass a cursor position outside the
     * buffer so apply_regex_conceal's cursor_inside check never fires. */
    if (global_gui && global_gui->read_mode) cursor_char = -1;

    apply_regex_conceal(buf, text, "\\*\\*([^\\n]*?[^\\n\\*][^\\n]*?)\\*\\*", cursor_char, 2, conceal_tag);
    apply_regex_conceal(buf, text, "==([^\\n]*?[^\\n=][^\\n]*?)==", cursor_char, 2, conceal_tag);
    apply_regex_conceal(buf, text, "(?<!\\*)\\*([^\\n\\*]+?)\\*(?!\\*)", cursor_char, 1, conceal_tag);

    /* Heading markers: match and conceal the # characters and the trailing space */
    apply_regex_conceal(buf, text, "(?m)^#[ \\t]", cursor_char, 2, conceal_tag);
    apply_regex_conceal(buf, text, "(?m)^##[ \\t]", cursor_char, 3, conceal_tag);
    apply_regex_conceal(buf, text, "(?m)^###[ \\t]", cursor_char, 4, conceal_tag);
    apply_regex_conceal(buf, text, "(?m)^####[ \\t]", cursor_char, 5, conceal_tag);
    apply_regex_conceal(buf, text, "(?m)^#####[ \\t]", cursor_char, 6, conceal_tag);
    apply_regex_conceal(buf, text, "(?m)^######[ \\t]", cursor_char, 7, conceal_tag);

    /* Inline links: match and conceal opening [ and closing ](url) */
    apply_regex_conceal(buf, text, "\\[([^\\]]+)\\]\\([^)]*\\)", cursor_char, 1, conceal_tag);

    /* Images: match and conceal entire match */
    apply_regex_conceal(buf, text, "!\\[[^\\]]*\\]\\([^)]*\\)", cursor_char, 2, conceal_tag);

    g_free(text);

    /* Heading size tags pass */
    int line_count = gtk_text_buffer_get_line_count(buf);
    for (int i = 0; i < line_count; i++) {
        GtkTextIter line_start, line_end;
        gtk_text_buffer_get_iter_at_line(buf, &line_start, i);
        line_end = line_start;
        gtk_text_iter_forward_to_line_end(&line_end);
        
        gchar *line_text = gtk_text_buffer_get_text(buf, &line_start, &line_end, TRUE);
        if (line_text) {
            int h_level = 0;
            while (line_text[h_level] == '#') {
                h_level++;
            }
            if (h_level > 0 && (line_text[h_level] == ' ' || line_text[h_level] == '\0') &&
                !strchr(line_text, '<') && !strchr(line_text, '|')) {
                if (h_level == 1) {
                    gtk_text_buffer_apply_tag(buf, h1_tag_lookup, &line_start, &line_end);
                } else if (h_level == 2) {
                    gtk_text_buffer_apply_tag(buf, h2_tag_lookup, &line_start, &line_end);
                } else if (h_level == 3) {
                    gtk_text_buffer_apply_tag(buf, h3_tag_lookup, &line_start, &line_end);
                } else if (h_level >= 4) {
                    gtk_text_buffer_apply_tag(buf, h4_tag_lookup, &line_start, &line_end);
                }
            }
            g_free(line_text);
        }
    }

    if (global_gui) global_gui->in_conceal_update = FALSE;
}

typedef struct {
    AppGui *gui;
    guint generation;
} ConcealData;

static gboolean idle_global_conceal_cb(gpointer user_data) {
    ConcealData *d = user_data;
    global_conceal_queued = FALSE;
    if (d->generation != d->gui->buffer_generation) {
        g_free(d);
        return G_SOURCE_REMOVE;
    }

    if (d->gui && global_source_view) {
        QIRTAS_PERF_BEGIN;
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));
        update_conceal_markdown_all_impl(buf);
        QIRTAS_PERF_END("idle_global_conceal_cb");
    }
    g_free(d);

    return G_SOURCE_REMOVE;
}

/* Synchronous full conceal pass — used by toggle_read_mode where the
 * caller needs the new conceal state to land immediately (before the
 * scroll-position restore runs), not on the next idle cycle. */
void update_conceal_markdown_all_sync(GtkTextBuffer *buf) {
    update_conceal_markdown_all_impl(buf);
}

void update_conceal_markdown_all(GtkTextBuffer *buf) {
    (void)buf;
    if (qirtas_no_conceal) return;
    if (global_conceal_queued) return;
    if (!global_gui) return;
    
    global_conceal_queued = TRUE;
    ConcealData *d = g_new(ConcealData, 1);
    d->gui = global_gui;
    d->generation = global_gui->buffer_generation;
    g_idle_add(idle_global_conceal_cb, d);
}

/* Reconceal exactly the line span [first_line, last_line] (inclusive),
 * clamped to the buffer. The single O(N) full-buffer pass lives in
 * update_conceal_markdown_all_impl; this is what the edit path and the
 * cursor-move path use so cost scales with the edit, not the document. */
static void update_conceal_markdown_range_impl(GtkTextBuffer *buf, int first_line, int last_line) {
    if (global_gui && global_gui->in_conceal_update) return;
    if (global_gui) global_gui->in_conceal_update = TRUE;

    GtkTextMark *insert_mark = gtk_text_buffer_get_insert(buf);

    int total_lines = gtk_text_buffer_get_line_count(buf);

    int start_line = first_line;
    if (start_line < 0) start_line = 0;
    int end_line = last_line;
    if (end_line >= total_lines) end_line = total_lines - 1;
    if (end_line < start_line) {
        if (global_gui) global_gui->in_conceal_update = FALSE;
        return;
    }

    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_line(buf, &start, start_line);
    gtk_text_buffer_get_iter_at_line(buf, &end, end_line);
    gtk_text_iter_forward_to_line_end(&end);

    gchar *text = gtk_text_buffer_get_text(buf, &start, &end, TRUE);
    if (!text || strlen(text) == 0) {
        g_free(text);
        if (global_gui) global_gui->in_conceal_update = FALSE;
        return;
    }

    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buf);
    GtkTextTag *conceal_tag = gtk_text_tag_table_lookup(table, "conceal");
    if (!conceal_tag) {
        conceal_tag = gtk_text_buffer_create_tag(buf, "conceal",
                                                 "scale", 0.01,
                                                 "foreground", "rgba(0,0,0,0)",
                                                 NULL);
    }
    /* Conceal must outrank heading/syntax tags or their scale wins. Only
     * reassign when actually wrong — set_priority dirties the buffer and this
     * runs on every cursor move / edit. */
    {
        const int want = gtk_text_tag_table_get_size(table) - 1;
        if (gtk_text_tag_get_priority(conceal_tag) != want)
            gtk_text_tag_set_priority(conceal_tag, want);
    }
    GtkTextTag *h1_tag = gtk_text_tag_table_lookup(table, "heading1");
    if (!h1_tag) h1_tag = gtk_text_buffer_create_tag(buf, "heading1", "scale", 2.0, NULL);
    GtkTextTag *h2_tag = gtk_text_tag_table_lookup(table, "heading2");
    if (!h2_tag) h2_tag = gtk_text_buffer_create_tag(buf, "heading2", "scale", 1.6, NULL);
    GtkTextTag *h3_tag = gtk_text_tag_table_lookup(table, "heading3");
    if (!h3_tag) h3_tag = gtk_text_buffer_create_tag(buf, "heading3", "scale", 1.3, NULL);
    GtkTextTag *h4_tag = gtk_text_tag_table_lookup(table, "heading4");
    if (!h4_tag) h4_tag = gtk_text_buffer_create_tag(buf, "heading4", "scale", 1.1, NULL);

    gtk_text_buffer_get_iter_at_line(buf, &start, start_line);
    gtk_text_buffer_get_iter_at_line(buf, &end, end_line);
    gtk_text_iter_forward_to_line_end(&end);

    gtk_text_buffer_remove_tag(buf, conceal_tag, &start, &end);
    gtk_text_buffer_remove_tag(buf, h1_tag, &start, &end);
    gtk_text_buffer_remove_tag(buf, h2_tag, &start, &end);
    gtk_text_buffer_remove_tag(buf, h3_tag, &start, &end);
    gtk_text_buffer_remove_tag(buf, h4_tag, &start, &end);

    if (!strchr(text, '*') && !strchr(text, '=') && !strchr(text, '#') &&
        !strchr(text, '[') && !strchr(text, ']') && !strchr(text, '(') &&
        !strchr(text, ')') && !strchr(text, '!')) {
        g_free(text);
        if (global_gui) global_gui->in_conceal_update = FALSE;
        return;
    }

    gint range_start_offset = gtk_text_iter_get_offset(&start);
    
    GtkTextIter current_cursor;
    gtk_text_buffer_get_iter_at_mark(buf, &current_cursor, insert_mark);
    gint cursor_char = gtk_text_iter_get_offset(&current_cursor);

    if (global_gui && global_gui->read_mode) cursor_char = -1;

    apply_regex_conceal_local(buf, text, range_start_offset, "\\*\\*([^\\n]*?[^\\n\\*][^\\n]*?)\\*\\*", cursor_char, 2, conceal_tag);
    apply_regex_conceal_local(buf, text, range_start_offset, "==([^\\n]*?[^\\n=][^\\n]*?)==", cursor_char, 2, conceal_tag);
    apply_regex_conceal_local(buf, text, range_start_offset, "(?<!\\*)\\*([^\\n\\*]+?)\\*(?!\\*)", cursor_char, 1, conceal_tag);

    apply_regex_conceal_local(buf, text, range_start_offset, "(?m)^#[ \\t]", cursor_char, 2, conceal_tag);
    apply_regex_conceal_local(buf, text, range_start_offset, "(?m)^##[ \\t]", cursor_char, 3, conceal_tag);
    apply_regex_conceal_local(buf, text, range_start_offset, "(?m)^###[ \\t]", cursor_char, 4, conceal_tag);
    apply_regex_conceal_local(buf, text, range_start_offset, "(?m)^####[ \\t]", cursor_char, 5, conceal_tag);
    apply_regex_conceal_local(buf, text, range_start_offset, "(?m)^#####[ \\t]", cursor_char, 6, conceal_tag);
    apply_regex_conceal_local(buf, text, range_start_offset, "(?m)^######[ \\t]", cursor_char, 7, conceal_tag);

    apply_regex_conceal_local(buf, text, range_start_offset, "\\[([^\\]]+)\\]\\([^)]*\\)", cursor_char, 1, conceal_tag);
    apply_regex_conceal_local(buf, text, range_start_offset, "!\\[[^\\]]*\\]\\([^)]*\\)", cursor_char, 2, conceal_tag);

    g_free(text);

    for (int i = start_line; i <= end_line; i++) {
        GtkTextIter line_start, line_end;
        gtk_text_buffer_get_iter_at_line(buf, &line_start, i);
        line_end = line_start;
        gtk_text_iter_forward_to_line_end(&line_end);
        
        gchar *line_text = gtk_text_buffer_get_text(buf, &line_start, &line_end, TRUE);
        if (line_text) {
            int h_level = 0;
            while (line_text[h_level] == '#') {
                h_level++;
            }
            if (h_level > 0 && (line_text[h_level] == ' ' || line_text[h_level] == '\0') &&
                !strchr(line_text, '<') && !strchr(line_text, '|')) {
                if (h_level == 1) {
                    gtk_text_buffer_apply_tag(buf, h1_tag, &line_start, &line_end);
                } else if (h_level == 2) {
                    gtk_text_buffer_apply_tag(buf, h2_tag, &line_start, &line_end);
                } else if (h_level == 3) {
                    gtk_text_buffer_apply_tag(buf, h3_tag, &line_start, &line_end);
                } else if (h_level >= 4) {
                    gtk_text_buffer_apply_tag(buf, h4_tag, &line_start, &line_end);
                }
            }
            g_free(line_text);
        }
    }

    if (global_gui) global_gui->in_conceal_update = FALSE;
}

/* Public ranged reconceal — used by the edit-debounce path to touch only the
 * lines that actually changed instead of the whole document. */
void update_conceal_markdown_range(GtkTextBuffer *buf, int first_line, int last_line) {
    if (qirtas_no_conceal) return;
    update_conceal_markdown_range_impl(buf, first_line, last_line);
}

/* Cursor-move reconceal: the line under the caret plus one on each side, so
 * syntax markers reveal/hide as the caret enters/leaves them. */
static void update_conceal_markdown_impl(GtkTextBuffer *buf) {
    GtkTextMark *insert_mark = gtk_text_buffer_get_insert(buf);
    GtkTextIter cursor_iter;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor_iter, insert_mark);
    int cursor_line = gtk_text_iter_get_line(&cursor_iter);
    update_conceal_markdown_range_impl(buf, cursor_line - 1, cursor_line + 1);
}

static gboolean idle_local_conceal_cb(gpointer user_data) {
    ConcealData *d = user_data;
    local_conceal_queued = FALSE;
    if (d->generation != d->gui->buffer_generation) {
        g_free(d);
        return G_SOURCE_REMOVE;
    }

    if (d->gui && global_source_view) {
        QIRTAS_PERF_BEGIN;
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));
        update_conceal_markdown_impl(buf);
        QIRTAS_PERF_END("idle_local_conceal_cb");
    }
    g_free(d);

    return G_SOURCE_REMOVE;
}

void update_conceal_markdown(GtkTextBuffer *buf) {
    (void)buf;
    if (qirtas_no_conceal) return;
    if (local_conceal_queued) return;
    if (!global_gui) return;
    
    local_conceal_queued = TRUE;
    ConcealData *d = g_new(ConcealData, 1);
    d->gui = global_gui;
    d->generation = global_gui->buffer_generation;
    g_idle_add(idle_local_conceal_cb, d);
}

void on_buffer_modified_changed(GtkTextBuffer *buf, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (gui && gui->tabs.active != -1 && gui->tabs.active < gui->tabs.count) {
        gboolean is_modified = gtk_text_buffer_get_modified(buf);
        gui->tabs.modified[gui->tabs.active] = is_modified;
        gui_tabs_refresh(gui);
    }
}

void on_mark_set(GtkTextBuffer *buf, GtkTextIter *location, GtkTextMark *mark, gpointer user_data) {
    (void)location;
    AppGui *gui = (AppGui *)user_data;
    if (gui->in_scroll_update) return;
    if (!gui->source_view) return;

    GtkTextMark *insert_mark = gtk_text_buffer_get_insert(buf);

    if (mark == insert_mark) {
        cursor_trail_wake(gui);
        /* Defer tag updates to idle so Pango layout is stable and doesn't get stale byte-index cache. */
        update_conceal_markdown(buf);

        if (!gtk_widget_get_realized(gui->source_view)) return;
        if (gui->primary_button_down && !gui->mouse_dragging) return;
        if (gui->loading_viewport) return;
        if (gui->scroll_queued) return;

        /* Defer to a high-priority idle: mark-set fires while the text
         * btree is mid-mutation; scrolling synchronously here validates
         * layout against a stale byte index and aborts with
         * "Byte index N is off the end of the line". HIGH_IDLE+15 runs
         * after GTK layout (+10) but before paint (+20), so the
         * viewport still moves in the same frame the caret is drawn. */
        gui->scroll_queued = TRUE;
        ScrollToCursorData *d = g_new(ScrollToCursorData, 1);
        d->gui = gui;
        d->offset = -1; /* idle scrolls to the live insert mark */
        d->generation = gui->buffer_generation;
        g_idle_add_full(G_PRIORITY_HIGH_IDLE + 15, idle_scroll_to_cursor, d, NULL);
    }
}

void on_cursor_position_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    (void)object;
    (void)pspec;
    (void)user_data;
}
