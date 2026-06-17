#include "gui_internal.h"
#include <string.h>

/* Markdown tables rendered as a grid, reveal-on-cursor. Same view-only model as
 * gui_codeblock.c / gui_hr.c: the GtkTextBuffer view is decorated while the Zig
 * doc_buf keeps the real "| a | b |" markdown, so files save unchanged.
 *
 * A rendered table = the header line's text replaced by a GtkGrid child anchor,
 * the remaining table lines (delimiter + body) hidden with a transparent tag,
 * and the whole line range tagged "md-table-region" for cursor detection and
 * idempotency. The original markdown is stashed on the anchor so the table can
 * be restored verbatim when the cursor enters it (reveal), then re-rendered when
 * the cursor leaves. Line count stays stable (no lines added/removed) so the
 * doc_buf<->view mirroring in the buffer signal handlers never desyncs. */

extern void on_insert_text_before(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data);
extern void on_insert_text_after(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data);
extern void on_delete_range_after(GtkTextBuffer *buf, GtkTextIter *start, GtkTextIter *end, gpointer user_data);
extern void on_buffer_changed(GtkTextBuffer *buf, gpointer user_data);
/* mark-set MUST be blocked too: our delete/insert moves the cursor mark, which
 * fires mark-set synchronously mid-mutation; on_mark_set then runs the conceal
 * pass (and this module's own cursor handler) on a half-edited buffer with stale
 * iterators — segfault. Block it for the duration of every structural change. */
extern void on_mark_set(GtkTextBuffer *buf, GtkTextIter *location, GtkTextMark *mark, gpointer user_data);

/* The single table currently un-rendered because the cursor is inside it (or
 * NULL). A mark on its first line so we can re-render exactly that table when
 * the cursor leaves — no full-document rescan on cursor moves. */
static GtkTextMark *g_revealed_start = NULL;
static guint g_table_reconcile_id = 0;

/* ---- parsing helpers ---------------------------------------------------- */

/* Split a markdown table row into trimmed cell strings. Optional leading and
 * trailing pipes are dropped. (Escaped \| inside cells is not handled in v1.) */
static GPtrArray *split_row(const char *line) {
    GPtrArray *cells = g_ptr_array_new_with_free_func(g_free);
    char *dup = g_strdup(line);
    g_strstrip(dup);
    char *s = dup;
    if (*s == '|') s++;
    size_t L = strlen(s);
    if (L > 0 && s[L - 1] == '|') s[L - 1] = '\0';
    char **parts = g_strsplit(s, "|", -1);
    for (int i = 0; parts[i]; i++) {
        char *c = g_strdup(parts[i]);
        g_strstrip(c);
        g_ptr_array_add(cells, c);
    }
    g_strfreev(parts);
    g_free(dup);
    return cells;
}

/* Table structure detection moved to src/markdown.zig (shared + unit-tested);
 * these stay as thin C glue. split_row (cell TEXT extraction for the widget)
 * remains C — it returns a GLib string array, see docs/PORTABILITY.md. */
static gboolean is_delimiter_row(const char *line) {
    return zig_table_is_delimiter(line);
}

static gboolean is_table_row(const char *line) {
    return zig_table_is_row(line);
}

/* Per-column GtkAlign from the delimiter row's colons (codes via Zig). */
static void parse_alignments(const char *delim, GtkAlign *aligns, int max_cols) {
    for (int i = 0; i < max_cols; i++) aligns[i] = GTK_ALIGN_START;
    if (max_cols <= 0) return;
    int *codes = g_new0(int, max_cols);
    int n = zig_table_aligns(delim, codes, max_cols);
    for (int i = 0; i < n && i < max_cols; i++) {
        aligns[i] = (codes[i] == 1) ? GTK_ALIGN_CENTER
                  : (codes[i] == 2) ? GTK_ALIGN_END
                                    : GTK_ALIGN_START;
    }
    g_free(codes);
}

/* ---- line / buffer helpers ---------------------------------------------- */

