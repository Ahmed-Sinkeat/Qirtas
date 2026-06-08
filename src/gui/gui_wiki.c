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

void apply_wiki_link_tags(GtkTextBuffer *buf) {
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
