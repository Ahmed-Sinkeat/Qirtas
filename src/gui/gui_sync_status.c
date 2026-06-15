#include <gtk/gtk.h>
#include <string.h>
#include "gui_internal.h"

/* Sync-status UI reporting. Maps backend status strings/connection states
 * (Google Drive, Dropbox, GitHub, local) onto the status labels, connect/sync
 * buttons, and the global sync badge. The network/OAuth side lives in
 * gui_sync.c; this file only touches widgets. gui_update_* are called over FFI
 * from the Zig sync layer and dispatch onto the main thread. */

void gui_set_sync_state(QirtasSyncState state) {
    if (!global_sync_label) return;

    gtk_widget_remove_css_class(global_sync_label, "status-saved");
    gtk_widget_remove_css_class(global_sync_label, "status-saving");
    gtk_widget_remove_css_class(global_sync_label, "status-failed");

    switch (state) {
    case QIRTAS_SYNC_SYNCED:
        gtk_widget_add_css_class(global_sync_label, "status-saved");
        break;
    case QIRTAS_SYNC_SAVING:
        gtk_widget_add_css_class(global_sync_label, "status-saving");
        break;
    default:
        gtk_widget_add_css_class(global_sync_label, "status-failed");
        break;
    }
}

static gboolean sync_status_is_busy(const char *status_text) {
    return status_text &&
           (strcmp(status_text, "Syncing...") == 0 ||
            strcmp(status_text, "Exchanging code...") == 0 ||
            strcmp(status_text, "Connecting...") == 0);
}

static gboolean sync_status_is_success(const char *status_text) {
    return status_text &&
           (strcmp(status_text, "Saved") == 0 ||
            strcmp(status_text, "Synced") == 0 ||
            strcmp(status_text, "Synced ✓") == 0 ||
            strcmp(status_text, "Updated") == 0 ||
            strcmp(status_text, "Connected") == 0);
}

static gboolean sync_status_is_error(const char *status_text) {
    return status_text &&
           (g_str_has_prefix(status_text, "Error:") ||
            strcmp(status_text, "Authentication Failed") == 0 ||
            strcmp(status_text, "Auth Expired (Reconnect)") == 0 ||
            strcmp(status_text, "Offline") == 0 ||
            strcmp(status_text, "Sync Failed") == 0);
}

static void set_sync_status_label(GtkWidget *label_widget, const char *status_text) {
    if (!label_widget || !status_text) return;

    gtk_label_set_text(GTK_LABEL(label_widget), status_text);
    gtk_widget_remove_css_class(label_widget, "status-saved");
    gtk_widget_remove_css_class(label_widget, "status-saving");
    gtk_widget_remove_css_class(label_widget, "status-failed");

    if (sync_status_is_busy(status_text)) {
        gtk_widget_add_css_class(label_widget, "status-saving");
    } else if (sync_status_is_success(status_text)) {
        gtk_widget_add_css_class(label_widget, "status-saved");
    } else if (sync_status_is_error(status_text)) {
        gtk_widget_add_css_class(label_widget, "status-failed");
    }
}

typedef struct {
    int connected;
    char *status_text;
} SyncStatusUpdate;

static void update_sync_status_callback(void *user_data) {
    SyncStatusUpdate *up = (SyncStatusUpdate *)user_data;
    if (global_gui) {
        if (global_gui->sync_status_lbl) {
            set_sync_status_label(global_gui->sync_status_lbl, up->status_text);
        }

        if (up->connected == 2) {
            if (global_gui->sync_code_box) {
                gtk_widget_set_visible(global_gui->sync_code_box, TRUE);
            }
            if (global_gui->sync_now_btn) {
                gtk_widget_set_visible(global_gui->sync_now_btn, FALSE);
                gtk_widget_set_sensitive(global_gui->sync_now_btn, FALSE);
            }
        } else {
            if (global_gui->sync_code_box) {
                gtk_widget_set_visible(global_gui->sync_code_box, FALSE);
            }
            if (up->connected == 1) {
                if (global_gui->sync_connect_btn) {
                    gtk_button_set_label(GTK_BUTTON(global_gui->sync_connect_btn), "Disconnect");
                }
                if (global_gui->sync_now_btn) {
                    gtk_widget_set_visible(global_gui->sync_now_btn, TRUE);
                    gtk_widget_set_sensitive(global_gui->sync_now_btn, TRUE);
                }
            } else {
                if (global_gui->sync_connect_btn) {
                    gtk_button_set_label(GTK_BUTTON(global_gui->sync_connect_btn), "Connect to Google Drive");
                }
                if (global_gui->sync_now_btn) {
                    gtk_widget_set_visible(global_gui->sync_now_btn, FALSE);
                    gtk_widget_set_sensitive(global_gui->sync_now_btn, FALSE);
                }
            }
        }

        if (up->connected == 1) {
            gui_set_sync_status("Synced");
        } else if (up->connected == 2 && sync_status_is_busy(up->status_text)) {
            gui_set_sync_status("Saving...");
        } else {
            gui_set_sync_status("Not Synced");
        }
    }
    g_free(up->status_text);
    g_free(up);
}

void gui_update_sync_status(int connected, const char *status_text) {
    SyncStatusUpdate *up = g_new0(SyncStatusUpdate, 1);
    up->connected = connected;
    up->status_text = g_strdup(status_text);
    gui_run_on_main_thread(update_sync_status_callback, up);
}

