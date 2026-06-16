#include "gui_internal.h"
#include <sqlite3.h>
#include <string.h>
#include <time.h>

/* ============================================================
 * FILE HISTORY — crash-recovery snapshots
 *
 * Called after every successful autosave (gui_trigger_autosave).
 * Stores a full snapshot of the document in the vault DB, at most
 * one per HISTORY_MIN_INTERVAL_SEC per file, skipping unchanged
 * content. Pruning keeps everything from the last 24h, one per
 * hour for 7 days, one per day for 30 days.
 * ============================================================ */

#define HISTORY_MIN_INTERVAL_SEC 300

extern const char *zig_get_document_text(void);
extern void zig_free_document_text(const char *ptr);
extern const char *zig_history_encrypt(const char *plaintext);
extern const char *zig_history_decrypt(const char *hex_blob);
extern void zig_open_file(const char *filename);

static int history_ensure_table(sqlite3 *db) {
    const char *sql =
        "CREATE TABLE IF NOT EXISTS file_history ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  path TEXT NOT NULL,"
        "  ts INTEGER NOT NULL,"
        "  content TEXT NOT NULL);"
        "CREATE INDEX IF NOT EXISTS idx_file_history_path_ts"
        "  ON file_history(path, ts);";
    return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static void history_prune(sqlite3 *db) {
    /* Older than 30 days: drop. */
    sqlite3_exec(db,
        "DELETE FROM file_history WHERE ts < strftime('%s','now','-30 days');",
        NULL, NULL, NULL);
    /* 7-30 days old: keep one per day per file. */
    sqlite3_exec(db,
        "DELETE FROM file_history WHERE ts < strftime('%s','now','-7 days')"
        " AND id NOT IN (SELECT MAX(id) FROM file_history"
        "   WHERE ts < strftime('%s','now','-7 days')"
        "   GROUP BY path, strftime('%Y%m%d', ts, 'unixepoch'));",
        NULL, NULL, NULL);
    /* 1-7 days old: keep one per hour per file. */
    sqlite3_exec(db,
        "DELETE FROM file_history WHERE ts < strftime('%s','now','-1 day')"
        " AND id NOT IN (SELECT MAX(id) FROM file_history"
        "   WHERE ts < strftime('%s','now','-1 day')"
        "   GROUP BY path, strftime('%Y%m%d%H', ts, 'unixepoch'));",
        NULL, NULL, NULL);
}

/* Strip leading "./" so "./foo.md" and "foo.md" map to the same key. */
static const char *norm_path(const char *p) {
    while (p[0] == '.' && p[1] == '/') p += 2;
    return p;
}

void gui_history_record(const char *path) {
    if (!path || !*path || strcmp(path, "Untitled") == 0) return;
    path = norm_path(path);

    const char *text = zig_get_document_text();
    if (!text) return;

    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        zig_free_document_text(text);
        return;
    }

    if (history_ensure_table(db) != SQLITE_OK) {
        sqlite3_close(db);
        zig_free_document_text(text);
        return;
    }

    /* Skip if last snapshot is recent or identical. Snapshot content is
     * stored encrypted (hex blob) with the vault master key — decrypt the
     * last one to compare against the current plaintext. */
    gboolean skip = FALSE;
    sqlite3_stmt *stmt = NULL;
    const char *q =
        "SELECT ts, content FROM file_history WHERE path = ?"
        " ORDER BY ts DESC, id DESC LIMIT 1;";
    if (sqlite3_prepare_v2(db, q, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            sqlite3_int64 last_ts = sqlite3_column_int64(stmt, 0);
            const unsigned char *last_enc = sqlite3_column_text(stmt, 1);
            sqlite3_int64 now = (sqlite3_int64)time(NULL);
            if (last_enc) {
                const char *last_pt = zig_history_decrypt((const char *)last_enc);
                if (last_pt) {
                    if (strcmp(last_pt, text) == 0) skip = TRUE;
                    zig_free_document_text(last_pt);
                }
            }
            if (now - last_ts < HISTORY_MIN_INTERVAL_SEC) skip = TRUE;
        }
        sqlite3_finalize(stmt);
    }

    if (!skip) {
        const char *enc = zig_history_encrypt(text);
        if (enc) {
            const char *ins =
                "INSERT INTO file_history (path, ts, content)"
                " VALUES (?, strftime('%s','now'), ?);";
            if (sqlite3_prepare_v2(db, ins, -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, enc, -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
            zig_free_document_text(enc);
            history_prune(db);
        }
    }

    sqlite3_close(db);
    zig_free_document_text(text);
}

/* ============================================================
 * FILE HISTORY — restore dialog
 * ============================================================ */

typedef struct {
    GtkWidget *window;
    char      *path;
} HistoryDialogData;

static void on_history_dialog_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    HistoryDialogData *hd = (HistoryDialogData *)user_data;
    g_free(hd->path);
    g_free(hd);
}

static void on_history_restore_response(AdwAlertDialog *dlg, const char *response, gpointer user_data) {
    (void)user_data;
    if (strcmp(response, "restore") != 0) return;

    const char *path = g_object_get_data(G_OBJECT(dlg), "history-path");
    const char *content = g_object_get_data(G_OBJECT(dlg), "history-content");
    GtkWidget *history_win = g_object_get_data(G_OBJECT(dlg), "history-window");
    if (!path || !content) return;

    zig_open_file(path);

    if (global_source_view) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));
        gtk_text_buffer_set_text(buf, content, -1);
        gtk_text_buffer_set_modified(buf, TRUE);
        gui_trigger_autosave();
    }

    if (history_win) gtk_window_destroy(GTK_WINDOW(history_win));
}

