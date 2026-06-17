#include "gui_internal.h"
#include <string.h>

/* Fenced code blocks → a "pill" header (language label + copy button), like the
 * card you see in chat UIs. This is a VIEW-ONLY decoration, exactly like the
 * horizontal-rule renderer in gui_hr.c: the opening ``` fence line's text is
 * replaced by a child-anchor widget and the closing fence is hidden with a
 * transparent tag. The Zig document buffer (doc_buf) is the source of truth and
 * is never touched — saving writes the real ```lang … ``` markdown back out.
 *
 * Safety: replacing text means delete + insert_child_anchor, which would echo
 * into doc_buf through the buffer signal handlers. We block those handlers for
 * the duration (block-by-func is reference counted, so this is also safe when
 * the caller — gui_set_text — has already blocked them). Iterators are
 * re-resolved by line NUMBER after every mutation; reusing an iter across a
 * delete/insert is the data-loss bug documented in gui_hr.c. */

extern void on_insert_text_before(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data);
extern void on_insert_text_after(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data);
extern void on_delete_range_after(GtkTextBuffer *buf, GtkTextIter *start, GtkTextIter *end, gpointer user_data);
extern void on_buffer_changed(GtkTextBuffer *buf, gpointer user_data);
/* Blocked alongside the others: our delete/insert moves the cursor mark and
 * fires mark-set synchronously; on_mark_set's conceal pass then runs on a
 * half-mutated buffer with stale iterators. */
extern void on_mark_set(GtkTextBuffer *buf, GtkTextIter *location, GtkTextMark *mark, gpointer user_data);

/* Fence detection moved to src/markdown.zig (shared + unit-tested). These stay
 * as thin C glue; lang_out keeps the g_strdup ownership the callers expect. */
static gboolean fence_only(const char *s) {
    return s ? zig_fence_only(s) : FALSE;
}

static gboolean opening_fence(const char *s, char **lang_out) {
    if (!s) return FALSE;
    char lang[128];
    if (!zig_code_fence_lang(s, lang, (int)sizeof lang)) return FALSE;
    *lang_out = g_strdup(lang);
    return TRUE;
}

/* First fence-only line at or after `from_line`, or -1 if the block never
 * closes (an unterminated fence is left as raw text, not pilled). */
static int find_closing_fence(GtkTextBuffer *buf, int from_line) {
    int total = gtk_text_buffer_get_line_count(buf);
    for (int l = from_line; l < total; l++) {
        GtkTextIter s, e;
        gtk_text_buffer_get_iter_at_line(buf, &s, l);
        e = s;
        if (!gtk_text_iter_ends_line(&e)) gtk_text_iter_forward_to_line_end(&e);
        gchar *t = gtk_text_buffer_get_text(buf, &s, &e, TRUE);
        gboolean hit = fence_only(t);
        g_free(t);
        if (hit) return l;
    }
    return -1;
}

/* Display name for the pill: "bash" → "Bash", empty → "Code". */
static char *lang_display(const char *lang) {
    if (!lang || !*lang) return g_strdup("Code");
    char *d = g_strdup(lang);
    d[0] = g_ascii_toupper(d[0]);
    return d;
}

typedef struct {
    AppGui *gui;
    GtkTextChildAnchor *anchor; /* the pill, on the former opening-fence line */
} CodeCopyData;

static void code_copy_data_free(gpointer data, GClosure *closure) {
    (void)closure;
    CodeCopyData *d = data;
    if (!d) return;
    if (d->anchor) g_object_unref(d->anchor);
    g_free(d);
}

/* Copy the code BETWEEN the pill's opening fence and the matching closing fence
 * to the clipboard. Reads straight from the live view buffer (the code text is
 * never decorated, only the two fence lines are). */
