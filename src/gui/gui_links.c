#include "gui_internal.h"
#include <string.h>

/* ── Markdown link rendering and cursor-reveal ──────────────────────────
 * [text](url) pattern:
 *   edit mode  — [ and ](url) concealed; cursor inside reveals raw markdown
 *   read mode  — brackets/URL always concealed; single-click opens URL
 * ─────────────────────────────────────────────────────────────────────── */

static GtkTextTag *link_bracket_tag(GtkTextBuffer *buf) {
    GtkTextTagTable *t = gtk_text_buffer_get_tag_table(buf);
    GtkTextTag *tag = gtk_text_tag_table_lookup(t, "md-link-bracket");
    if (tag) return tag;
    /* Conceal the brackets with scale+transparent ink, NOT the "invisible"
     * property. GTK4's visible-line-index bookkeeping aborts with
     * "Byte index N is off the end of the line" on any line that mixes an
     * invisible segment with multi-byte UTF-8 (Arabic, emoji) the moment a
     * click or cursor move triggers a pixel->iter conversion. The markdown
     * conceal engine hit this and moved to scale 0.01 + transparent
     * foreground (see gui_conceal.c); the link bracket tag must match, or a
     * line carrying both a link and an emoji crashes the app on click. */
    return gtk_text_buffer_create_tag(buf, "md-link-bracket",
                                      "scale", 0.01,
                                      "foreground", "rgba(0,0,0,0)",
                                      NULL);
}

static GtkTextTag *link_text_tag(GtkTextBuffer *buf) {
    GtkTextTagTable *t = gtk_text_buffer_get_tag_table(buf);
    GtkTextTag *tag = gtk_text_tag_table_lookup(t, "md-link-text");
    if (tag) return tag;
    return gtk_text_buffer_create_tag(buf, "md-link-text",
                                      "foreground", "#1565c0",
                                      "underline", PANGO_UNDERLINE_SINGLE, NULL);
}

static GtkTextTag *link_region_tag(GtkTextBuffer *buf) {
    GtkTextTagTable *t = gtk_text_buffer_get_tag_table(buf);
    GtkTextTag *tag = gtk_text_tag_table_lookup(t, "md-link-region");
    if (tag) return tag;
    return gtk_text_buffer_create_tag(buf, "md-link-region", NULL);
}

/* Scan one line for [text](url) and apply/refresh link tags on it.
 * Called by both the full scan and per-line re-conceal after reveal. */
static void scan_line_for_links(GtkTextBuffer *buf, int line_num) {
    GtkTextIter ls, le;
    gtk_text_buffer_get_iter_at_line(buf, &ls, line_num);
    le = ls;
    if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);

    /* Remove existing link tags so brackets become visible before re-search. */
    gtk_text_buffer_remove_tag(buf, link_bracket_tag(buf), &ls, &le);
    gtk_text_buffer_remove_tag(buf, link_text_tag(buf),    &ls, &le);
    gtk_text_buffer_remove_tag(buf, link_region_tag(buf),  &ls, &le);

    /* Re-fetch after tag removal (iterators may be stale). */
    gtk_text_buffer_get_iter_at_line(buf, &ls, line_num);
    le = ls;
    if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);

    GtkTextIter pos = ls;
    while (TRUE) {
        /* Find '[' */
        GtkTextIter ob, oe;
        if (!gtk_text_iter_forward_search(&pos, "[",
                GTK_TEXT_SEARCH_VISIBLE_ONLY, &ob, &oe, &le)) break;

        /* Skip wiki links [[ and footnotes [^ */
        gunichar next_ch = gtk_text_iter_get_char(&oe);
        if (next_ch == '[' || next_ch == '^') { pos = oe; continue; }

        /* Find '](' after the '[' */
        GtkTextIter cb, ce;
        if (!gtk_text_iter_forward_search(&oe, "](",
                GTK_TEXT_SEARCH_VISIBLE_ONLY, &cb, &ce, &le)) { pos = oe; continue; }

        /* Reject empty text '[](' */
        if (gtk_text_iter_equal(&oe, &cb)) { pos = ce; continue; }

        /* Find ')' closing the URL */
        GtkTextIter pb, pe;
        if (!gtk_text_iter_forward_search(&ce, ")",
                GTK_TEXT_SEARCH_VISIBLE_ONLY, &pb, &pe, &le)) { pos = ce; continue; }

        /* Apply tags:
         *   ob..oe : '[' character             → bracket (invisible)
         *   oe..cb : text                       → link-text (colored+underline)
         *   cb..pe : '](url)'                   → bracket (invisible)
         *   ob..pe : whole pattern              → region (for detection)  */
        gtk_text_buffer_apply_tag(buf, link_bracket_tag(buf), &ob, &oe);
        gtk_text_buffer_apply_tag(buf, link_text_tag(buf),    &oe, &cb);
        gtk_text_buffer_apply_tag(buf, link_bracket_tag(buf), &cb, &pe);
        gtk_text_buffer_apply_tag(buf, link_region_tag(buf),  &ob, &pe);

        pos = pe;
    }
}

