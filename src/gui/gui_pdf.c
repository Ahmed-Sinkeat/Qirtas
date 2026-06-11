#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include "gui_internal.h"

static gboolean on_print_paginate(GtkPrintOperation *operation, GtkPrintContext *context, gpointer user_data) {
    GtkSourcePrintCompositor *compositor = GTK_SOURCE_PRINT_COMPOSITOR(user_data);
    if (gtk_source_print_compositor_paginate(compositor, context)) {
        int n_pages = gtk_source_print_compositor_get_n_pages(compositor);
        gtk_print_operation_set_n_pages(operation, n_pages);
        return TRUE;
    }
    return FALSE;
}

static void on_print_draw_page(GtkPrintOperation *operation, GtkPrintContext *context, gint page_nr, gpointer user_data) {
    (void)operation;
    GtkSourcePrintCompositor *compositor = GTK_SOURCE_PRINT_COMPOSITOR(user_data);
    gtk_source_print_compositor_draw_page(compositor, context, page_nr);
}

static void on_print_end(GtkPrintOperation *operation, GtkPrintContext *context, gpointer user_data) {
    (void)operation;
    (void)context;
    GtkSourcePrintCompositor *compositor = GTK_SOURCE_PRINT_COMPOSITOR(user_data);
    g_object_unref(compositor);
}

static void do_pdf_export(AppGui *gui, const char *pdf_path) {
    if (!gui || !gui->source_view) return;

    GtkSourceView *source_view = GTK_SOURCE_VIEW(gui->source_view);
    GtkSourcePrintCompositor *compositor = gtk_source_print_compositor_new_from_view(source_view);

    // Retain wrap mode and highlight syntax settings
    GtkWrapMode wrap_mode = gtk_text_view_get_wrap_mode(GTK_TEXT_VIEW(source_view));
    gtk_source_print_compositor_set_wrap_mode(compositor, wrap_mode);
    gtk_source_print_compositor_set_highlight_syntax(compositor, TRUE);

    // Configure header / footer
    gtk_source_print_compositor_set_print_header(compositor, TRUE);
    gtk_source_print_compositor_set_print_footer(compositor, TRUE);

    GtkPrintOperation *operation = gtk_print_operation_new();
    gtk_print_operation_set_export_filename(operation, pdf_path);

    g_signal_connect(operation, "paginate", G_CALLBACK(on_print_paginate), compositor);
    g_signal_connect(operation, "draw-page", G_CALLBACK(on_print_draw_page), compositor);
    g_signal_connect(operation, "end-print", G_CALLBACK(on_print_end), compositor);

    GError *error = NULL;
    GtkPrintOperationResult result = gtk_print_operation_run(operation,
                                                            GTK_PRINT_OPERATION_ACTION_EXPORT,
                                                            GTK_WINDOW(gui->window),
                                                            &error);

    if (result == GTK_PRINT_OPERATION_RESULT_ERROR) {
        g_warning("PDF Export Error: %s", error ? error->message : "Unknown error");
        g_clear_error(&error);
    }

    g_object_unref(operation);
}

static void on_pdf_save_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    AppGui *gui = (AppGui *)user_data;
    GError *error = NULL;
    GFile *file = gtk_file_dialog_save_finish(dialog, res, &error);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            do_pdf_export(gui, path);
            g_free(path);
        }
        g_object_unref(file);
    } else if (error) {
        g_clear_error(&error);
    }
}

void qirtas_export_to_pdf(AppGui *gui) {
    if (!gui || !gui->window) return;

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Export to PDF");
    gtk_file_dialog_set_initial_name(dialog, "document.pdf");

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "PDF Documents");
    gtk_file_filter_add_pattern(filter, "*.pdf");
    gtk_file_filter_add_mime_type(filter, "application/pdf");

    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    g_object_unref(filter);

    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(filters);

    gtk_file_dialog_save(dialog, GTK_WINDOW(gui->window), NULL, on_pdf_save_response, gui);
}