static char *line_text(GtkTextBuffer *buf, int line) {
    GtkTextIter s, e;
    gtk_text_buffer_get_iter_at_line(buf, &s, line);
    e = s;
    if (!gtk_text_iter_ends_line(&e)) gtk_text_iter_forward_to_line_end(&e);
    return gtk_text_buffer_get_text(buf, &s, &e, TRUE);
}

static GtkTextTag *table_hide_tag(GtkTextBuffer *buf) {
    GtkTextTagTable *tab = gtk_text_buffer_get_tag_table(buf);
    GtkTextTag *t = gtk_text_tag_table_lookup(tab, "md-table-hidden");
    if (t) return t;
    GdkRGBA clear = { 0, 0, 0, 0 };
    return gtk_text_buffer_create_tag(buf, "md-table-hidden",
                                      "invisible", TRUE,
                                      "scale", 0.001, "foreground-rgba", &clear, NULL);
}

/* Marks the whole rendered line range; used for cursor-in-table detection and
 * to skip already-rendered tables on a re-scan. */
static GtkTextTag *table_region_tag(GtkTextBuffer *buf) {
    GtkTextTagTable *tab = gtk_text_buffer_get_tag_table(buf);
    GtkTextTag *t = gtk_text_tag_table_lookup(tab, "md-table-region");
    if (t) return t;
    return gtk_text_buffer_create_tag(buf, "md-table-region", NULL);
}

/* ---- rendering ---------------------------------------------------------- */

static GtkWidget *build_table_grid(GtkTextBuffer *buf, int header, int last) {
    /* rows = header + body (delimiter row at header+1 is skipped). */
    char *delim = line_text(buf, header + 1);

    char *hdr_txt = line_text(buf, header);
    GPtrArray *hdr = split_row(hdr_txt);
    g_free(hdr_txt);
    int max_cols = (int)hdr->len;
    for (int r = header + 2; r <= last; r++) {
        char *lt = line_text(buf, r);
        GPtrArray *c = split_row(lt);
        if ((int)c->len > max_cols) max_cols = (int)c->len;
        g_ptr_array_free(c, TRUE);
        g_free(lt);
    }
    if (max_cols < 1) max_cols = 1;

    GtkAlign *aligns = g_new(GtkAlign, max_cols);
    parse_alignments(delim, aligns, max_cols);
    g_free(delim);

    GtkWidget *grid = gtk_grid_new();
    gtk_widget_add_css_class(grid, "md-table");
    gtk_widget_set_hexpand(grid, TRUE);

    int grid_row = 0;
    /* header row */
    for (int c = 0; c < max_cols; c++) {
        const char *txt = (c < (int)hdr->len) ? g_ptr_array_index(hdr, c) : "";
        GtkWidget *cell = gtk_label_new(txt);
        gtk_widget_add_css_class(cell, "md-table-cell");
        gtk_widget_add_css_class(cell, "md-table-header");
        gtk_widget_set_halign(cell, aligns[c]);
        gtk_widget_set_hexpand(cell, TRUE);
        gtk_grid_attach(GTK_GRID(grid), cell, c, grid_row, 1, 1);
    }
    grid_row++;
    g_ptr_array_free(hdr, TRUE);

    /* body rows */
    for (int r = header + 2; r <= last; r++) {
        char *lt = line_text(buf, r);
        GPtrArray *cells = split_row(lt);
        g_free(lt);
        for (int c = 0; c < max_cols; c++) {
            const char *txt = (c < (int)cells->len) ? g_ptr_array_index(cells, c) : "";
            GtkWidget *cell = gtk_label_new(txt);
            gtk_widget_add_css_class(cell, "md-table-cell");
            gtk_widget_set_halign(cell, aligns[c]);
            gtk_widget_set_hexpand(cell, TRUE);
            gtk_grid_attach(GTK_GRID(grid), cell, c, grid_row, 1, 1);
        }
        grid_row++;
        g_ptr_array_free(cells, TRUE);
    }

    g_free(aligns);
    return grid;
}

