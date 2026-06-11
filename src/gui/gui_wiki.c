#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <string.h>
#include "gui_internal.h"

static gboolean editor_get_iter_at_widget_point(AppGui *gui, gdouble x, gdouble y, GtkTextIter *iter) {
    if (!gui || !gui->source_view || !iter) return FALSE;
    int bx, by;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(gui->source_view),
                                         GTK_TEXT_WINDOW_WIDGET,
                                         (int)x, (int)y, &bx, &by);
    int trailing = 0;
    return gtk_text_view_get_iter_at_position(GTK_TEXT_VIEW(gui->source_view), iter, &trailing, bx, by);
}

static gboolean wiki_local_queued = FALSE;
static gboolean wiki_global_queued = FALSE;

static void apply_wiki_link_tags_local_impl(GtkTextBuffer *buf) {
    if (global_gui && global_gui->in_conceal_update) return;

    GtkTextMark *insert_mark = gtk_text_buffer_get_insert(buf);
    GtkTextIter cursor_iter;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor_iter, insert_mark);

    int cursor_line = gtk_text_iter_get_line(&cursor_iter);
    int total_lines = gtk_text_buffer_get_line_count(buf);

    int start_line = cursor_line - 1;
    if (start_line < 0) start_line = 0;
    int end_line = cursor_line + 1;
    if (end_line >= total_lines) end_line = total_lines - 1;

    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_line(buf, &start, start_line);
    gtk_text_buffer_get_iter_at_line(buf, &end, end_line);
    gtk_text_iter_forward_to_line_end(&end);

    GtkTextTag *tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buf), "wiki-link");
    if (!tag) {
        tag = gtk_text_buffer_create_tag(buf, "wiki-link",
                                         "underline", PANGO_UNDERLINE_SINGLE,
                                         "foreground", "#ff79c6",
                                         NULL);
    }

    // Remove tags locally first
    gtk_text_buffer_remove_tag(buf, tag, &start, &end);

    // Re-fetch fresh GtkTextIter values from the buffer after tag modification
    gtk_text_buffer_get_iter_at_line(buf, &start, start_line);
    gtk_text_buffer_get_iter_at_line(buf, &end, end_line);
    gtk_text_iter_forward_to_line_end(&end);

    GtkTextIter iter = start;
    while (TRUE) {
        GtkTextIter match_start, match_end;
        if (!gtk_text_iter_forward_search(&iter, "[[", GTK_TEXT_SEARCH_VISIBLE_ONLY, &match_start, &match_end, &end)) {
            break;
        }

        GtkTextIter close_start, close_end;
        if (!gtk_text_iter_forward_search(&match_end, "]]", GTK_TEXT_SEARCH_VISIBLE_ONLY, &close_start, &close_end, &end)) {
            break;
        }

        int end_offset = gtk_text_iter_get_offset(&end);
        int close_end_offset = gtk_text_iter_get_offset(&close_end);

        gtk_text_buffer_apply_tag(buf, tag, &match_start, &close_end);

        // Reacquire fresh GtkTextIter values from the buffer after tag modification
        gtk_text_buffer_get_iter_at_offset(buf, &iter, close_end_offset);
        gtk_text_buffer_get_iter_at_offset(buf, &end, end_offset);
    }
}

typedef struct {
    AppGui *gui;
    guint generation;
} WikiData;

static gboolean idle_wiki_local_cb(gpointer user_data) {
    WikiData *d = user_data;
    wiki_local_queued = FALSE;
    if (d->generation != d->gui->buffer_generation) {
        g_free(d);
        return G_SOURCE_REMOVE;
    }
    if (d->gui && global_source_view) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));
        apply_wiki_link_tags_local_impl(buf);
    }
    g_free(d);
    return G_SOURCE_REMOVE;
}

void apply_wiki_link_tags_local(GtkTextBuffer *buf) {
    (void)buf;
    if (wiki_local_queued) return;
    if (!global_gui) return;
    
    wiki_local_queued = TRUE;
    WikiData *d = g_new(WikiData, 1);
    d->gui = global_gui;
    d->generation = global_gui->buffer_generation;
    g_idle_add(idle_wiki_local_cb, d);
}