void parse_and_apply_link_tags(GtkTextBuffer *buf, AppGui *gui) {
    (void)gui;
    int total = gtk_text_buffer_get_line_count(buf);
    for (int line = 0; line < total; line++)
        scan_line_for_links(buf, line);
}

/* ── URL extraction ─────────────────────────────────────────────────── */

static char *extract_url_at_iter(GtkTextBuffer *buf, GtkTextIter *iter) {
    if (!gtk_text_iter_has_tag(iter, link_region_tag(buf))) return NULL;

    GtkTextIter rs = *iter, re = *iter;
    if (!gtk_text_iter_starts_tag(&rs, link_region_tag(buf)))
        gtk_text_iter_backward_to_tag_toggle(&rs, link_region_tag(buf));
    if (!gtk_text_iter_ends_tag(&re, link_region_tag(buf)))
        gtk_text_iter_forward_to_tag_toggle(&re, link_region_tag(buf));

    /* include_hidden=TRUE so we read ](url) even when bracket tag is applied. */
    gchar *full = gtk_text_buffer_get_text(buf, &rs, &re, TRUE);
    if (!full) return NULL;

    const char *bracket = strstr(full, "](");
    if (!bracket) { g_free(full); return NULL; }
    const char *url_start = bracket + 2;
    const char *url_end   = strchr(url_start, ')');
    if (!url_end) { g_free(full); return NULL; }

    char *url = g_strndup(url_start, (gsize)(url_end - url_start));
    g_free(full);
    return url;
}

/* Called from on_editor_left_click. Returns TRUE if the click was a link open. */
gboolean gui_link_handle_click(AppGui *gui, GtkTextIter *iter,
                               int n_press, GdkModifierType state) {
    if (!gui || !gui->source_view) return FALSE;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));

    /* Read mode: single-click. Edit mode: double-click or Ctrl. */
    gboolean should_open = gui->read_mode
        ? (n_press == 1)
        : ((n_press >= 2) || ((state & GDK_CONTROL_MASK) != 0));
    if (!should_open) return FALSE;

    /* Check region tag (covers brackets too) so RTL/invisible-char edge
     * cases don't miss the click when iter lands on an invisible bracket. */
    if (!gtk_text_iter_has_tag(iter, link_region_tag(buf))) return FALSE;

    char *url = extract_url_at_iter(buf, iter);
    if (!url || !url[0]) { g_free(url); return FALSE; }

    g_app_info_launch_default_for_uri(url, NULL, NULL);
    g_free(url);
    return TRUE;
}

/* ── Cursor-reveal (edit mode only) ─────────────────────────────────── */

static GtkTextMark *g_link_revealed_mark = NULL;
static guint        g_link_reconcile_id  = 0;