/* Render the table spanning lines [header..last] in place. */
static void render_one_table(GtkTextBuffer *buf, AppGui *gui, int header, int last) {
    /* Stash the verbatim markdown for reveal/restore. */
    GString *raw = g_string_new("");
    for (int r = header; r <= last; r++) {
        char *lt = line_text(buf, r);
        g_string_append(raw, lt);
        if (r < last) g_string_append_c(raw, '\n');
        g_free(lt);
    }

    GtkWidget *grid = build_table_grid(buf, header, last);

    /* Replace the header line's text with the grid anchor (keeps the line). */
    GtkTextIter hs, he;
    gtk_text_buffer_get_iter_at_line(buf, &hs, header);
    he = hs;
    if (!gtk_text_iter_ends_line(&he)) gtk_text_iter_forward_to_line_end(&he);
    gtk_text_buffer_delete(buf, &hs, &he);

    GtkTextIter at;
    gtk_text_buffer_get_iter_at_line(buf, &at, header);
    GtkTextChildAnchor *anchor = gtk_text_child_anchor_new();
    gtk_text_buffer_insert_child_anchor(buf, &at, anchor);
    g_object_set_data_full(G_OBJECT(anchor), "table-md", g_string_free(raw, FALSE), g_free);
    gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(gui->source_view), grid, anchor);

    /* Hide the delimiter + body lines. */
    GtkTextTag *hide = table_hide_tag(buf);
    GtkTextIter bs, be;
    gtk_text_buffer_get_iter_at_line(buf, &bs, header + 1);
    gtk_text_buffer_get_iter_at_line(buf, &be, last);
    if (!gtk_text_iter_ends_line(&be)) gtk_text_iter_forward_to_line_end(&be);
    gtk_text_buffer_apply_tag(buf, hide, &bs, &be);

    /* Tag the whole region (header anchor line .. last) for cursor detection. */
    GtkTextTag *region = table_region_tag(buf);
    GtkTextIter rs, re;
    gtk_text_buffer_get_iter_at_line(buf, &rs, header);
    gtk_text_buffer_get_iter_at_line(buf, &re, last);
    if (!gtk_text_iter_ends_line(&re)) gtk_text_iter_forward_to_line_end(&re);
    gtk_text_buffer_apply_tag(buf, region, &rs, &re);
}

static void block_handlers(GtkTextBuffer *buf, AppGui *gui) {
    g_signal_handlers_block_by_func(buf, on_insert_text_before, gui);
    g_signal_handlers_block_by_func(buf, on_insert_text_after, gui);
    g_signal_handlers_block_by_func(buf, on_delete_range_after, gui);
    g_signal_handlers_block_by_func(buf, on_buffer_changed, gui);
    g_signal_handlers_block_by_func(buf, on_mark_set, gui);
}
static void unblock_handlers(GtkTextBuffer *buf, AppGui *gui) {
    g_signal_handlers_unblock_by_func(buf, on_insert_text_before, gui);
    g_signal_handlers_unblock_by_func(buf, on_insert_text_after, gui);
    g_signal_handlers_unblock_by_func(buf, on_delete_range_after, gui);
    g_signal_handlers_unblock_by_func(buf, on_buffer_changed, gui);
    g_signal_handlers_unblock_by_func(buf, on_mark_set, gui);
}

