#include <gtk/gtk.h>
#include <string.h>
#include "gui_internal.h"

/* ── Outline panel ──
 * Heading TOC in the sidebar (above the Notes tree). Rebuilt by the same
 * debounced pass as the word count, so it costs nothing extra per
 * keystroke. Click → jump to the heading line. */

static void on_outline_row_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    AppGui *gui = global_gui;
    if (!gui || !gui->source_view) return;
    int line = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "heading-line"));
    gtk_stack_set_visible_child_name(GTK_STACK(gui->stack), "editor");
    gui_set_cursor_position(line + 1, 0);
    gtk_widget_grab_focus(gui->source_view);
}

void gui_outline_refresh(AppGui *gui) {
    if (!gui || !gui->outline_box || !gui->source_view) return;

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(gui->outline_box)) != NULL)
        gtk_box_remove(GTK_BOX(gui->outline_box), child);

    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
    int line_count = gtk_text_buffer_get_line_count(buf);
    int shown = 0;

    for (int i = 0; i < line_count && shown < 200; i++) {
        GtkTextIter ls, le;
        gtk_text_buffer_get_iter_at_line(buf, &ls, i);
        le = ls;
        if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
        gchar *text = gtk_text_buffer_get_text(buf, &ls, &le, TRUE);

        int level = 0;
        while (text[level] == '#') level++;
        if (level >= 1 && level <= 6 && text[level] == ' ' && text[level + 1] != '\0') {
            GtkWidget *btn = gtk_button_new();
            gtk_widget_add_css_class(btn, "tree-row");
            GtkWidget *lbl = gtk_label_new(text + level + 1);
            gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
            gtk_widget_set_halign(lbl, GTK_ALIGN_START);
            gtk_widget_add_css_class(lbl, "tree-row-label");
            gtk_button_set_child(GTK_BUTTON(btn), lbl);
            gtk_widget_set_margin_start(btn, (level - 1) * 12);
            g_object_set_data(G_OBJECT(btn), "heading-line", GINT_TO_POINTER(i));
            g_signal_connect(btn, "clicked", G_CALLBACK(on_outline_row_clicked), NULL);
            gtk_box_append(GTK_BOX(gui->outline_box), btn);
            shown++;
        }
        g_free(text);
    }

    /* The outline panel is now user-toggled (header ≡ / × button) and lives
     * in a resizable paned, so we no longer auto-hide it on empty headings —
     * doing so fought the user's toggle and the paned's saved size. */
    (void)shown;
}