static gboolean link_reconcile_idle(gpointer data) {
    AppGui *gui = data;
    g_link_reconcile_id = 0;
    if (!gui || !gui->source_view) return G_SOURCE_REMOVE;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));

    GtkTextIter cur;
    gtk_text_buffer_get_iter_at_mark(buf, &cur, gtk_text_buffer_get_insert(buf));
    gboolean cur_in_region = gtk_text_iter_has_tag(&cur, link_region_tag(buf));

    /* Re-conceal the previously-revealed link if cursor left it. */
    if (g_link_revealed_mark) {
        GtkTextIter old_it;
        gtk_text_buffer_get_iter_at_mark(buf, &old_it, g_link_revealed_mark);
        int old_line = gtk_text_iter_get_line(&old_it);

        gboolean still_in_old = cur_in_region &&
                                 gtk_text_iter_get_line(&cur) == old_line;
        gtk_text_buffer_delete_mark(buf, g_link_revealed_mark);
        g_link_revealed_mark = NULL;

        if (!still_in_old) {
            scan_line_for_links(buf, old_line); /* re-applies bracket tags */
            /* Refresh cur_in_region — scan may have changed tags at cursor. */
            gtk_text_buffer_get_iter_at_mark(buf, &cur, gtk_text_buffer_get_insert(buf));
            cur_in_region = gtk_text_iter_has_tag(&cur, link_region_tag(buf));
        } else {
            return G_SOURCE_REMOVE; /* still inside — nothing to do */
        }
    }

    /* In edit mode: reveal the link under the cursor. */
    if (!gui->read_mode && cur_in_region) {
        GtkTextIter rs = cur, re = cur;
        if (!gtk_text_iter_starts_tag(&rs, link_region_tag(buf)))
            gtk_text_iter_backward_to_tag_toggle(&rs, link_region_tag(buf));
        if (!gtk_text_iter_ends_tag(&re, link_region_tag(buf)))
            gtk_text_iter_forward_to_tag_toggle(&re, link_region_tag(buf));

        gtk_text_buffer_remove_tag(buf, link_bracket_tag(buf), &rs, &re);

        if (g_link_revealed_mark) gtk_text_buffer_delete_mark(buf, g_link_revealed_mark);
        g_link_revealed_mark = gtk_text_buffer_create_mark(buf, NULL, &rs, TRUE);
    }

    return G_SOURCE_REMOVE;
}

void gui_links_on_cursor_moved(GtkTextBuffer *buf, AppGui *gui) {
    if (!gui || !gui->source_view) return;
    if (!g_link_revealed_mark) {
        GtkTextIter cur;
        gtk_text_buffer_get_iter_at_mark(buf, &cur, gtk_text_buffer_get_insert(buf));
        if (!gtk_text_iter_has_tag(&cur, link_region_tag(buf))) return;
    }
    if (g_link_reconcile_id == 0)
        g_link_reconcile_id = g_idle_add(link_reconcile_idle, gui);
}

/* Called when entering read mode — cancel any active cursor-reveal. */
void gui_links_on_read_mode_changed(AppGui *gui) {
    if (!gui || !gui->source_view) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));

    if (g_link_reconcile_id) {
        g_source_remove(g_link_reconcile_id);
        g_link_reconcile_id = 0;
    }
    if (g_link_revealed_mark) {
        GtkTextIter old_it;
        gtk_text_buffer_get_iter_at_mark(buf, &old_it, g_link_revealed_mark);
        int old_line = gtk_text_iter_get_line(&old_it);
        gtk_text_buffer_delete_mark(buf, g_link_revealed_mark);
        g_link_revealed_mark = NULL;
        if (gui->read_mode)
            scan_line_for_links(buf, old_line); /* re-conceal on read mode enter */
    }
}

/* Drop reveal state on buffer swap (file load / tab switch). */
void gui_links_reset(GtkTextBuffer *buf) {
    if (g_link_revealed_mark && buf)
        gtk_text_buffer_delete_mark(buf, g_link_revealed_mark);
    g_link_revealed_mark = NULL;
    if (g_link_reconcile_id) {
        g_source_remove(g_link_reconcile_id);
        g_link_reconcile_id = 0;
    }
}