void parse_and_render_tables(GtkTextBuffer *buf, AppGui *gui) {
    if (!gui || !gui->source_view) return;
    block_handlers(buf, gui);
    GtkTextTag *region = table_region_tag(buf);

    GtkTextIter cur_it;
    gtk_text_buffer_get_iter_at_mark(buf, &cur_it, gtk_text_buffer_get_insert(buf));
    int cursor_line = gtk_text_iter_get_line(&cur_it);
    gboolean skipped_for_cursor = FALSE;

    int line = 0;
    while (line < gtk_text_buffer_get_line_count(buf)) {
        GtkTextIter ls;
        gtk_text_buffer_get_iter_at_line(buf, &ls, line);

        /* Idempotency: skip past an already-rendered table region. */
        if (gtk_text_iter_has_tag(&ls, region)) { line++; continue; }

        char *lt = line_text(buf, line);
        gboolean header_here = FALSE;
        if (is_table_row(lt) && line + 1 < gtk_text_buffer_get_line_count(buf)) {
            char *nxt = line_text(buf, line + 1);
            header_here = is_delimiter_row(nxt);
            g_free(nxt);
        }
        g_free(lt);

        if (header_here) {
            int last = line + 1; /* delimiter row */
            int total = gtk_text_buffer_get_line_count(buf);
            for (int r = line + 2; r < total; r++) {
                char *rt = line_text(buf, r);
                gboolean row = is_table_row(rt);
                g_free(rt);
                if (!row) break;
                last = r;
            }
            if (cursor_line >= line && cursor_line <= last) {
                /* Cursor in the table — leave it raw/editable; re-arm. */
                skipped_for_cursor = TRUE;
                line = last + 1;
                continue;
            }
            render_one_table(buf, gui, line, last);
            line = last + 1;
            continue;
        }
        line++;
    }

    unblock_handlers(buf, gui);
    if (skipped_for_cursor) gui->table_dirty = TRUE;
}

/* If the cursor sits inside a rendered table region, restore that table to raw
 * markdown so it can be edited. Returns TRUE if it un-rendered something. */
gboolean gui_table_reveal_at_cursor(GtkTextBuffer *buf, AppGui *gui) {
    if (!gui || !gui->source_view) return FALSE;
    GtkTextTag *region = table_region_tag(buf);

    GtkTextIter cur;
    gtk_text_buffer_get_iter_at_mark(buf, &cur, gtk_text_buffer_get_insert(buf));
    GtkTextIter ls = cur;
    gtk_text_iter_set_line_offset(&ls, 0);
    if (!gtk_text_iter_has_tag(&ls, region)) return FALSE;

    /* Walk up/down to the region bounds. */
    int line = gtk_text_iter_get_line(&ls);
    int top = line, bot = line;
    while (top > 0) {
        GtkTextIter t;
        gtk_text_buffer_get_iter_at_line(buf, &t, top - 1);
        if (!gtk_text_iter_has_tag(&t, region)) break;
        top--;
    }
    int total = gtk_text_buffer_get_line_count(buf);
    while (bot < total - 1) {
        GtkTextIter b;
        gtk_text_buffer_get_iter_at_line(buf, &b, bot + 1);
        if (!gtk_text_iter_has_tag(&b, region)) break;
        bot++;
    }

    /* The grid anchor lives on the top line; its stashed markdown is the
     * verbatim source. */
    GtkTextIter anchor_it;
    gtk_text_buffer_get_iter_at_line(buf, &anchor_it, top);
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&anchor_it);
    const char *md = anchor ? g_object_get_data(G_OBJECT(anchor), "table-md") : NULL;
    char *md_dup = md ? g_strdup(md) : NULL;
    /* No stashed source → never delete (would collapse N lines to 1 and desync
     * the view from doc_buf). Leave the table rendered. */
    if (!md_dup) return FALSE;

    block_handlers(buf, gui);

    /* Remove the grid widget FIRST, while the buffer is consistent. If we let
     * gtk_text_buffer_delete() tear the child widget down implicitly, the
     * unrealize/unmap path re-enters the buffer (gtk_text_buffer_get_selection_
     * bounds) mid-delete and segfaults. */
    if (anchor) {
        guint n_w = 0;
        GtkWidget **ws = gtk_text_child_anchor_get_widgets(anchor, &n_w);
        for (guint i = 0; i < n_w; i++)
            gtk_text_view_remove(GTK_TEXT_VIEW(gui->source_view), ws[i]);
        g_free(ws);
    }

    GtkTextIter rs, re;
    gtk_text_buffer_get_iter_at_line(buf, &rs, top);
    gtk_text_buffer_get_iter_at_line(buf, &re, bot);
    if (!gtk_text_iter_ends_line(&re)) gtk_text_iter_forward_to_line_end(&re);
    /* Deleting the range now removes only the (widget-free) anchor and the
     * hide/region tags along with the text. */
    gtk_text_buffer_delete(buf, &rs, &re);
    if (md_dup) {
        GtkTextIter ins;
        gtk_text_buffer_get_iter_at_line(buf, &ins, top);
        gtk_text_buffer_insert(buf, &ins, md_dup, -1);
    }
    unblock_handlers(buf, gui);
    g_free(md_dup);

    /* Remember this table's start so the cursor handler can re-render exactly it
     * when the cursor leaves — no full rescan. */
    GtkTextIter ms;
    gtk_text_buffer_get_iter_at_line(buf, &ms, top);
    if (g_revealed_start) gtk_text_buffer_delete_mark(buf, g_revealed_start);
    g_revealed_start = gtk_text_buffer_create_mark(buf, NULL, &ms, TRUE);
    return TRUE;
}