static void on_code_copy_clicked(GtkButton *btn, gpointer user_data) {
    CodeCopyData *d = user_data;
    if (!d || !d->gui || !d->gui->source_view) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->gui->source_view));

    GtkTextIter it;
    gtk_text_buffer_get_iter_at_child_anchor(buf, &it, d->anchor);
    if (!gtk_text_iter_forward_line(&it)) return; /* nothing after the fence */

    GString *code = g_string_new("");
    while (!gtk_text_iter_is_end(&it)) {
        GtkTextIter ls = it, le;
        gtk_text_iter_set_line_offset(&ls, 0);
        le = ls;
        if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
        gchar *ln = gtk_text_buffer_get_text(buf, &ls, &le, FALSE);
        if (fence_only(ln)) { g_free(ln); break; } /* reached closing fence */
        g_string_append(code, ln);
        g_string_append_c(code, '\n');
        g_free(ln);
        if (!gtk_text_iter_forward_line(&it)) break;
    }

    GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(btn));
    gdk_clipboard_set_text(cb, code->str);
    g_string_free(code, TRUE);

    /* Lightweight feedback: flip to a checkmark for a moment. */
    gtk_button_set_icon_name(btn, "object-select-symbolic");
}

static GtkTextTag *fence_hide_tag(GtkTextBuffer *buf) {
    GtkTextTagTable *tab = gtk_text_buffer_get_tag_table(buf);
    GtkTextTag *t = gtk_text_tag_table_lookup(tab, "code-fence-hide");
    if (t) return t;
    /* Per gui_conceal.c's note, never use the "invisible" property (GTK4's
     * visible-line bookkeeping mishandles it). Shrink + transparent ink hides
     * the closing ``` while keeping the line — and its text — intact. */
    GdkRGBA clear = { 0, 0, 0, 0 };
    return gtk_text_buffer_create_tag(buf, "code-fence-hide",
                                      "scale", 0.01,
                                      "foreground-rgba", &clear,
                                      NULL);
}

/* Replacing the opening ``` in the view removes the fence that GtkSourceView
 * keys code highlighting off, so the body would render as plain prose. A
 * monospace tag keeps it looking like code (the style scheme still colours
 * tokens on top). */
static GtkTextTag *code_body_tag(GtkTextBuffer *buf) {
    GtkTextTagTable *tab = gtk_text_buffer_get_tag_table(buf);
    GtkTextTag *t = gtk_text_tag_table_lookup(tab, "code-block-body");
    if (t) return t;
    /* Translucent grey paragraph fill gives the body a "card" look that reads
     * on both light and dark themes (alpha composites over the view colour). */
    GdkRGBA card = { 0.5, 0.5, 0.5, 0.08 };
    return gtk_text_buffer_create_tag(buf, "code-block-body",
                                      "family", "monospace",
                                      "paragraph-background-rgba", &card,
                                      NULL);
}