static void update_dropbox_status_callback(void *user_data) {
    SyncStatusUpdate *up = (SyncStatusUpdate *)user_data;
    if (global_gui) {
        if (global_gui->dropbox_status_lbl) {
            set_sync_status_label(global_gui->dropbox_status_lbl, up->status_text);
        }

        if (up->connected == 2) {
            if (global_gui->dropbox_code_box) {
                gtk_widget_set_visible(global_gui->dropbox_code_box, TRUE);
            }
            if (global_gui->dropbox_now_btn) {
                gtk_widget_set_visible(global_gui->dropbox_now_btn, FALSE);
                gtk_widget_set_sensitive(global_gui->dropbox_now_btn, FALSE);
            }
        } else {
            if (global_gui->dropbox_code_box) {
                gtk_widget_set_visible(global_gui->dropbox_code_box, FALSE);
            }
            if (up->connected == 1) {
                if (global_gui->dropbox_connect_btn) {
                    gtk_button_set_label(GTK_BUTTON(global_gui->dropbox_connect_btn), "Disconnect");
                }
                if (global_gui->dropbox_now_btn) {
                    gtk_widget_set_visible(global_gui->dropbox_now_btn, TRUE);
                    gtk_widget_set_sensitive(global_gui->dropbox_now_btn, TRUE);
                }
            } else {
                if (global_gui->dropbox_connect_btn) {
                    gtk_button_set_label(GTK_BUTTON(global_gui->dropbox_connect_btn), "Connect to Dropbox");
                }
                if (global_gui->dropbox_now_btn) {
                    gtk_widget_set_visible(global_gui->dropbox_now_btn, FALSE);
                    gtk_widget_set_sensitive(global_gui->dropbox_now_btn, FALSE);
                }
            }
        }

        if (sync_status_is_busy(up->status_text)) {
            gui_set_sync_status("Saving...");
        } else if (sync_status_is_success(up->status_text)) {
            gui_set_sync_status("Synced");
        } else if (sync_status_is_error(up->status_text)) {
            gui_set_sync_status("Not Synced");
        }
    }
    g_free(up->status_text);
    g_free(up);
}

void gui_update_dropbox_status(int connected, const char *status_text) {
    SyncStatusUpdate *up = g_new0(SyncStatusUpdate, 1);
    up->connected = connected;
    up->status_text = g_strdup(status_text);
    gui_run_on_main_thread(update_dropbox_status_callback, up);
}

static void update_github_status_callback(void *user_data) {
    SyncStatusUpdate *up = (SyncStatusUpdate *)user_data;
    if (global_gui) {
        if (global_gui->github_status_lbl) {
            set_sync_status_label(global_gui->github_status_lbl, up->status_text);
        }

        if (up->connected == 1) {
            if (global_gui->github_connect_btn) {
                gtk_button_set_label(GTK_BUTTON(global_gui->github_connect_btn), "Disconnect");
            }
            if (global_gui->github_now_btn) {
                gtk_widget_set_visible(global_gui->github_now_btn, TRUE);
                gtk_widget_set_sensitive(global_gui->github_now_btn, TRUE);
            }
        } else {
            if (global_gui->github_connect_btn) {
                gtk_button_set_label(GTK_BUTTON(global_gui->github_connect_btn), "Connect to GitHub");
            }
            if (global_gui->github_now_btn) {
                gtk_widget_set_visible(global_gui->github_now_btn, FALSE);
                gtk_widget_set_sensitive(global_gui->github_now_btn, FALSE);
            }
        }

        if (sync_status_is_busy(up->status_text)) {
            gui_set_sync_status("Saving...");
        } else if (sync_status_is_success(up->status_text)) {
            gui_set_sync_status("Synced");
        } else if (sync_status_is_error(up->status_text)) {
            gui_set_sync_status("Not Synced");
        }
    }
    g_free(up->status_text);
    g_free(up);
}

void gui_update_github_status(int connected, const char *status_text) {
    SyncStatusUpdate *up = g_new0(SyncStatusUpdate, 1);
    up->connected = connected;
    up->status_text = g_strdup(status_text);
    gui_run_on_main_thread(update_github_status_callback, up);
}

static void update_local_sync_status_callback(void *user_data) {
    SyncStatusUpdate *up = (SyncStatusUpdate *)user_data;
    if (global_gui) {
        if (global_gui->local_sync_status_lbl) {
            set_sync_status_label(global_gui->local_sync_status_lbl, up->status_text);
        }
        if (global_gui->local_sync_btn) {
            gtk_widget_set_sensitive(global_gui->local_sync_btn, up->connected != 2);
        }

        if (up->connected == 2) {
            gui_set_sync_status("Saving...");
        } else if (up->connected == 1) {
            gui_set_sync_status("Synced");
        } else if (sync_status_is_error(up->status_text)) {
            gui_set_sync_status("Not Synced");
        } else {
            gui_set_sync_status("Not Synced");
        }
    }
    g_free(up->status_text);
    g_free(up);
}

void gui_update_local_sync_status(int connected, const char *status_text) {
    SyncStatusUpdate *up = g_new0(SyncStatusUpdate, 1);
    up->connected = connected;
    up->status_text = g_strdup(status_text);
    gui_run_on_main_thread(update_local_sync_status_callback, up);
}
