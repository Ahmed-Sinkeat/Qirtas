/* Fold-map: translate line numbers between the VIEW buffer (where rendered
 * regions are collapsed to a single anchor line) and the DOC buffer (the full
 * markdown source mirrored to Zig). See the Fold doc in gui_shared.h.
 *
 * These two functions are deliberately pure (no GTK, no globals): they take a
 * caller-built snapshot of the current folds so they can be unit-tested in
 * isolation (tests/test_c_behavior.c, `zig build test-c`). The live registry
 * that builds the snapshot from GtkTextMarks lives elsewhere — keeping the
 * fiddly integer math testable on its own is the whole point. */

#include "gui_shared.h"

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
