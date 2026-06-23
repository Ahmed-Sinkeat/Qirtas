#include "gui_internal.h"
#include <string.h>

extern void on_insert_text_before(GtkTextBuffer *, GtkTextIter *, gchar *, gint, gpointer);
extern void on_insert_text_after(GtkTextBuffer *, GtkTextIter *, gchar *, gint, gpointer);
/* on_delete_range_BEFORE is the doc_buf delete mirror (zig_delete_range). It MUST
 * be blocked: render_todo_line deletes the "- [ ] text" line text in the view,
 * and todos render LIVE on read-mode entry (not under loading_viewport), so an
 * unblocked delete here erases the todo text from doc_buf — the saved file loses
 * the item. (on_delete_range_after is view-only bookkeeping, blocked for safety.) */
extern void on_delete_range_before(GtkTextBuffer *, GtkTextIter *, GtkTextIter *, gpointer);
extern void on_delete_range_after(GtkTextBuffer *, GtkTextIter *, GtkTextIter *, gpointer);
extern void on_buffer_changed(GtkTextBuffer *, gpointer);
extern void on_mark_set(GtkTextBuffer *, GtkTextIter *, GtkTextMark *, gpointer);

/* Returns 1 if line matches "- [ ] text" or "- [x] text".
 * Sets *checked and *text_start (pointer into line at the item text). */
static int parse_todo_line(const char *line, gboolean *checked, const char **text_start) {
    if (!line) return 0;
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '-' && *p != '*' && *p != '+') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '[') return 0;
    p++;
    char mark = *p;
    if (mark != ' ' && mark != 'x' && mark != 'X') return 0;
    p++;
    if (*p != ']') return 0;
    p++;
    if (*p != ' ' && *p != '\t' && *p != '\0') return 0;
    while (*p == ' ' || *p == '\t') p++;
    *checked    = (mark == 'x' || mark == 'X');
    *text_start = p;
    return 1;
}

static void block_handlers(GtkTextBuffer *buf, AppGui *gui) {
    g_signal_handlers_block_by_func(buf, on_insert_text_before, gui);
    g_signal_handlers_block_by_func(buf, on_insert_text_after,  gui);
    g_signal_handlers_block_by_func(buf, on_delete_range_before, gui);
    g_signal_handlers_block_by_func(buf, on_delete_range_after, gui);
    g_signal_handlers_block_by_func(buf, on_buffer_changed,     gui);
    g_signal_handlers_block_by_func(buf, on_mark_set,           gui);
}
static void unblock_handlers(GtkTextBuffer *buf, AppGui *gui) {
    g_signal_handlers_unblock_by_func(buf, on_insert_text_before, gui);
    g_signal_handlers_unblock_by_func(buf, on_insert_text_after,  gui);
    g_signal_handlers_unblock_by_func(buf, on_delete_range_before, gui);
    g_signal_handlers_unblock_by_func(buf, on_delete_range_after, gui);
    g_signal_handlers_unblock_by_func(buf, on_buffer_changed,     gui);
    g_signal_handlers_unblock_by_func(buf, on_mark_set,           gui);
}

typedef struct {
    GtkTextBuffer      *buf;
    AppGui             *gui;
    GtkTextChildAnchor *anchor;
} TodoToggleData;

static void on_todo_toggled(GtkCheckButton *btn, gpointer user_data) {
    (void)btn;
    TodoToggleData *d = (TodoToggleData *)user_data;
    if (!d || !d->buf || !d->gui || !d->anchor) return;

    gboolean now_checked = gtk_check_button_get_active(GTK_CHECK_BUTTON(btn));

    const char *old_md = (const char *)g_object_get_data(G_OBJECT(d->anchor), "todo-md");
    if (!old_md) return;

    char *new_md = g_strdup(old_md);
    char *bracket = strstr(new_md, "[ ]");
    if (!bracket) bracket = strstr(new_md, "[x]");
    if (!bracket) bracket = strstr(new_md, "[X]");
    if (bracket) bracket[1] = now_checked ? 'x' : ' ';

    /* Update anchor metadata for any future restore pass. */
    g_object_set_data_full(G_OBJECT(d->anchor), "todo-md", new_md, g_free);

    /* Update zig doc_buf — the GTK buffer has an anchor char here,
     * but the zig side still holds the original markdown. */
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_child_anchor(d->buf, &it, d->anchor);
    int line_num = gtk_text_iter_get_line(&it);
    Position ps = { line_num, 0 };
    Position pe = { line_num, (int)strlen(old_md) };
    zig_replace_range(ps, pe, new_md);
    /* new_md ownership transferred to anchor's GObject data above. */
}