static void on_history_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box;
    HistoryDialogData *hd = (HistoryDialogData *)user_data;
    const char *content = g_object_get_data(G_OBJECT(row), "content");
    if (!content) return;

    AdwAlertDialog *dlg = ADW_ALERT_DIALOG(
        adw_alert_dialog_new(qirtas_tr("Restore this version?"),
            qirtas_tr("The current content of this file will be replaced with the selected snapshot. This cannot be undone.")));
    adw_alert_dialog_add_responses(dlg,
        "cancel",  qirtas_tr("Cancel"),
        "restore", qirtas_tr("Restore"), NULL);
    adw_alert_dialog_set_response_appearance(dlg, "restore", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(dlg, "cancel");
    adw_alert_dialog_set_close_response(dlg, "cancel");

    g_object_set_data_full(G_OBJECT(dlg), "history-path", g_strdup(hd->path), g_free);
    g_object_set_data_full(G_OBJECT(dlg), "history-content", g_strdup(content), g_free);
    g_object_set_data(G_OBJECT(dlg), "history-window", hd->window);

    g_signal_connect(dlg, "response", G_CALLBACK(on_history_restore_response), NULL);
    adw_dialog_present(ADW_DIALOG(dlg), hd->window);
}

/* Lists past snapshots for `path` (newest first) with a Restore-on-click
 * flow. Snapshots are recorded by gui_history_record() after each save. */
void show_file_history(AppGui *gui, const char *path) {
    if (!gui || !path || !*path) return;
    path = norm_path(path);

    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }
    history_ensure_table(db);

    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), qirtas_tr("File History"));
    gtk_window_set_default_size(GTK_WINDOW(win), 420, 420);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(gui->window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_widget_add_css_class(win, "settings-sheet-window");

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 10);
    gtk_widget_set_margin_end(vbox, 10);
    gtk_widget_set_margin_top(vbox, 10);
    gtk_widget_set_margin_bottom(vbox, 10);
    gtk_window_set_child(GTK_WINDOW(win), vbox);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(vbox), scroll);

    GtkWidget *listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), listbox);

    HistoryDialogData *hd = g_new0(HistoryDialogData, 1);
    hd->window = win;
    hd->path = g_strdup(path);

    int n_rows = 0;
    sqlite3_stmt *stmt = NULL;
    const char *q =
        "SELECT ts, content FROM file_history WHERE path = ?"
        " ORDER BY ts DESC, id DESC;";
    if (sqlite3_prepare_v2(db, q, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            sqlite3_int64 ts = sqlite3_column_int64(stmt, 0);
            const unsigned char *enc = sqlite3_column_text(stmt, 1);
            if (!enc) continue;
            const char *plain = zig_history_decrypt((const char *)enc);
            if (!plain) continue;

            GDateTime *dt = g_date_time_new_from_unix_local((gint64)ts);
            char *ts_str = g_date_time_format(dt, "%Y-%m-%d %H:%M");
            g_date_time_unref(dt);

            const char *nl = strchr(plain, '\n');
            gchar *line = nl ? g_strndup(plain, (gsize)(nl - plain)) : g_strdup(plain);
            glong line_chars = g_utf8_strlen(line, -1);
            gchar *preview;
            if (line_chars > 60) {
                gchar *cut = g_utf8_substring(line, 0, 60);
                preview = g_strdup_printf("%s\xe2\x80\xa6", cut); /* … */
                g_free(cut);
            } else {
                preview = g_strdup(line);
            }
            g_free(line);

            GtkWidget *row = gtk_list_box_row_new();
            GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_widget_set_margin_start(row_box, 10);
            gtk_widget_set_margin_end(row_box, 10);
            gtk_widget_set_margin_top(row_box, 6);
            gtk_widget_set_margin_bottom(row_box, 6);

            GtkWidget *ts_lbl = gtk_label_new(ts_str);
            gtk_widget_set_halign(ts_lbl, GTK_ALIGN_START);
            gtk_widget_add_css_class(ts_lbl, "heading");
            gtk_box_append(GTK_BOX(row_box), ts_lbl);

            GtkWidget *prev_lbl = gtk_label_new(preview);
            gtk_widget_set_halign(prev_lbl, GTK_ALIGN_START);
            gtk_label_set_ellipsize(GTK_LABEL(prev_lbl), PANGO_ELLIPSIZE_END);
            gtk_widget_add_css_class(prev_lbl, "dim-label");
            gtk_box_append(GTK_BOX(row_box), prev_lbl);

            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_box);
            g_object_set_data_full(G_OBJECT(row), "content", g_strdup(plain), g_free);
            gtk_list_box_append(GTK_LIST_BOX(listbox), row);

            g_free(ts_str);
            g_free(preview);
            zig_free_document_text(plain);
            n_rows++;
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);

    if (n_rows == 0) {
        GtkWidget *empty = gtk_label_new(qirtas_tr("No history yet for this file."));
        gtk_widget_set_margin_top(empty, 30);
        gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(empty, "dim-label");
        gtk_box_append(GTK_BOX(vbox), empty);
    }

    g_signal_connect(listbox, "row-activated", G_CALLBACK(on_history_row_activated), hd);
    g_signal_connect(win, "destroy", G_CALLBACK(on_history_dialog_destroy), hd);

    gtk_window_present(GTK_WINDOW(win));
}