/* Re-render the currently-revealed table once the cursor has left it. */
static void rerender_revealed_if_left(GtkTextBuffer *buf, AppGui *gui) {
    if (!g_revealed_start) return;
    GtkTextIter ts;
    gtk_text_buffer_get_iter_at_mark(buf, &ts, g_revealed_start);
    int t0 = gtk_text_iter_get_line(&ts);
    int total = gtk_text_buffer_get_line_count(buf);
    int t1 = t0;
    for (int r = t0; r < total; r++) {
        char *rt = line_text(buf, r);
        gboolean row = is_table_row(rt);
        g_free(rt);
        if (!row) break;
        t1 = r;
    }

    GtkTextIter cur;
    gtk_text_buffer_get_iter_at_mark(buf, &cur, gtk_text_buffer_get_insert(buf));
    int cl = gtk_text_iter_get_line(&cur);
    if (cl >= t0 && cl <= t1) return; /* still inside — keep it raw */

    /* Cursor left: re-render if it's still a valid table. */
    if (t1 >= t0 + 1) {
        char *h = line_text(buf, t0);
        char *d = line_text(buf, t0 + 1);
        gboolean ok = is_table_row(h) && is_delimiter_row(d);
        g_free(h);
        g_free(d);
        if (ok) {
            block_handlers(buf, gui);
            render_one_table(buf, gui, t0, t1);
            unblock_handlers(buf, gui);
        }
    }
    gtk_text_buffer_delete_mark(buf, g_revealed_start);
    g_revealed_start = NULL;
}

static gboolean table_reconcile_idle(gpointer data) {
    AppGui *gui = data;
    g_table_reconcile_id = 0;
    if (!gui || !gui->source_view) return G_SOURCE_REMOVE;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
    rerender_revealed_if_left(buf, gui);     /* re-grid the table we just left */
    gui_table_reveal_at_cursor(buf, gui);    /* un-grid the table we just entered */
    return G_SOURCE_REMOVE;
}

/* Drop reveal state before the buffer content is swapped (file load / tab
 * switch). Otherwise the mark points into the old document and the cursor
 * handler could re-grid a bogus range. */
void gui_table_reset_reveal(GtkTextBuffer *buf) {
    if (g_revealed_start) {
        gtk_text_buffer_delete_mark(buf, g_revealed_start);
        g_revealed_start = NULL;
    }
    if (g_table_reconcile_id != 0) {
        g_source_remove(g_table_reconcile_id);
        g_table_reconcile_id = 0;
    }
}

/* Cursor moved (from on_mark_set). Defer the reveal/re-render to idle: mutating
 * the buffer synchronously inside mark-set aborts GTK's layout (stale byte
 * index). Only schedules when there's actually a table to reveal or re-render. */
void gui_table_on_cursor_moved(GtkTextBuffer *buf, AppGui *gui) {
    if (!gui || !gui->source_view) return;
    if (!g_revealed_start) {
        GtkTextTag *region = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buf), "md-table-region");
        if (!region) return;
        GtkTextIter ls;
        gtk_text_buffer_get_iter_at_mark(buf, &ls, gtk_text_buffer_get_insert(buf));
        gtk_text_iter_set_line_offset(&ls, 0);
        if (!gtk_text_iter_has_tag(&ls, region)) return;
    }
    if (g_table_reconcile_id == 0)
        g_table_reconcile_id = g_idle_add(table_reconcile_idle, gui);
}
