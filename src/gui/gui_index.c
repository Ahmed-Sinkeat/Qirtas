#include <gtk/gtk.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include "gui_internal.h"

/* SQLite FTS indexing for the in-app search.
 * Called over FFI from the Zig side (sync.zig / main.zig) — signatures must
 * stay identical. No GTK widget access here; pure file IO + sqlite. */

void gui_index_all_files(void) {
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }

    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS file_metadata (filepath TEXT PRIMARY KEY, last_modified INTEGER NOT NULL, drive_file_id TEXT);", NULL, NULL, NULL);
    sqlite3_exec(db, "ALTER TABLE file_metadata ADD COLUMN drive_file_id TEXT;", NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE VIRTUAL TABLE IF NOT EXISTS note_search USING fts5(filepath UNINDEXED, title, content);", NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE VIRTUAL TABLE IF NOT EXISTS file_content_fts USING fts5(content, filepath UNINDEXED);", NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS sync_tokens (id INTEGER PRIMARY KEY CHECK (id = 1), client_id TEXT NOT NULL, client_secret TEXT NOT NULL, access_token TEXT, refresh_token TEXT, expiry_time INTEGER DEFAULT 0);", NULL, NULL, NULL);

    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS dropbox_sync_tokens (id INTEGER PRIMARY KEY CHECK (id = 1), client_id TEXT NOT NULL, client_secret TEXT NOT NULL, access_token TEXT, refresh_token TEXT, expiry_time INTEGER DEFAULT 0);", NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS github_sync_tokens (id INTEGER PRIMARY KEY CHECK (id = 1), personal_token TEXT, repo_name TEXT NOT NULL, access_token TEXT, expiry_time INTEGER DEFAULT 0);", NULL, NULL, NULL);

    sqlite3_stmt *stmt_cnt = NULL;
    int fts_count = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM file_content_fts;", -1, &stmt_cnt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt_cnt) == SQLITE_ROW) {
            fts_count = sqlite3_column_int(stmt_cnt, 0);
        }
        sqlite3_finalize(stmt_cnt);
    }
    if (fts_count == 0) {
        sqlite3_exec(db, "DELETE FROM file_metadata;", NULL, NULL, NULL);
    }

    GError *err = NULL;
    GDir *dir = g_dir_open(".", 0, &err);
    if (!dir) {
        sqlite3_close(db);
        return;
    }

    const char *nm;
    while ((nm = g_dir_read_name(dir)) != NULL) {
        if (nm[0] == '.') continue;

        gboolean is_md = g_str_has_suffix(nm, ".md") || g_str_has_suffix(nm, ".txt") ||
                         g_str_has_suffix(nm, ".zig") || g_str_has_suffix(nm, ".zon") ||
                         g_str_has_suffix(nm, ".c") || g_str_has_suffix(nm, ".h");
        if (!is_md) continue;

        struct stat st;
        if (stat(nm, &st) != 0) continue;
        long long last_mod = st.st_mtime;

        sqlite3_stmt *stmt_check = NULL;
        const char *sql_check = "SELECT last_modified FROM file_metadata WHERE filepath = ?;";
        gboolean up_to_date = FALSE;
        if (sqlite3_prepare_v2(db, sql_check, -1, &stmt_check, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt_check, 1, nm, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt_check) == SQLITE_ROW) {
                long long db_last_mod = sqlite3_column_int64(stmt_check, 0);
                if (db_last_mod == last_mod) {
                    up_to_date = TRUE;
                }
            }
            sqlite3_finalize(stmt_check);
        }

        if (up_to_date) continue;

        FILE *f = fopen(nm, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz < 0) { fclose(f); continue; }
        char *content = g_malloc(sz + 1);
        size_t read_bytes = fread(content, 1, sz, f);
        content[read_bytes] = '\0';
        fclose(f);

        sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);

        sqlite3_stmt *stmt_del = NULL;
        if (sqlite3_prepare_v2(db, "DELETE FROM note_search WHERE filepath = ?;", -1, &stmt_del, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt_del, 1, nm, -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt_del);
            sqlite3_finalize(stmt_del);
        }

        sqlite3_stmt *stmt_del_fts = NULL;
        if (sqlite3_prepare_v2(db, "DELETE FROM file_content_fts WHERE filepath = ?;", -1, &stmt_del_fts, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt_del_fts, 1, nm, -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt_del_fts);
            sqlite3_finalize(stmt_del_fts);
        }

        sqlite3_stmt *stmt_ins = NULL;
        if (sqlite3_prepare_v2(db, "INSERT INTO note_search (filepath, title, content) VALUES (?, ?, ?);", -1, &stmt_ins, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt_ins, 1, nm, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt_ins, 2, nm, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt_ins, 3, content, -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt_ins);
            sqlite3_finalize(stmt_ins);
        }

        sqlite3_stmt *stmt_ins_fts = NULL;
        if (sqlite3_prepare_v2(db, "INSERT INTO file_content_fts (content, filepath) VALUES (?, ?);", -1, &stmt_ins_fts, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt_ins_fts, 1, content, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt_ins_fts, 2, nm, -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt_ins_fts);
            sqlite3_finalize(stmt_ins_fts);
        }

        sqlite3_stmt *stmt_meta = NULL;
        if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO file_metadata (filepath, last_modified) VALUES (?, ?);", -1, &stmt_meta, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt_meta, 1, nm, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt_meta, 2, last_mod);
            sqlite3_step(stmt_meta);
            sqlite3_finalize(stmt_meta);
        }

        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
        g_free(content);
    }

    g_dir_close(dir);
    sqlite3_close(db);
}