void parse_and_render_code_pills(GtkTextBuffer *buf, AppGui *gui) {
    if (!gui || !gui->source_view) return;

    g_signal_handlers_block_by_func(buf, on_insert_text_before, gui);
    g_signal_handlers_block_by_func(buf, on_insert_text_after, gui);
    g_signal_handlers_block_by_func(buf, on_delete_range_after, gui);
    g_signal_handlers_block_by_func(buf, on_buffer_changed, gui);
    g_signal_handlers_block_by_func(buf, on_mark_set, gui);

    GtkTextTag *hide = fence_hide_tag(buf);
    GtkTextTag *body = code_body_tag(buf);

    /* Don't pill a block while the cursor is on its opening fence: the writer
     * may still be typing the language (```bash). Render it once they move off.
     * `skipped_for_cursor` re-arms the debounce so that happens. */
    GtkTextIter cur_it;
    gtk_text_buffer_get_iter_at_mark(buf, &cur_it, gtk_text_buffer_get_insert(buf));
    int cursor_line = gtk_text_iter_get_line(&cur_it);
    gboolean skipped_for_cursor = FALSE;

    int line = 0;
    while (line < gtk_text_buffer_get_line_count(buf)) {
        GtkTextIter s, e;
        gtk_text_buffer_get_iter_at_line(buf, &s, line);

        /* Idempotency for the live re-render path: an already-hidden closing
         * fence still reads as ``` and would otherwise be mistaken for a NEW
         * opening fence. Skip lines already tagged as a hidden fence. (Opening
         * fences of rendered blocks are anchors, not backticks, so they fall
         * through opening_fence() harmlessly.) */
        if (gtk_text_iter_has_tag(&s, hide)) { line++; continue; }

        e = s;
        if (!gtk_text_iter_ends_line(&e)) gtk_text_iter_forward_to_line_end(&e);
        gchar *line_text = gtk_text_buffer_get_text(buf, &s, &e, TRUE);

        char *lang = NULL;
        if (line_text && opening_fence(line_text, &lang)) {
            int close_line = find_closing_fence(buf, line + 1);
            if (close_line > line) {
                if (cursor_line == line) {
                    /* Writer is on the opening fence — leave it raw/editable. */
                    skipped_for_cursor = TRUE;
                    g_free(lang);
                    g_free(line_text);
                    line = close_line + 1;
                    continue;
                }
                /* Replace the opening fence line's text with the pill anchor.
                 * Deleting text + inserting the anchor keeps the line (and so
                 * the line count, and close_line's index) stable. */
                gtk_text_buffer_get_iter_at_line(buf, &s, line);
                e = s;
                if (!gtk_text_iter_ends_line(&e)) gtk_text_iter_forward_to_line_end(&e);
                gtk_text_buffer_delete(buf, &s, &e);

                GtkTextIter at;
                gtk_text_buffer_get_iter_at_line(buf, &at, line);
                GtkTextChildAnchor *anchor = gtk_text_child_anchor_new();
                gtk_text_buffer_insert_child_anchor(buf, &at, anchor);

                GtkWidget *pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
                gtk_widget_add_css_class(pill, "code-pill");
                gtk_widget_set_hexpand(pill, TRUE);

                char *disp = lang_display(lang);
                GtkWidget *label = gtk_label_new(disp);
                g_free(disp);
                gtk_widget_add_css_class(label, "code-pill-lang");
                gtk_widget_set_halign(label, GTK_ALIGN_START);
                gtk_widget_set_hexpand(label, TRUE);

                GtkWidget *copy = gtk_button_new_from_icon_name("edit-copy-symbolic");
                gtk_widget_add_css_class(copy, "code-pill-copy");
                gtk_widget_add_css_class(copy, "flat");
                gtk_widget_set_tooltip_text(copy, "Copy code");

                CodeCopyData *cd = g_new0(CodeCopyData, 1);
                cd->gui = gui;
                cd->anchor = g_object_ref(anchor);
                g_signal_connect_data(copy, "clicked", G_CALLBACK(on_code_copy_clicked),
                                      cd, code_copy_data_free, 0);

                gtk_box_append(GTK_BOX(pill), label);
                gtk_box_append(GTK_BOX(pill), copy);
                gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(gui->source_view), pill, anchor);

                /* Monospace the body lines [line+1 .. close_line-1] — but NOT
                 * Arabic/RTL lines. Monospace fonts break Arabic cursive joining
                 * and often lack Arabic glyphs (letters mis-shape or vanish), so
                 * Arabic code keeps the shaping UI font; only Latin lines get the
                 * code look. */
                for (int br = line + 1; br < close_line; br++) {
                    GtkTextIter bls, ble;
                    gtk_text_buffer_get_iter_at_line(buf, &bls, br);
                    ble = bls;
                    if (!gtk_text_iter_ends_line(&ble)) gtk_text_iter_forward_to_line_end(&ble);
                    gchar *bt = gtk_text_buffer_get_text(buf, &bls, &ble, TRUE);
                    gboolean rtl = bt ? zig_detect_rtl(bt) : FALSE;
                    g_free(bt);
                    if (!rtl) gtk_text_buffer_apply_tag(buf, body, &bls, &ble);
                }

                /* Hide the closing fence. */
                GtkTextIter cs, ce;
                gtk_text_buffer_get_iter_at_line(buf, &cs, close_line);
                ce = cs;
                if (!gtk_text_iter_ends_line(&ce)) gtk_text_iter_forward_to_line_end(&ce);
                gtk_text_buffer_apply_tag(buf, hide, &cs, &ce);

                g_free(lang);
                g_free(line_text);
                line = close_line + 1; /* skip the whole block */
                continue;
            }
            g_free(lang);
        }

        g_free(line_text);
        line++;
    }

    g_signal_handlers_unblock_by_func(buf, on_insert_text_before, gui);
    g_signal_handlers_unblock_by_func(buf, on_insert_text_after, gui);
    g_signal_handlers_unblock_by_func(buf, on_delete_range_after, gui);
    g_signal_handlers_unblock_by_func(buf, on_buffer_changed, gui);
    g_signal_handlers_unblock_by_func(buf, on_mark_set, gui);

    /* A block was left raw because the cursor was on its fence — re-arm so the
     * next edit (the writer typing/leaving the line) renders it. */
    if (skipped_for_cursor) gui->code_pill_dirty = TRUE;
}
