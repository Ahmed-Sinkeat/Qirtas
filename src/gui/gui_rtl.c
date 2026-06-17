#include <gtk/gtk.h>
#include "gui_internal.h"

/* Per-paragraph RTL/LTR text direction, extracted from gui.c. The two public
 * entry points (update_all_paragraphs_direction, update_paragraph_direction_lines)
 * are declared in gui_internal.h and called from gui_buffer.c / gui_tabs.c.
 * NOTE: gui_conceal.c keeps its own private detect_rtl/update_paragraph_direction
 * copies for the conceal pass; this module backs the buffer-edit path. */

/* ============================================================
 * TEXT DIRECTION (RTL/LTR)
 * ============================================================ */

/* Direction detection now lives in Zig (src/markdown.zig, zig_detect_rtl) so it
 * is shared with future platforms and covered by `zig build test-regression`.
 * This module keeps only the GTK tag application. */

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
        if (zig_detect_rtl(text)) {
            gtk_text_buffer_remove_tag(buf, ltr_tag, &start, &end);
            gtk_text_buffer_apply_tag(buf, rtl_tag, &start, &end);
        } else {
            gtk_text_buffer_remove_tag(buf, rtl_tag, &start, &end);
            gtk_text_buffer_apply_tag(buf, ltr_tag, &start, &end);
        }
        g_free(text);
    }
}

void update_all_paragraphs_direction(GtkTextBuffer *buf) {
    GtkTextIter iter;
    gtk_text_buffer_get_start_iter(buf, &iter);
    while (TRUE) {
        update_paragraph_direction(buf, &iter);
        if (!gtk_text_iter_forward_line(&iter)) {
            break;
        }
    }
}

/* Per-edit direction update: only the touched lines. The full pass above is
 * O(document) and pulls a full document copy out of Zig just to sample for
 * RTL — running it per keystroke (even from idle) was the typing-lag bug. */
void update_paragraph_direction_lines(GtkTextBuffer *buf, gint first_line, gint last_line) {
    for (gint l = first_line; l <= last_line; l++) {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(buf, &it, l);
        update_paragraph_direction(buf, &it);
        if (l >= gtk_text_buffer_get_line_count(buf)) break;
    }
}