void gui_index_file(const char *filename) {
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }

    struct stat st;
    if (stat(filename, &st) != 0) {
        sqlite3_close(db);
        return;
    }
    long long last_mod = st.st_mtime;

    FILE *f = fopen(filename, "rb");
    if (!f) {
        sqlite3_close(db);
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); sqlite3_close(db); return; }
    char *content = g_malloc(sz + 1);
    size_t read_bytes = fread(content, 1, sz, f);
    content[read_bytes] = '\0';
    fclose(f);

    sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);

    sqlite3_stmt *stmt_del = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM note_search WHERE filepath = ?;", -1, &stmt_del, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt_del, 1, filename, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt_del);
        sqlite3_finalize(stmt_del);
    }

    sqlite3_stmt *stmt_del_fts = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM file_content_fts WHERE filepath = ?;", -1, &stmt_del_fts, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt_del_fts, 1, filename, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt_del_fts);
        sqlite3_finalize(stmt_del_fts);
    }

    sqlite3_stmt *stmt_ins = NULL;
    if (sqlite3_prepare_v2(db, "INSERT INTO note_search (filepath, title, content) VALUES (?, ?, ?);", -1, &stmt_ins, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt_ins, 1, filename, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt_ins, 2, filename, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt_ins, 3, content, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt_ins);
        sqlite3_finalize(stmt_ins);
    }

    sqlite3_stmt *stmt_ins_fts = NULL;
    if (sqlite3_prepare_v2(db, "INSERT INTO file_content_fts (content, filepath) VALUES (?, ?);", -1, &stmt_ins_fts, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt_ins_fts, 1, content, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt_ins_fts, 2, filename, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt_ins_fts);
        sqlite3_finalize(stmt_ins_fts);
    }

    sqlite3_stmt *stmt_meta = NULL;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO file_metadata (filepath, last_modified) VALUES (?, ?);", -1, &stmt_meta, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt_meta, 1, filename, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt_meta, 2, last_mod);
        sqlite3_step(stmt_meta);
        sqlite3_finalize(stmt_meta);
    }

    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    g_free(content);
    sqlite3_close(db);
}

void gui_remove_file_from_index(const char *filename) {
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }

    sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);

    sqlite3_stmt *stmt_del = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM note_search WHERE filepath = ?;", -1, &stmt_del, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt_del, 1, filename, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt_del);
        sqlite3_finalize(stmt_del);
    }

    sqlite3_stmt *stmt_del_fts = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM file_content_fts WHERE filepath = ?;", -1, &stmt_del_fts, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt_del_fts, 1, filename, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt_del_fts);
        sqlite3_finalize(stmt_del_fts);
    }

    sqlite3_stmt *stmt_meta = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM file_metadata WHERE filepath = ?;", -1, &stmt_meta, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt_meta, 1, filename, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt_meta);
        sqlite3_finalize(stmt_meta);
    }

    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_close(db);
}
