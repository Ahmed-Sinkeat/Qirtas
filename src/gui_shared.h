#pragma once

#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <adwaita.h>

typedef void (*GuiIdleCallback)(void *user_data);

typedef struct {
    int line;
    int col;
} Position;

extern void zig_insert_text(Position pos, const char *text);
extern void zig_delete_range(Position start, Position end);
extern void zig_replace_range(Position start, Position end, const char *text);
extern void zig_undo_push(int cursor_line, int cursor_col);
extern void zig_undo_commit(void);
extern void zig_undo(void);
extern void zig_redo(void);
extern void zig_undo_clear(void);
extern int zig_save_document(void);
extern const char *zig_get_document_text(void);
extern void zig_free_document_text(const char *ptr);
/* Portable text logic (src/markdown.zig). */
extern int zig_detect_rtl(const char *text);          /* 1 = RTL paragraph, 0 = LTR */
extern int zig_heading_level(const char *line);       /* ATX heading level 1-6, else 0 */
extern int zig_fuzzy_score(const char *haystack, const char *needle); /* -1 = no match */
/* Build Arabic-tolerant search regex from NFKC-normalized input. Returns a
 * Zig-allocated string — free with zig_free_document_text, NOT g_free. */
extern char *zig_arabic_search_regex(const char *normalized_input);
/* Markdown table structure (src/markdown.zig). */
extern int zig_table_is_delimiter(const char *line);  /* 1 = delimiter row */
extern int zig_table_is_row(const char *line);         /* 1 = plausible table row */
extern int zig_table_aligns(const char *delim, int *out_codes, int max); /* per-col 0/1/2; returns count */
/* Fenced code blocks (src/markdown.zig). */
extern int zig_fence_only(const char *line);                          /* 1 = closing/bare fence */
extern int zig_code_fence_lang(const char *line, char *out, int max); /* 1 = opening fence; out = language */

/* Zig -> C FFI */
void gui_set_text(const char *text, int len);
void gui_set_title(const char *title);
/* Sync dot state. Enum, not strings — status text passed through
 * qirtas_tr would silently fail strcmp classification for Arabic users.
 * Translation happens at labels only; state travels as this enum. */
typedef enum {
    QIRTAS_SYNC_SYNCED = 0,
    QIRTAS_SYNC_SAVING = 1,
    QIRTAS_SYNC_NOT_SYNCED = 2,
} QirtasSyncState;
void gui_set_sync_state(QirtasSyncState state);
void gui_show_editor(void);
void gui_show_recovery_dialog(void);
void gui_get_cursor_position(int *line, int *col);
void gui_set_cursor_position(int line, int col);
void gui_refresh_explorer(void);
void gui_set_virtual_scroll_mode(int enabled, int total_lines);
void gui_update_sync_status(int connected, const char *status_text);
void gui_update_dropbox_status(int connected, const char *status_text);
void gui_update_github_status(int connected, const char *status_text);
void gui_update_local_sync_status(int connected, const char *status_text);

/* C -> Zig FFI */
extern void zig_on_gui_ready(void);
extern int zig_has_active_master_key(void);
extern void zig_open_file(const char *filename);
extern void zig_open_vault(const char *dir_path);
extern void zig_search_workspace(const char *query);
extern const char *zig_get_search_snippet(const char *filepath);
extern int zig_get_search_rank(const char *filepath);
extern void zig_set_cursor_trail(int enabled);
extern int zig_get_cursor_trail(void);
extern void zig_open_wiki_link(const char *note_name);
extern void zig_create_new_file(const char *filename);
extern void zig_on_shutdown(void);
extern void zig_force_save(void);
extern void zig_save_sync_credentials(const char *client_id, const char *client_secret);
extern void zig_sync_connect(void);
extern void zig_sync_submit_code(const char *code);
extern void zig_sync_now(void);
extern int zig_sync_check_status(void);
extern void zig_sync_disconnect(void);
extern void zig_save_dropbox_credentials(const char *client_id, const char *client_secret);
extern void zig_dropbox_connect(void);
extern void zig_dropbox_submit_code(const char *code);
extern void zig_dropbox_now(void);
extern int zig_dropbox_check_status(void);
extern void zig_dropbox_disconnect(void);
extern void zig_save_github_credentials(const char *token, const char *repo);
extern int zig_get_github_credentials_decrypted(char *token_buf, int token_buf_max, char *repo_buf, int repo_buf_max);
extern int zig_get_dropbox_credentials_decrypted(char *client_id_buf, int client_id_max, char *client_secret_buf, int client_secret_max);
extern void zig_github_now(void);
extern void zig_github_connect_with_token(const char *token, const char *repo);
extern int zig_github_check_status(void);
extern void zig_github_disconnect(void);
extern void zig_local_sync_now(void);
extern unsigned long zig_pkce_challenge(char *out, unsigned long out_max);
extern void zig_set_editor_border(int enabled);
extern int zig_get_editor_border(void);