/* Create a todo widget anchor at line_num. Caller must block handlers first. */
static void render_todo_line(GtkTextBuffer *buf, AppGui *gui, int line_num) {
    GtkTextIter ls, le;
    gtk_text_buffer_get_iter_at_line(buf, &ls, line_num);
    le = ls;
    if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);

    gchar *raw = gtk_text_buffer_get_text(buf, &ls, &le, TRUE);
    if (!raw) return;

    gboolean checked;
    const char *item_text;
    if (!parse_todo_line(raw, &checked, &item_text)) { g_free(raw); return; }

    /* Single GtkCheckButton with built-in label. GTK4 respects widget direction
     * for indicator placement — Arabic (RTL) text gets the box on the right. */
    GtkWidget *check = gtk_check_button_new_with_label(item_text);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check), checked);
    gtk_widget_add_css_class(check, "todo-check");

    /* Replace line text with anchor. */
    gtk_text_buffer_delete(buf, &ls, &le);
    gtk_text_buffer_get_iter_at_line(buf, &ls, line_num);
    GtkTextChildAnchor *anchor = gtk_text_child_anchor_new();
    gtk_text_buffer_insert_child_anchor(buf, &ls, anchor);

    g_object_set_data_full(G_OBJECT(anchor), "todo-md", g_strdup(raw), g_free);
    gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(gui->source_view), check, anchor);

    TodoToggleData *td = g_new0(TodoToggleData, 1);
    td->buf    = buf;
    td->gui    = gui;
    td->anchor = anchor;
    g_signal_connect_data(check, "toggled", G_CALLBACK(on_todo_toggled), td,
                          (GClosureNotify)g_free, 0);

    g_free(raw);
}

void parse_and_render_todos(GtkTextBuffer *buf, AppGui *gui) {
    if (!gui || !gui->source_view) return;
    if (!gui->read_mode) return;  /* widgets only in read mode */

    int total = gtk_text_buffer_get_line_count(buf);
    for (int line = 0; line < total; line++) {
        GtkTextIter ls;
        gtk_text_buffer_get_iter_at_line(buf, &ls, line);

        /* Skip already-rendered anchor lines that belong to todo items. */
        GtkTextChildAnchor *existing = gtk_text_iter_get_child_anchor(&ls);
        if (existing && g_object_get_data(G_OBJECT(existing), "todo-md")) continue;

        GtkTextIter le = ls;
        if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
        gchar *t = gtk_text_buffer_get_text(buf, &ls, &le, TRUE);
        if (!t) continue;

        gboolean chk;
        const char *item_text;
        gboolean is_todo = parse_todo_line(t, &chk, &item_text);
        g_free(t);

        if (is_todo) {
            block_handlers(buf, gui);
            render_todo_line(buf, gui, line);
            unblock_handlers(buf, gui);
        }
    }
}

/* Remove every rendered todo anchor and restore raw markdown.
 * Called when leaving read mode. */
void gui_todo_restore_all(GtkTextBuffer *buf, AppGui *gui) {
    if (!gui || !gui->source_view) return;

    block_handlers(buf, gui);
    int total = gtk_text_buffer_get_line_count(buf);
    for (int line = 0; line < total; line++) {
        GtkTextIter ls;
        gtk_text_buffer_get_iter_at_line(buf, &ls, line);
        GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&ls);
        if (!anchor) continue;
        const char *md = (const char *)g_object_get_data(G_OBJECT(anchor), "todo-md");
        if (!md) continue;

        char *md_dup = g_strdup(md);
        GtkTextIter le = ls;
        gtk_text_iter_forward_char(&le);  /* past the 1-char anchor */
        gtk_text_buffer_delete(buf, &ls, &le);
        gtk_text_buffer_get_iter_at_line(buf, &ls, line);
        gtk_text_buffer_insert(buf, &ls, md_dup, -1);
        g_free(md_dup);
        /* zig doc_buf already correct — no zig update needed. */
    }
    unblock_handlers(buf, gui);
}

/* No cursor-reveal: todo items stay as widgets for the whole read-mode session.
 * Users toggle read mode off to edit item text. */
void gui_todo_on_cursor_moved(GtkTextBuffer *buf, AppGui *gui) {
    (void)buf; (void)gui;
}
