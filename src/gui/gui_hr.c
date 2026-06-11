#include "gui_internal.h"
#include <string.h>

// External prototype declarations of buffer signal handlers to enable blocking/unblocking
extern void on_insert_text_before(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data);
extern void on_insert_text_after(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data);
extern void on_delete_range_after(GtkTextBuffer *buf, GtkTextIter *start, GtkTextIter *end, gpointer user_data);
extern void on_buffer_changed(GtkTextBuffer *buf, gpointer user_data);

void parse_and_render_hrs(GtkTextBuffer *buf, AppGui *gui) {
    GtkTextIter iter;
    gtk_text_buffer_get_start_iter(buf, &iter);

    while (TRUE) {
        GtkTextIter line_end = iter;
        gtk_text_iter_forward_to_line_end(&line_end);

        gchar *line_text = gtk_text_buffer_get_text(buf, &iter, &line_end, TRUE);
        if (!line_text) break;

        if (strcmp(line_text, "---") == 0) {
            g_free(line_text);

            GtkTextIter replace_start = iter;
            GtkTextIter replace_end = line_end;

            gtk_text_buffer_delete(buf, &replace_start, &replace_end);

            GtkTextChildAnchor *anchor = gtk_text_child_anchor_new();
            gtk_text_buffer_insert_child_anchor(buf, &replace_start, anchor);

            GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
            gtk_widget_set_hexpand(separator, TRUE);
            gtk_widget_add_css_class(separator, "hr-line");

            gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(gui->source_view), separator, anchor);

            iter = replace_start;
        } else {
            g_free(line_text);
        }

        if (!gtk_text_iter_forward_line(&iter)) {
            break;
        }
    }
}

void check_and_insert_hr(GtkTextBuffer *buf, AppGui *gui) {
    GtkTextMark *insert_mark = gtk_text_buffer_get_insert(buf);
    GtkTextIter cursor;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor, insert_mark);

    GtkTextIter line_start = cursor;
    gtk_text_iter_set_line_offset(&line_start, 0);

    gchar *line_text = gtk_text_buffer_get_text(buf, &line_start, &cursor, TRUE);
    if (!line_text) return;

    gboolean is_hr = FALSE;
    GtkTextIter replace_start = line_start;
    GtkTextIter replace_end = cursor;

    size_t len = strlen(line_text);
    if (len >= 4 && strcmp(line_text + len - 4, "--- ") == 0) {
        is_hr = TRUE;
        replace_start = cursor;
        gtk_text_iter_backward_chars(&replace_start, 4);
    } else if (len >= 4 && strcmp(line_text + len - 4, "---\n") == 0) {
        is_hr = TRUE;
        replace_start = cursor;
        gtk_text_iter_backward_chars(&replace_start, 4);
    } else {
        GtkTextIter prev_char = cursor;
        if (gtk_text_iter_backward_char(&prev_char)) {
            gunichar c = gtk_text_iter_get_char(&prev_char);
            if (c == '\n') {
                GtkTextIter prev_line_start = prev_char;
                gtk_text_iter_set_line_offset(&prev_line_start, 0);
                gchar *prev_line_text = gtk_text_buffer_get_text(buf, &prev_line_start, &prev_char, TRUE);
                if (prev_line_text && strcmp(prev_line_text, "---") == 0) {
                    is_hr = TRUE;
                    replace_start = prev_line_start;
                    replace_end = prev_char;
                }
                g_free(prev_line_text);
            }
        }
    }

    g_free(line_text);

    if (is_hr) {
        g_signal_handlers_block_by_func(buf, on_insert_text_before, gui);
        g_signal_handlers_block_by_func(buf, on_insert_text_after, gui);
        g_signal_handlers_block_by_func(buf, on_delete_range_after, gui);
        g_signal_handlers_block_by_func(buf, on_buffer_changed, gui);

        gtk_text_buffer_delete(buf, &replace_start, &replace_end);

        GtkTextChildAnchor *anchor = gtk_text_child_anchor_new();
        gtk_text_buffer_insert_child_anchor(buf, &replace_start, anchor);
        
        GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_widget_set_hexpand(separator, TRUE);
        gtk_widget_add_css_class(separator, "hr-line");

        gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(gui->source_view), separator, anchor);

        GtkTextIter next_char = replace_start;
        gunichar nc = gtk_text_iter_get_char(&next_char);
        if (nc != '\n' && nc != '\r' && !gtk_text_iter_is_end(&next_char)) {
            gtk_text_buffer_insert(buf, &replace_start, "\n", 1);
        } else if (gtk_text_iter_is_end(&next_char)) {
            gtk_text_buffer_insert(buf, &replace_start, "\n", 1);
        }

        g_signal_handlers_unblock_by_func(buf, on_insert_text_before, gui);
        g_signal_handlers_unblock_by_func(buf, on_insert_text_after, gui);
        g_signal_handlers_unblock_by_func(buf, on_delete_range_after, gui);
        g_signal_handlers_unblock_by_func(buf, on_buffer_changed, gui);
    }
}

gboolean idle_render_hrs_cb(gpointer user_data) {
    HrRenderData *d = user_data;
    if (d->generation != d->gui->buffer_generation) {
        g_free(d);
        return G_SOURCE_REMOVE;
    }
    if (d->gui && d->gui->source_view) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->gui->source_view));
        parse_and_render_hrs(buf, d->gui);
    }
    g_free(d);
    return G_SOURCE_REMOVE;
}
