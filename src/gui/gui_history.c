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

void gui_history_record(const char *path) {
    if (!path || !*path || strcmp(path, "Untitled") == 0) return;

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
