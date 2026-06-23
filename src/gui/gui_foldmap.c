/* Fold-map: translate line numbers between the VIEW buffer (where rendered
 * regions are collapsed to a single anchor line) and the DOC buffer (the full
 * markdown source mirrored to Zig). See the Fold doc in gui_shared.h.
 *
 * These two functions are deliberately pure (no GTK, no globals): they take a
 * caller-built snapshot of the current folds so they can be unit-tested in
 * isolation (tests/test_c_behavior.c, `zig build test-c`). The live registry
 * that builds the snapshot from GtkTextMarks lives elsewhere — keeping the
 * fiddly integer math testable on its own is the whole point. */

#include "gui_internal.h"  /* pulls gui_shared.h (Fold, Position) + GTK */

/* view line -> doc line. The anchor line of a fold maps to that fold's first
 * doc line; lines after a fold are pushed down by its hidden (doc_count-1)
 * lines. Folds are sorted by view_line, so we can stop at the first fold that
 * starts at/after the query line. */
int foldmap_view_to_doc(const Fold *folds, int n, int view_line) {
    int doc = view_line;
    for (int i = 0; i < n; i++) {
        if (folds[i].view_line < view_line)
            doc += folds[i].doc_count - 1;
        else
            break;
    }
    return doc;
}

/* doc line -> view line. A doc line that falls inside a fold's collapsed range
 * maps to that fold's single anchor line; a doc line past a fold is pulled up
 * by the lines that fold hides. */
int foldmap_doc_to_view(const Fold *folds, int n, int doc_line) {
    int hidden_before = 0;
    for (int i = 0; i < n; i++) {
        int doc_start = folds[i].doc_line;
        int doc_end = doc_start + folds[i].doc_count; /* exclusive */
        if (doc_line < doc_start)
            break;                       /* query is before this fold */
        if (doc_line < doc_end)
            return folds[i].view_line;   /* inside the fold -> its anchor line */
        hidden_before += folds[i].doc_count - 1; /* fully past this fold */
    }
    return doc_line - hidden_before;
}

/* ── Live registry ────────────────────────────────────────────────────────
 * The set of folds currently rendered in the view. Each fold is pinned to its
 * anchor line by a GtkTextMark (left gravity), so ordinary edits above/below
 * shift it automatically — we never store a raw line number that could go
 * stale. The pure translators above run against a snapshot rebuilt from the
 * live mark positions on demand; with no folds registered the snapshot is
 * empty and every translation is the identity (so this whole layer is inert
 * until a decorator starts folding in stage 3). */

typedef struct {
    GtkTextMark *mark;  /* on the anchor view line, left gravity */
    int doc_count;      /* doc lines this fold stands in for (>=1) */
} LiveFold;

static GArray *g_folds = NULL; /* of LiveFold; lazily created */

/* ponytail: per-edit translation sorts this snapshot, so a hard cap keeps it
 * stack-allocated and O(f log f) bounded. A markdown doc with >1024 rendered
 * regions is not a realistic case; beyond it we simply translate as identity
 * for the overflow (folds render raw, never corrupt). */
#define FOLD_MAX 1024

static int cmp_fold_view_line(const void *a, const void *b) {
    return ((const Fold *)a)->view_line - ((const Fold *)b)->view_line;
}

/* Build a sorted, self-consistent Fold[] from the live marks. Returns count. */
static int foldmap_snapshot(GtkTextBuffer *buf, Fold *out, int max) {
    if (!g_folds || g_folds->len == 0) return 0;
    int n = 0;
    for (guint i = 0; i < g_folds->len && n < max; i++) {
        LiveFold *lf = &g_array_index(g_folds, LiveFold, i);
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_mark(buf, &it, lf->mark);
        out[n].view_line = gtk_text_iter_get_line(&it);
        out[n].doc_count = lf->doc_count;
        out[n].doc_line = 0; /* filled after sort */
        n++;
    }
    qsort(out, n, sizeof(Fold), cmp_fold_view_line);
    /* doc_line[i] = view_line[i] + hidden lines of all earlier folds */
    int hidden = 0;
    for (int i = 0; i < n; i++) {
        out[i].doc_line = out[i].view_line + hidden;
        hidden += out[i].doc_count - 1;
    }
    return n;
}

int foldmap_live_view_to_doc(GtkTextBuffer *buf, int view_line) {
    if (!g_folds || g_folds->len == 0) return view_line; /* fast path: inert */
    Fold tmp[FOLD_MAX];
    int n = foldmap_snapshot(buf, tmp, FOLD_MAX);
    return foldmap_view_to_doc(tmp, n, view_line);
}

int foldmap_live_doc_to_view(GtkTextBuffer *buf, int doc_line) {
    if (!g_folds || g_folds->len == 0) return doc_line; /* fast path: inert */
    Fold tmp[FOLD_MAX];
    int n = foldmap_snapshot(buf, tmp, FOLD_MAX);
    return foldmap_doc_to_view(tmp, n, doc_line);
}

/* Register a fold: pin a mark to the start of `view_line`; it stands in for
 * `doc_count` doc lines. Caller has already collapsed the region in the view. */
void foldmap_register(GtkTextBuffer *buf, int view_line, int doc_count) {
    if (doc_count < 1) doc_count = 1;
    if (!g_folds) g_folds = g_array_new(FALSE, FALSE, sizeof(LiveFold));
    if (g_folds->len >= FOLD_MAX) return; /* overflow: leave unfolded */
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line(buf, &it, view_line);
    LiveFold lf = { gtk_text_buffer_create_mark(buf, NULL, &it, TRUE), doc_count };
    g_array_append_val(g_folds, lf);
}

/* Drop the fold whose anchor mark currently sits on `view_line`. */
void foldmap_unregister_at(GtkTextBuffer *buf, int view_line) {
    if (!g_folds) return;
    for (guint i = 0; i < g_folds->len; i++) {
        LiveFold *lf = &g_array_index(g_folds, LiveFold, i);
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_mark(buf, &it, lf->mark);
        if (gtk_text_iter_get_line(&it) == view_line) {
            gtk_text_buffer_delete_mark(buf, lf->mark);
            g_array_remove_index(g_folds, i);
            return;
        }
    }
}

/* Forget every fold (file load / tab switch — the marks die with the buffer
 * content, but the array must be reset so stale entries don't translate). */
void foldmap_clear(GtkTextBuffer *buf) {
    if (!g_folds) return;
    for (guint i = 0; i < g_folds->len; i++) {
        LiveFold *lf = &g_array_index(g_folds, LiveFold, i);
        if (buf && lf->mark) gtk_text_buffer_delete_mark(buf, lf->mark);
    }
    g_array_set_size(g_folds, 0);
}