static void apply_wiki_link_tags_impl(GtkTextBuffer *buf) {
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);

    GtkTextTag *tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buf), "wiki-link");
    if (tag) {
        gtk_text_buffer_remove_tag(buf, tag, &start, &end);
    } else {
        tag = gtk_text_buffer_create_tag(buf, "wiki-link",
                                         "underline", PANGO_UNDERLINE_SINGLE,
                                         "foreground", "#ff79c6",
                                         NULL);
    }

    GtkTextIter iter;
    gtk_text_buffer_get_start_iter(buf, &iter);
    while (TRUE) {
        GtkTextIter match_start, match_end;
        if (!gtk_text_iter_forward_search(&iter, "[[", GTK_TEXT_SEARCH_VISIBLE_ONLY, &match_start, &match_end, NULL)) {
            break;
        }

        GtkTextIter close_start, close_end;
        if (!gtk_text_iter_forward_search(&match_end, "]]", GTK_TEXT_SEARCH_VISIBLE_ONLY, &close_start, &close_end, NULL)) {
            break;
        }

        gtk_text_buffer_apply_tag(buf, tag, &match_start, &close_end);
        iter = close_end;
    }
}

static gboolean idle_wiki_global_cb(gpointer user_data) {
    WikiData *d = user_data;
    wiki_global_queued = FALSE;
    if (d->generation != d->gui->buffer_generation) {
        g_free(d);
        return G_SOURCE_REMOVE;
    }
    if (d->gui && global_source_view) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));
        apply_wiki_link_tags_impl(buf);
    }
    g_free(d);
    return G_SOURCE_REMOVE;
}

void apply_wiki_link_tags(GtkTextBuffer *buf) {
    (void)buf;
    if (wiki_global_queued) return;
    if (!global_gui) return;
    
    wiki_global_queued = TRUE;
    WikiData *d = g_new(WikiData, 1);
    d->gui = global_gui;
    d->generation = global_gui->buffer_generation;
    g_idle_add(idle_wiki_global_cb, d);
}

void on_editor_left_click(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;

    if (gui->sidebar && gtk_widget_get_visible(gui->sidebar)) {
        gtk_widget_set_visible(gui->sidebar, FALSE);
    }

    GdkModifierType state =
        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    gboolean should_open_link = (n_press >= 2) || ((state & GDK_CONTROL_MASK) != 0);
    if (!should_open_link) return;

    GtkTextIter iter;
    if (editor_get_iter_at_widget_point(gui, x, y, &iter)) {
        GtkTextTagTable *table = gtk_text_buffer_get_tag_table(gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view)));
        GtkTextTag *wiki_tag = gtk_text_tag_table_lookup(table, "wiki-link");
        if (wiki_tag && gtk_text_iter_has_tag(&iter, wiki_tag)) {
            GtkTextIter tag_start = iter;
            GtkTextIter tag_end = iter;

            while (!gtk_text_iter_is_start(&tag_start) && gtk_text_iter_has_tag(&tag_start, wiki_tag)) {
                gtk_text_iter_backward_char(&tag_start);
            }
            if (!gtk_text_iter_has_tag(&tag_start, wiki_tag)) {
                gtk_text_iter_forward_char(&tag_start);
            }

            while (!gtk_text_iter_is_end(&tag_end) && gtk_text_iter_has_tag(&tag_end, wiki_tag)) {
                gtk_text_iter_forward_char(&tag_end);
            }

            char *full_link = gtk_text_buffer_get_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view)), &tag_start, &tag_end, TRUE);
            size_t len = strlen(full_link);
            if (len > 4 && full_link[0] == '[' && full_link[1] == '[') {
                char *note_name = g_strndup(full_link + 2, len - 4);
                zig_open_wiki_link(note_name);
                g_free(note_name);
                gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
            }
            g_free(full_link);
        }
    }
}

void on_editor_motion(GtkEventControllerMotion *controller, gdouble x, gdouble y, gpointer user_data) {
    (void)controller;
    AppGui *gui = (AppGui *)user_data;
    GtkTextIter iter;
    gboolean is_link = FALSE;

    if (editor_get_iter_at_widget_point(gui, x, y, &iter)) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
        GtkTextTag *wiki_tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buf), "wiki-link");
        is_link = wiki_tag && gtk_text_iter_has_tag(&iter, wiki_tag);
    }

    gtk_widget_set_cursor_from_name(gui->source_view, is_link ? "pointer" : "text");
}
