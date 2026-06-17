#include <gtk/gtk.h>
#include <adwaita.h>
#include <string.h>
#include "gui_internal.h"

/* File/vault/save-as dialogs, the unified add/open popover, folder-creation
 * prompt, and app/window shutdown + unsaved-changes confirmation. Extracted
 * from gui.c. Public entry points declared in gui_internal.h; the two response
 * handlers below stay file-local. */

static void on_close_confirm_response(AdwAlertDialog *dlg, const char *response, gpointer user_data);
static void on_folder_dialog_response(AdwAlertDialog *dlg, const char *response, gpointer user_data);

void on_open_dialog_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(dialog, res, &error);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            zig_open_file(path);
            g_free(path);
        }
        g_object_unref(file);
    } else if (error) {
        g_clear_error(&error);
    }
}

void on_vault_dialog_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    AppGui *gui = (AppGui *)user_data;
    GError *error = NULL;
    GFile *folder = gtk_file_dialog_select_folder_finish(dialog, res, &error);
    if (folder) {
        char *path = g_file_get_path(folder);
        if (path) {
            /* Drop the previous vault's tabs BEFORE the new vault loads — they
             * point at files in the old vault and would otherwise resolve
             * against the new vault's cwd and create empty files there. */
            gui_tabs_close_all(gui);
            zig_open_vault(path);
            if (gui->vault_path_lbl_val) {
                gtk_label_set_text(GTK_LABEL(gui->vault_path_lbl_val), path);
            }
            g_free(path);
        }
        g_object_unref(folder);
    } else if (error) {
        g_clear_error(&error);
    }
}

void on_open_vault_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppGui *gui = (AppGui *)user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Vault Directory");
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(gui->settings_window), NULL, on_vault_dialog_response, gui);
}

void on_open_existing_clicked(GtkButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Existing File");
    gtk_file_dialog_open(dialog, GTK_WINDOW(gui->window), NULL, on_open_dialog_response, gui);
    
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
}

void on_create_submit(GtkEntry *entry, gpointer user_data) {
    AddPopoverWidgets *w = (AddPopoverWidgets *)user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (text && strlen(text) > 0) {
        zig_create_new_file(text);
    }
    gtk_popover_popdown(GTK_POPOVER(w->popover));
}

void on_create_new_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AddPopoverWidgets *w = (AddPopoverWidgets *)user_data;
    gtk_widget_set_visible(w->box_actions, FALSE);
    gtk_widget_set_visible(w->box_input, TRUE);
    gtk_widget_grab_focus(w->entry_name);
}

void on_popover_closed(GtkPopover *popover, gpointer user_data) {
    (void)popover;
    AddPopoverWidgets *w = (AddPopoverWidgets *)user_data;
    gtk_widget_set_visible(w->box_actions, TRUE);
    gtk_widget_set_visible(w->box_input, FALSE);
    gtk_editable_set_text(GTK_EDITABLE(w->entry_name), "");
}

void on_app_shutdown(GApplication *app, gpointer user_data) {
    (void)app; (void)user_data;
    zig_on_shutdown();
}

static void on_close_confirm_response(AdwAlertDialog *dlg, const char *response, gpointer user_data) {
    (void)dlg;
    AppGui *gui = (AppGui *)user_data;
    if (!gui) return;
    if (strcmp(response, "save") == 0) {
        gui_manual_save(gui);
        gtk_window_destroy(GTK_WINDOW(gui->window));   /* bypasses close-request */
    } else if (strcmp(response, "discard") == 0) {
        gui_set_buffer_modified(FALSE);
        gtk_window_destroy(GTK_WINDOW(gui->window));
    }
    /* "cancel": keep the window open, do nothing. */
}

static void on_folder_dialog_response(AdwAlertDialog *dlg, const char *response, gpointer user_data) {
    GtkEntry *entry = GTK_ENTRY(user_data);
    if (strcmp(response, "create") == 0) {
        const char *name = gtk_editable_get_text(GTK_EDITABLE(entry));
        if (name && name[0]) {
            extern void zig_create_folder(const char *name);
            const char *parent = g_object_get_data(G_OBJECT(dlg), "parent_dir");
            if (parent && parent[0]) {
                char *full = g_strdup_printf("%s/%s", parent, name);
                zig_create_folder(full);
                g_free(full);
            } else {
                zig_create_folder(name);
            }
        }
    }
}

void prompt_new_folder(AppGui *gui, const char *parent_dir) {
    if (!gui) return;
    AdwAlertDialog *dlg = ADW_ALERT_DIALOG(
        adw_alert_dialog_new(qirtas_tr("New Folder"), NULL));
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), qirtas_tr("Folder name"));
    adw_alert_dialog_set_extra_child(dlg, entry);
    adw_alert_dialog_add_responses(dlg,
        "cancel", qirtas_tr("Cancel"),
        "create", qirtas_tr("Create"), NULL);
    adw_alert_dialog_set_response_appearance(dlg, "create", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(dlg, "create");
    adw_alert_dialog_set_close_response(dlg, "cancel");
    if (parent_dir)
        g_object_set_data_full(G_OBJECT(dlg), "parent_dir", g_strdup(parent_dir), g_free);
    g_signal_connect(dlg, "response", G_CALLBACK(on_folder_dialog_response), entry);
    adw_dialog_present(ADW_DIALOG(dlg), gui->window);
}

gboolean on_window_close_request(GtkWindow *window, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    /* Save the window geometry while the window is still realized — covers the
     * normal close and the unsaved-changes prompt path (both enter here). */
    gui_save_window_geometry(gui);
    /* Unsaved changes → prompt Save / Discard / Cancel and block the close
     * until the user decides. Cleanup itself runs from the shutdown signal. */
    if (gui && gui_get_buffer_modified()) {
        AdwAlertDialog *dlg = ADW_ALERT_DIALOG(
            adw_alert_dialog_new(qirtas_tr("Unsaved changes"),
                                 qirtas_tr("Save your changes before closing?")));
        adw_alert_dialog_add_responses(dlg,
            "cancel",  qirtas_tr("Cancel"),
            "discard", qirtas_tr("Discard"),
            "save",    qirtas_tr("Save"), NULL);
        adw_alert_dialog_set_response_appearance(dlg, "discard", ADW_RESPONSE_DESTRUCTIVE);
        adw_alert_dialog_set_response_appearance(dlg, "save", ADW_RESPONSE_SUGGESTED);
        adw_alert_dialog_set_default_response(dlg, "save");
        adw_alert_dialog_set_close_response(dlg, "cancel");
        g_signal_connect(dlg, "response", G_CALLBACK(on_close_confirm_response), gui);
        adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(window));
        return TRUE;   /* block; the response handler destroys when ready */
    }
    return FALSE;       /* clean → allow close, shutdown signal does cleanup */
}

void on_save_as_dialog_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    AppGui *gui = (AppGui *)user_data;
    GError *error = NULL;
    GFile *file = gtk_file_dialog_save_finish(dialog, res, &error);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
            GtkTextIter start, end;
            gtk_text_buffer_get_bounds(buf, &start, &end);
            char *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
            if (text) {
                FILE *f = fopen(path, "w");
                if (f) {
                    fputs(text, f);
                    fclose(f);
                    gui_set_sync_status("Saved");
                    gui_show_toast(qirtas_tr("Saved"));
                    zig_open_file(path);
                } else {
                    gui_set_sync_status("Save As Failed");
                    gui_show_toast(qirtas_tr("Save failed"));
                }
                g_free(text);
            }
            g_free(path);
        }
        g_object_unref(file);
    } else if (error) {
        g_clear_error(&error);
    }
}

void trigger_save_as(AppGui *gui) {
    if (!gui || !gui->window) return;

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save As...");
    gtk_file_dialog_set_initial_name(dialog, "untitled.md");

    /* Default to the vault (current working directory) so the user doesn't
     * have to navigate away from their notes. */
    char *cwd = g_get_current_dir();
    GFile *vault_dir = g_file_new_for_path(cwd);
    gtk_file_dialog_set_initial_folder(dialog, vault_dir);
    g_object_unref(vault_dir);
    g_free(cwd);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Markdown Files");
    gtk_file_filter_add_pattern(filter, "*.md");
    gtk_file_filter_add_mime_type(filter, "text/markdown");

    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    g_object_unref(filter);

    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(filters);

    gtk_file_dialog_save(dialog, GTK_WINDOW(gui->window), NULL, on_save_as_dialog_response, gui);
}
