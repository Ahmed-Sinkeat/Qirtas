#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <adwaita.h>
#include <pango/pango.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <sqlite3.h>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>
#include "gui_internal.h"
#include "gui_shared.h"
/* ============================================================
 *  gui.c  —  Qirtas GTK4 UI Layer
 * ============================================================
 *
 *  MODULE MAP  (logical sections inside this file — see src/STRUCTURE.md)
 *  ─────────────────────────────────────────────────────────
 *  gui.c              →  Entry point, run_gui(), FFI surface to Zig
 *  gui_shared.h       →  Shared includes, AppGui struct, globals
 *  gui_editor.c       →  Text editing: format, paragraph, key bindings
 *  gui_conceal.c      →  Markdown concealment (invisible delimiters)
 *  gui_popover.c      →  Right-click formatting menu
 *  gui_explorer.c     →  File/vault tree explorer
 *  gui_theme.c        →  CSS loading, theme switching, font update
 *  gui_search.c       →  In-file search bar
 *  gui_settings.c     →  Settings window, shortcuts, export
 *  gui_cursor_trail.c →  Animated cursor trail
 *  gui_hr.c           →  Horizontal rule (---) rendering
 *  gui_wiki.c         →  [[Wiki link]] tagging & navigation
 *  gui_sync.c         →  Sync status UI (Dropbox, GitHub)
 *
 *  CSS / Assets  (already separated)
 *  ─────────────────────────────────────────────────────────
 *  src/ui/themes/base.css          →  Layout and widget styles
 *  src/ui/themes/theme-*.css       →  Per-theme CSS variables
 *  src/ui/themes/README.md         →  Theme authoring guide
 *  src/ui/qirtas_markdown.lang     →  GtkSourceView syntax definition
 *  src/ui/qirtas*.style-scheme.xml →  Editor colour schemes
 * ============================================================ */

/* AppGui struct, DB_PATH, QirtasSyncState: canonical defs in gui_internal.h
 * / gui_shared.h (included above). gui.c no longer defines its own. */

/* ====
 * GLOBALS
  ==== */

/* App language (0 = English, 1 = Arabic). Drives RTL paragraph direction
 * in gui_buffer.c; UI dropdown/restart-button not yet ported to redesign. */
int qirtas_app_language = 0;

/* Perf observability gate (QIRTAS_PERF=1). Read by QIRTAS_PERF_* macros in
 * gui_internal.h; set from env in run_gui(). */
int qirtas_perf_enabled = 0;

/* Icon style (0 = Classic, 1 = Modern); drives qirtas_icon() lookup. */
int qirtas_icon_style = 0;

/* Translation passthrough. Full EN/AR tr_table not yet ported to the redesign
 * (Language System = separate task); return English unchanged for now. */
const char *qirtas_tr(const char *en) { return en; }

AppGui    *global_gui         = NULL;
GtkWidget *global_window      = NULL;
GtkWidget *global_source_view = NULL;
GtkWidget *global_sync_label  = NULL;
GtkWidget *global_path_label  = NULL;
GtkWidget *global_time_label  = NULL;

static int seconds_elapsed = 0;
static void *global_add_popover_widgets = NULL;

extern void zig_on_gui_ready(void);
extern void zig_open_file(const char *filename);
extern void zig_open_vault(const char *dir_path);
extern void zig_search_workspace(const char *query);
extern const char *zig_get_search_snippet(const char *filepath);
extern int zig_get_search_rank(const char *filepath);
extern void zig_set_cursor_trail(int enabled);
extern int zig_get_cursor_trail(void);

/* ============
 * FORWARD DECLARATIONS
  ============ */

static void on_open_vault_clicked(GtkButton *btn, gpointer user_data);
static void on_vault_dialog_response(GObject *source_object, GAsyncResult *res, gpointer user_data);
static void on_trail_toggled(GtkCheckButton *chk, gpointer user_data);

void populate_explorer(AppGui *gui);
static void set_active_tab(AppGui *gui, GtkWidget *active_btn, const char *page);
void toggle_search(AppGui *gui);
void on_insert_text_before(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data);
void on_insert_text_after(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data);
void on_delete_range_after(GtkTextBuffer *buf, GtkTextIter *start, GtkTextIter *end, gpointer user_data);
gboolean on_editor_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
void on_editor_right_click(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data);
static gboolean editor_get_iter_at_widget_point(AppGui *gui, gdouble x, gdouble y, GtkTextIter *iter);
static void apply_regex_conceal(GtkTextBuffer *buf, const gchar *text, const gchar *pattern, gint cursor_char, gint delim_len, GtkTextTag *conceal_tag);
static void apply_regex_conceal_local(GtkTextBuffer *buf, const gchar *text, gint range_start_offset, const gchar *pattern, gint cursor_char, gint delim_len, GtkTextTag *conceal_tag);
void update_conceal_markdown(GtkTextBuffer *buf);
void update_conceal_markdown_all(GtkTextBuffer *buf);
void on_buffer_changed(GtkTextBuffer *buf, gpointer user_data);
void on_mark_set(GtkTextBuffer *buf, GtkTextIter *location, GtkTextMark *mark, gpointer user_data);
static void on_cursor_position_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
static void on_format_clicked(GtkButton *btn, gpointer user_data);
static void on_para_clicked(GtkButton *btn, gpointer user_data);
void apply_wiki_link_tags(GtkTextBuffer *buf);
void on_editor_left_click(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data);
void on_editor_motion(GtkEventControllerMotion *controller, gdouble x, gdouble y, gpointer user_data);
static gboolean on_editor_mouse_event(GtkEventControllerLegacy *controller, GdkEvent *event, gpointer user_data);
gboolean keycode_matches_latin_keyval(guint keycode, guint target_keyval);
void show_keybindings_window(AppGui *gui);
static void on_settings_btn_clicked(GtkButton *btn, gpointer user_data);
static gboolean on_settings_window_close_request(GtkWindow *window, gpointer user_data);
static void on_status_bar_pos_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);
static void on_sidebar_side_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);
static void on_export_pdf_clicked(GtkButton *btn, gpointer user_data);
void qirtas_export_to_pdf(AppGui *gui);
void update_editor_font(AppGui *gui);
void check_and_insert_hr(GtkTextBuffer *buf, AppGui *gui);
void parse_and_render_hrs(GtkTextBuffer *buf, AppGui *gui);
static char *replace_anchors_with_hrs(const char *src);
void apply_paragraph_alignment(GtkTextBuffer *buf, GtkJustification justification);
static void on_paste_plain_text_received(GObject *source_object, GAsyncResult *res, gpointer user_data);
static void on_save_as_dialog_response(GObject *source_object, GAsyncResult *res, gpointer user_data);
void trigger_save_as(AppGui *gui);
void move_current_line(GtkTextBuffer *buf, gboolean up);
static int get_line_height(GtkWidget *text_view);

/* Sync callbacks defined in gui_sync.c */
void load_sync_credentials(AppGui *gui);
void on_save_credentials_clicked(GtkButton *btn, gpointer user_data);
void on_sync_connect_clicked(GtkButton *btn, gpointer user_data);
void on_sync_submit_clicked(GtkButton *btn, gpointer user_data);
void on_sync_now_clicked(GtkButton *btn, gpointer user_data);
void on_dropbox_save_credentials_clicked(GtkButton *btn, gpointer user_data);
void on_dropbox_connect_clicked(GtkButton *btn, gpointer user_data);
void on_dropbox_submit_clicked(GtkButton *btn, gpointer user_data);
void on_dropbox_now_clicked(GtkButton *btn, gpointer user_data);
void on_github_save_credentials_clicked(GtkButton *btn, gpointer user_data);
void on_github_connect_clicked(GtkButton *btn, gpointer user_data);
void on_github_now_clicked(GtkButton *btn, gpointer user_data);
void on_local_sync_clicked(GtkButton *btn, gpointer user_data);

/* ScrollToCursorData + AppShortcut: canonical defs in gui_internal.h. */
gboolean idle_scroll_to_cursor(gpointer user_data);

static gboolean parse_shortcut_string(const char *str, guint *out_keyval, GdkModifierType *out_state);
void init_app_shortcuts(void);
gboolean match_app_shortcut(const char *action_id, guint keyval, guint keycode, GdkModifierType state);
static gboolean on_settings_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
static void on_edit_shortcut_clicked(GtkButton *btn, gpointer user_data);


/* FFI — Called from Zig */
char *gui_get_text(void);
void gui_free_text(char *text);
void gui_set_text(const char *text, int len);
void gui_set_title(const char *title);
void gui_set_sync_status(const char *status);
void gui_show_editor(void);
void gui_get_cursor_position(int *line, int *col);
void gui_set_cursor_position(int line, int col);
void gui_refresh_explorer(void);
typedef void (*GuiIdleCallback)(void *user_data);
void gui_run_on_main_thread(GuiIdleCallback callback, void *user_data);
void gui_update_sync_status(int connected, const char *status_text);
void gui_update_dropbox_status(int connected, const char *status_text);
void gui_update_github_status(int connected, const char *status_text);
void gui_update_local_sync_status(int connected, const char *status_text);

/* FFI — Implemented in Zig */
void zig_open_wiki_link(const char *note_name);
void zig_create_new_file(const char *filename);
void zig_on_shutdown(void);
void zig_force_save(void);
void zig_save_sync_credentials(const char *client_id, const char *client_secret);
void zig_sync_connect(void);
void zig_sync_submit_code(const char *code);
void zig_sync_now(void);
int  zig_sync_check_status(void);
void zig_sync_disconnect(void);

void zig_save_dropbox_credentials(const char *client_id, const char *client_secret);
void zig_dropbox_connect(void);
void zig_dropbox_submit_code(const char *code);
void zig_dropbox_now(void);
int  zig_dropbox_check_status(void);
void zig_dropbox_disconnect(void);

void zig_save_github_credentials(const char *token, const char *repo);
int  zig_get_github_credentials_decrypted(char *token_buf, int token_buf_max, char *repo_buf, int repo_buf_max);
int  zig_get_dropbox_credentials_decrypted(char *client_id_buf, int client_id_max, char *client_secret_buf, int client_secret_max);
void zig_github_now(void);
int  zig_github_check_status(void);
void zig_github_disconnect(void);
void zig_local_sync_now(void);

/* ============================================================
 * CSS — Deep Slate Dark Theme
 * NOTE: GTK4 CSS does NOT support:
 *   - !important
 *   - max-width / max-height
 *   - outline
 * ============================================================ */

/* ── Minimal structural fallback CSS ─────────────────────────────────────
 * Used ONLY when an external theme-*.css file cannot be read from disk.
 * The full visual theme is always loaded from src/ui/themes/theme-*.css.
 * This fallback keeps the app functional (readable, usable) but unstyled.
 * ──────────────────────────────────────────────────────────────────────── */
static const char *CSS_FALLBACK_MINIMAL =
    "* { font-family: 'JetBrains Mono', 'Fira Mono', 'DejaVu Sans Mono', monospace; }\n"
    ".sidebar   { min-width: 120px; padding: 16px 12px; }\n"
    ".workspace { padding: 0; }\n"
    "textview   { padding: 24px; }\n"
    ".search-bar-revealer { padding: 8px 16px; }\n"
    ".tree-container { background: transparent; }\n"
    ".tree-row   { padding: 4px 8px; border-radius: 6px; }\n"
    ".bottom-bar { padding: 4px 12px; }\n";

static char current_theme[32] = "dark";
static char custom_theme_path[1024] = "";
void on_theme_dropdown_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);
/* ghost shadow carets, push only on big jumps, decay fast.
 * GHOST_COUNT canonical in gui_internal.h. */
/* Minimum cursor movement in pixels to spawn a new ghost.
 * A typical character is ~8-10px wide. Threshold of 3 means
 * we capture most cursor movements, but they fade so fast that
 * slow typing remains unnoticeable. */
#define GHOST_MIN_DIST 3.0
/* Alpha decay per frame at ~60 fps.
 * 0.12 / frame → ghost lifetime = 0.25 / 0.12 ≈ 2 frames ≈ 33 ms.
 * Fast enough that you won't notice during slow typing.             */
#define GHOST_DECAY 0.12


/* ── Draw a single ghost caret (rounded-rect pill) at (gx, gy) with given alpha ── */
static void draw_ghost_caret(cairo_t *cr,
                             double gx, double gy,
                             double caret_w, double caret_h,
                             double r, double g_col, double b,
                             double alpha)
{
    if (alpha <= 0.01) return;

    /* Caret width: keep it narrow like a real I-beam (2–3 px) */
    double w      = caret_w < 2.5 ? 2.5 : caret_w;
    double h      = caret_h;
    double radius = w / 2.0;
    if (radius < 1.0) radius = 1.0;
    if (radius > h / 2.0) radius = h / 2.0;

    cairo_new_sub_path(cr);
    /* top-right arc */
    cairo_arc(cr, gx + w - radius, gy + radius, radius, -G_PI_2, 0);
    /* bottom-right arc */
    cairo_arc(cr, gx + w - radius, gy + h - radius, radius, 0, G_PI_2);
    /* bottom-left arc */
    cairo_arc(cr, gx + radius, gy + h - radius, radius, G_PI_2, G_PI);
    /* top-left arc */
    cairo_arc(cr, gx + radius, gy + radius, radius, G_PI, 3.0 * G_PI_2);
    cairo_close_path(cr);

    cairo_set_source_rgba(cr, r, g_col, b, alpha);
    cairo_fill(cr);
}


static char *resolve_resource_path(const char *rel_path) {
    static char abs_path[2048];
    char exe_path[1024] = {0};
    ssize_t exe_len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (exe_len > 0) {
        exe_path[exe_len] = '\0';
        char *last_slash = strrchr(exe_path, '/');
        if (last_slash) *last_slash = '\0'; /* e.g. /home/.../Qirtas/zig-out/bin */
        
        snprintf(abs_path, sizeof(abs_path), "%s/../../%s", exe_path, rel_path);
        
        if (access(abs_path, F_OK) != 0) {
            snprintf(abs_path, sizeof(abs_path), "%s/%s", exe_path, rel_path);
        }
        return abs_path;
    }
    strncpy(abs_path, rel_path, sizeof(abs_path) - 1);
    abs_path[sizeof(abs_path) - 1] = '\0';
    return abs_path;
}


static void on_custom_theme_dialog_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(dialog, res, &error);
    AppGui *gui = (AppGui *)user_data;
    GtkDropDown *dropdown = g_object_get_data(G_OBJECT(dialog), "theme-dropdown");
    
    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            strncpy(custom_theme_path, path, sizeof(custom_theme_path) - 1);
            custom_theme_path[sizeof(custom_theme_path) - 1] = '\0';
            apply_theme(gui, "custom");
            g_free(path);
        }
        g_object_unref(file);
    } else {
        if (dropdown) {
            int idx = 0;
            if (strcmp(current_theme, "sepia") == 0) idx = 1;
            else if (strcmp(current_theme, "midnight") == 0) idx = 2;
            else if (strcmp(current_theme, "things") == 0) idx = 3;
            else if (strcmp(current_theme, "typewriter-light") == 0) idx = 4;
            else if (strcmp(current_theme, "typewriter-dark") == 0) idx = 5;
            else if (strcmp(current_theme, "qirtas") == 0) idx = 6;
            else if (strcmp(current_theme, "custom") == 0) idx = 7;
            g_signal_handlers_block_by_func(dropdown, G_CALLBACK(on_theme_dropdown_changed), gui);
            gtk_drop_down_set_selected(dropdown, idx);
            g_signal_handlers_unblock_by_func(dropdown, G_CALLBACK(on_theme_dropdown_changed), gui);
        }
    }
}


/* ============================================================
 * TEXT DIRECTION (RTL/LTR)
 * ============================================================ */

static gboolean is_arabic_char(gunichar c) {
    return ((c >= 0x0600 && c <= 0x06FF) ||
            (c >= 0x0750 && c <= 0x077F) ||
            (c >= 0x08A0 && c <= 0x08FF) ||
            (c >= 0xFB50 && c <= 0xFDFF) ||
            (c >= 0xFE70 && c <= 0xFEFF));
}

static gboolean detect_rtl(const char *text) {
    if (!text) return FALSE;
    const char *p = text;
    while (*p) {
        gunichar c = g_utf8_get_char(p);
        
        // Skip whitespaces, digits, common punctuation, and markdown formatting characters
        if (g_unichar_isspace(c) ||
            g_unichar_isdigit(c) ||
            g_unichar_ispunct(c) ||
            c == '#' || c == '*' || c == '-' || c == '_' || c == '+' ||
            c == '>' || c == '`' || c == '[' || c == ']' || c == '(' ||
            c == ')' || c == '{' || c == '}' || c == '|' || c == '~') {
            p = g_utf8_next_char(p);
            continue;
        }
        
        // Check if the first strong character is Arabic
        if (is_arabic_char(c)) {
            return TRUE;
        }
        
        // If it starts with Latin/English characters, fall back to LTR
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            return FALSE;
        }
        
        p = g_utf8_next_char(p);
    }
    return FALSE;
}

static void update_paragraph_direction(GtkTextBuffer *buf, GtkTextIter *iter) {
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buf);
    GtkTextTag *rtl_tag = gtk_text_tag_table_lookup(table, "rtl-tag");
    if (!rtl_tag) {
        rtl_tag = gtk_text_buffer_create_tag(buf, "rtl-tag", "direction", GTK_TEXT_DIR_RTL, NULL);
    }
    GtkTextTag *ltr_tag = gtk_text_tag_table_lookup(table, "ltr-tag");
    if (!ltr_tag) {
        ltr_tag = gtk_text_buffer_create_tag(buf, "ltr-tag", "direction", GTK_TEXT_DIR_LTR, NULL);
    }

    gint line_num = gtk_text_iter_get_line(iter);
    GtkTextIter start;
    gtk_text_buffer_get_iter_at_line(buf, &start, line_num);
    GtkTextIter end = start;
    if (!gtk_text_iter_ends_line(&end)) {
        gtk_text_iter_forward_to_line_end(&end);
    }
    gchar *text = gtk_text_iter_get_text(&start, &end);
    if (text) {
        if (detect_rtl(text)) {
            gtk_text_buffer_remove_tag(buf, ltr_tag, &start, &end);
            gtk_text_buffer_apply_tag(buf, rtl_tag, &start, &end);
        } else {
            gtk_text_buffer_remove_tag(buf, rtl_tag, &start, &end);
            gtk_text_buffer_apply_tag(buf, ltr_tag, &start, &end);
        }
        g_free(text);
    }
}

void update_all_paragraphs_direction(GtkTextBuffer *buf) {
    GtkTextIter iter;
    gtk_text_buffer_get_start_iter(buf, &iter);
    while (TRUE) {
        update_paragraph_direction(buf, &iter);
        if (!gtk_text_iter_forward_line(&iter)) {
            break;
        }
    }
}

/* Per-edit direction update: only the touched lines. The full pass above is
 * O(document) and pulls a full document copy out of Zig just to sample for
 * RTL — running it per keystroke (even from idle) was the typing-lag bug. */
void update_paragraph_direction_lines(GtkTextBuffer *buf, gint first_line, gint last_line) {
    for (gint l = first_line; l <= last_line; l++) {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(buf, &it, l);
        update_paragraph_direction(buf, &it);
        if (l >= gtk_text_buffer_get_line_count(buf)) break;
    }
}

typedef struct {
    GtkTextBuffer *buf;
    GtkWidget     *popover;
    gint           saved_start; /* char offset of selection start when popover opened */
    gint           saved_end;   /* char offset of selection end   when popover opened */
} PopoverData;

/* Restore the saved selection from PopoverData and apply inline format wrapper */
static void apply_format_with_saved(GtkTextBuffer *buf, const char *prefix, const char *suffix,
                                    gint saved_start, gint saved_end) {
    gtk_text_buffer_begin_user_action(buf);

    gboolean has_selection = (saved_start != saved_end);

    if (!has_selection) {
        /* No selection — insert markers at saved cursor and place cursor between them */
        GtkTextIter cursor_iter;
        gtk_text_buffer_get_iter_at_offset(buf, &cursor_iter, saved_start);

        gint offset = gtk_text_iter_get_offset(&cursor_iter);
        gtk_text_buffer_insert(buf, &cursor_iter, prefix, -1);

        gtk_text_buffer_get_iter_at_offset(buf, &cursor_iter, offset + (gint)strlen(prefix));
        gtk_text_buffer_insert(buf, &cursor_iter, suffix, -1);

        GtkTextIter final_cursor;
        gtk_text_buffer_get_iter_at_offset(buf, &final_cursor, offset + (gint)strlen(prefix));
        gtk_text_buffer_select_range(buf, &final_cursor, &final_cursor);
    } else {
        /* Restore selection that was lost when popover opened */
        GtkTextIter start, end;
        gtk_text_buffer_get_iter_at_offset(buf, &start, saved_start);
        gtk_text_buffer_get_iter_at_offset(buf, &end,   saved_end);

        gchar *text     = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
        gchar *new_text = g_strconcat(prefix, text, suffix, NULL);

        gint start_offset = gtk_text_iter_get_offset(&start);
        gtk_text_buffer_delete(buf, &start, &end);

        GtkTextIter insert_iter;
        gtk_text_buffer_get_iter_at_offset(buf, &insert_iter, start_offset);
        gtk_text_buffer_insert(buf, &insert_iter, new_text, -1);

        GtkTextIter select_start, select_end;
        gtk_text_buffer_get_iter_at_offset(buf, &select_start, start_offset);
        gtk_text_buffer_get_iter_at_offset(buf, &select_end,   start_offset + (gint)strlen(new_text));
        gtk_text_buffer_select_range(buf, &select_start, &select_end);

        g_free(text);
        g_free(new_text);
    }
    gtk_text_buffer_end_user_action(buf);
}

/* Legacy wrapper used by keyboard shortcuts (Ctrl+B etc.) — uses live selection */

/* Core paragraph formatting body: strips existing markers, adds new prefix. */
static void apply_paragraph_format_core(GtkTextBuffer *buf, const char *prefix,
                                        gint start_line, gint end_line) {
    gtk_text_buffer_begin_user_action(buf);

    for (gint l = start_line; l <= end_line; l++) {
        GtkTextIter line_start, line_end;
        gtk_text_buffer_get_iter_at_line(buf, &line_start, l);

        line_end = line_start;
        gtk_text_iter_forward_to_line_end(&line_end);
        gchar *line_text = gtk_text_buffer_get_text(buf, &line_start, &line_end, FALSE);

        int ws = 0;
        while (line_text[ws] == ' ' || line_text[ws] == '\t') ws++;

        int strip_len = 0;
        char *content = line_text + ws;
        if (strncmp(content, "- [ ] ", 6) == 0 || strncmp(content, "- [x] ", 6) == 0 ||
            strncmp(content, "* [ ] ", 6) == 0 || strncmp(content, "* [x] ", 6) == 0) {
            strip_len = ws + 6;
        } else if (strncmp(content, "- ", 2) == 0 || strncmp(content, "* ", 2) == 0 ||
                   strncmp(content, "+ ", 2) == 0) {
            strip_len = ws + 2;
        } else if (content[0] == '#') {
            int h = 0;
            while (content[h] == '#') h++;
            if (content[h] == ' ') strip_len = ws + h + 1;
        } else {
            int num = 0;
            while (content[num] >= '0' && content[num] <= '9') num++;
            if (num > 0 && content[num] == '.' && content[num+1] == ' ')
                strip_len = ws + num + 2;
        }

        if (strip_len > 0) {
            GtkTextIter strip_end = line_start;
            gtk_text_iter_forward_chars(&strip_end, strip_len);
            gtk_text_buffer_delete(buf, &line_start, &strip_end);
            gtk_text_buffer_get_iter_at_line(buf, &line_start, l);
        }

        if (prefix && strlen(prefix) > 0) {
            char final_prefix[64];
            if (strcmp(prefix, "1. ") == 0)
                snprintf(final_prefix, sizeof(final_prefix), "%d. ", (l - start_line) + 1);
            else
                snprintf(final_prefix, sizeof(final_prefix), "%s", prefix);
            gtk_text_buffer_insert(buf, &line_start, final_prefix, -1);
        }

        g_free(line_text);
    }

    gtk_text_buffer_end_user_action(buf);
}

/* Entry point for popover buttons — uses saved selection offsets */
static void apply_paragraph_format_with_saved(GtkTextBuffer *buf, const char *prefix,
                                              gint saved_start, gint saved_end) {
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_offset(buf, &start, saved_start);
    gtk_text_buffer_get_iter_at_offset(buf, &end,   saved_end);
    gint start_line = gtk_text_iter_get_line(&start);
    gint end_line   = gtk_text_iter_get_line(&end);
    apply_paragraph_format_core(buf, prefix, start_line, end_line);

    gint total_lines = gtk_text_buffer_get_line_count(buf);
    if (end_line >= total_lines) {
        end_line = total_lines - 1;
    }
    if (end_line < 0) end_line = 0;

    GtkTextIter cursor_iter;
    gtk_text_buffer_get_iter_at_line(buf, &cursor_iter, end_line);
    gtk_text_iter_forward_to_line_end(&cursor_iter);
    gtk_text_buffer_select_range(buf, &cursor_iter, &cursor_iter);
}

/* Entry point for keyboard shortcuts — uses live selection */

typedef struct {
    GtkTextBuffer *buf;
    char          *prefix;
    char          *suffix;
    gint           saved_start;
    gint           saved_end;
    gboolean       is_paragraph;
} IdleFormatData;

static gboolean do_idle_format(gpointer user_data) {
    IdleFormatData *ifd = (IdleFormatData *)user_data;
    GtkTextBuffer *buf = ifd->buf;

    /* Block both signal handlers that queue scroll-to-cursor or conceal
     * updates while we mutate the buffer.  Without this, every individual
     * insert/delete inside the format loop fires mark-set → on_mark_set →
     * idle_scroll_to_cursor, queuing many scroll callbacks that fight each
     * other and produce a visible cursor jump. */
    if (global_gui) {
        g_signal_handlers_block_by_func(buf, on_mark_set,      global_gui);
        g_signal_handlers_block_by_func(buf, on_buffer_changed, global_gui);
    }

    if (ifd->is_paragraph) {
        apply_paragraph_format_with_saved(buf, ifd->prefix, ifd->saved_start, ifd->saved_end);
    } else {
        apply_format_with_saved(buf, ifd->prefix, ifd->suffix, ifd->saved_start, ifd->saved_end);
    }

    /* Unblock, then do a single conceal refresh so the decorators (bold
     * markers, highlights, etc.) render correctly around the new text. */
    if (global_gui) {
        g_signal_handlers_unblock_by_func(buf, on_mark_set,      global_gui);
        g_signal_handlers_unblock_by_func(buf, on_buffer_changed, global_gui);
    }

    /* One clean conceal pass now that all mutations are done. */
    update_conceal_markdown_all(buf);

    /* One well-timed scroll: queue an idle that runs AFTER GTK re-layouts
     * from the conceal-tag changes we just applied. */
    if (global_gui && global_gui->source_view &&
        gtk_widget_get_realized(global_gui->source_view)) {
        GtkTextIter insert_iter;
        gtk_text_buffer_get_iter_at_mark(buf, &insert_iter, gtk_text_buffer_get_insert(buf));

        ScrollToCursorData *d = g_new(ScrollToCursorData, 1);
        d->gui  = global_gui;
        d->offset = gtk_text_iter_get_offset(&insert_iter);
        g_idle_add(idle_scroll_to_cursor, d);
    }

    g_free(ifd->prefix);
    g_free(ifd->suffix);
    g_free(ifd);
    return G_SOURCE_REMOVE;
}

static void on_format_clicked(GtkButton *btn, gpointer user_data) {
    PopoverData *pd = (PopoverData *)user_data;
    const char *prefix = g_object_get_data(G_OBJECT(btn), "prefix");
    const char *suffix = g_object_get_data(G_OBJECT(btn), "suffix");
    
    gtk_popover_popdown(GTK_POPOVER(pd->popover));
    if (global_source_view) {
        gtk_widget_grab_focus(global_source_view);
    }
    
    IdleFormatData *ifd = g_new(IdleFormatData, 1);
    ifd->buf = pd->buf;
    ifd->prefix = prefix ? g_strdup(prefix) : NULL;
    ifd->suffix = suffix ? g_strdup(suffix) : NULL;
    ifd->saved_start = pd->saved_start;
    ifd->saved_end = pd->saved_end;
    ifd->is_paragraph = FALSE;
    
    g_idle_add(do_idle_format, ifd);
}

static void on_para_clicked(GtkButton *btn, gpointer user_data) {
    PopoverData *pd = (PopoverData *)user_data;
    const char *prefix = g_object_get_data(G_OBJECT(btn), "prefix");
    
    gtk_popover_popdown(GTK_POPOVER(pd->popover));
    if (global_source_view) {
        gtk_widget_grab_focus(global_source_view);
    }
    
    IdleFormatData *ifd = g_new(IdleFormatData, 1);
    ifd->buf = pd->buf;
    ifd->prefix = prefix ? g_strdup(prefix) : NULL;
    ifd->suffix = NULL;
    ifd->saved_start = pd->saved_start;
    ifd->saved_end = pd->saved_end;
    ifd->is_paragraph = TRUE;
    
    g_idle_add(do_idle_format, ifd);
}

/* ============================================================
 * UNIFIED ADD / OPEN POPOVER & SHUTDOWN CONTROLS
 * ============================================================ */

typedef struct {
    GtkWidget *popover;
    GtkWidget *box_actions;
    GtkWidget *box_input;
    GtkWidget *entry_name;
    AppGui *gui;
} AddPopoverWidgets;

static void on_open_dialog_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
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
    }
}

static void on_vault_dialog_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    AppGui *gui = (AppGui *)user_data;
    GError *error = NULL;
    GFile *folder = gtk_file_dialog_select_folder_finish(dialog, res, &error);
    if (folder) {
        char *path = g_file_get_path(folder);
        if (path) {
            zig_open_vault(path);
            if (gui->vault_path_lbl_val) {
                gtk_label_set_text(GTK_LABEL(gui->vault_path_lbl_val), path);
            }
            g_free(path);
        }
        g_object_unref(folder);
    }
}

static void on_open_vault_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppGui *gui = (AppGui *)user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Vault Directory");
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(gui->settings_window), NULL, on_vault_dialog_response, gui);
}

static void on_trail_toggled(GtkCheckButton *chk, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gboolean active = gtk_check_button_get_active(chk);
    gui->enable_cursor_trail = active;
    zig_set_cursor_trail(active ? 1 : 0);
    if (!active) {
        gui->trail_len = 0;
        if (gui->cursor_trail_area) {
            gtk_widget_queue_draw(gui->cursor_trail_area);
        }
    }
}

static void on_export_pdf_clicked(GtkButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    qirtas_export_to_pdf(gui);
}

static void on_open_existing_clicked(GtkButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Existing File");
    gtk_file_dialog_open(dialog, GTK_WINDOW(gui->window), NULL, on_open_dialog_response, gui);
    
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
}

static void on_create_submit(GtkEntry *entry, gpointer user_data) {
    AddPopoverWidgets *w = (AddPopoverWidgets *)user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (text && strlen(text) > 0) {
        zig_create_new_file(text);
    }
    gtk_popover_popdown(GTK_POPOVER(w->popover));
}

static void on_create_new_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AddPopoverWidgets *w = (AddPopoverWidgets *)user_data;
    gtk_widget_set_visible(w->box_actions, FALSE);
    gtk_widget_set_visible(w->box_input, TRUE);
    gtk_widget_grab_focus(w->entry_name);
}

static void on_popover_closed(GtkPopover *popover, gpointer user_data) {
    (void)popover;
    AddPopoverWidgets *w = (AddPopoverWidgets *)user_data;
    gtk_widget_set_visible(w->box_actions, TRUE);
    gtk_widget_set_visible(w->box_input, FALSE);
    gtk_editable_set_text(GTK_EDITABLE(w->entry_name), "");
}

static gboolean on_window_close_request(GtkWindow *window, gpointer user_data) {
    (void)window;
    zig_on_shutdown();
    return FALSE; // Allow window closure
}

static void on_popover_destroy(GtkWidget *widget, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (gui && gui->active_popover == widget) {
        gui->active_popover = NULL;
    }
}

static void on_editor_popover_closed(GtkPopover *popover, gpointer user_data) {
    (void)user_data;
    (void)popover;
    /* GTK4 popdown() already hides the popover; unparenting is handled by
     * the "destroy" signal connected to on_popover_destroy. Do nothing here. */
}


static gboolean editor_get_iter_at_widget_point(AppGui *gui, gdouble x, gdouble y, GtkTextIter *iter) {
    if (!gui || !gui->source_view || !iter) return FALSE;

    int bx, by;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(gui->source_view),
                                         GTK_TEXT_WINDOW_TEXT,
                                         (int)x, (int)y, &bx, &by);
    return gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(gui->source_view), iter, bx, by);
}


static gboolean on_editor_mouse_event(GtkEventControllerLegacy *controller,
                                      GdkEvent *event,
                                      gpointer user_data) {
    (void)controller;
    AppGui *gui = (AppGui *)user_data;
    if (!gui->source_view) return FALSE;
    
    GdkEventType type = gdk_event_get_event_type(event);
    
    // Track primary mouse button state separately from actual drag movement.
    if (type == GDK_BUTTON_PRESS) {
        guint button = gdk_button_event_get_button(event);
        if (button == GDK_BUTTON_PRIMARY) {
            double press_x = 0.0, press_y = 0.0;
            gui->primary_button_down = TRUE;
            gui->mouse_dragging = FALSE;
            if (gdk_event_get_position(event, &press_x, &press_y)) {
                gui->mouse_press_x = press_x;
                gui->mouse_press_y = press_y;
            }
        }
    } else if (type == GDK_BUTTON_RELEASE) {
        guint button = gdk_button_event_get_button(event);
        if (button == GDK_BUTTON_PRIMARY) {
            gui->primary_button_down = FALSE;
            gui->mouse_dragging = FALSE;
        }
    } else if (type == GDK_MOTION_NOTIFY) {
        GdkModifierType state = gdk_event_get_modifier_state(event);
        if ((state & GDK_BUTTON1_MASK) != 0 && gui->primary_button_down) {
            double motion_x = 0.0, motion_y = 0.0;
            if (gdk_event_get_position(event, &motion_x, &motion_y)) {
                double dx = motion_x - gui->mouse_press_x;
                double dy = motion_y - gui->mouse_press_y;
                gui->mouse_dragging = (dx * dx + dy * dy) >= 16.0;
            }
        } else {
            gui->primary_button_down = FALSE;
            gui->mouse_dragging = FALSE;
        }
    }

    if (type == GDK_MOTION_NOTIFY || type == GDK_BUTTON_PRESS) {
        double x = 0.0, y = 0.0;
        if (gdk_event_get_position(event, &x, &y)) {
            GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
            GtkTextIter end_iter;
            gtk_text_buffer_get_end_iter(buf, &end_iter);

            GdkRectangle end_rect;
            gtk_text_view_get_iter_location(GTK_TEXT_VIEW(gui->source_view), &end_iter, &end_rect);

            int win_x = 0, win_y = 0;
            gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(gui->source_view),
                                                  GTK_TEXT_WINDOW_WIDGET,
                                                  end_rect.x, end_rect.y,
                                                  &win_x, &win_y);

            /* Actual bottom of the last line of text in widget coordinates */
            double text_end_y = (double)(win_y + end_rect.height);

            if (y > text_end_y) {
                /* Pointer is in genuine empty space below all text.
                 * Place cursor/selection at the document end, clamp the
                 * viewport, and consume the event so GTK's autoscroll
                 * timer cannot feed this out-of-bounds position back.  */
                if (type == GDK_BUTTON_PRESS) {
                    guint button = gdk_button_event_get_button(event);
                    if (button != GDK_BUTTON_PRIMARY) {
                        return FALSE;
                    }
                    gtk_text_buffer_place_cursor(buf, &end_iter);
                } else {
                    if (!gui->mouse_dragging) {
                        return FALSE;
                    }
                    /* MOTION: extend selection to end while dragging */
                    GtkTextMark *sel_bound =
                        gtk_text_buffer_get_selection_bound(buf);
                    GtkTextIter bound;
                    gtk_text_buffer_get_iter_at_mark(buf, &bound, sel_bound);
                    gtk_text_buffer_select_range(buf, &end_iter, &bound);
                }

                if (gui->vadjustment) {
                    double ps  = gtk_adjustment_get_page_size(gui->vadjustment);
                    double ms  = end_rect.y + end_rect.height - ps;
                    if (ms < 0) ms = 0;
                    double cur = gtk_adjustment_get_value(gui->vadjustment);
                    if (cur > ms) {
                        gui->in_scroll_update = TRUE;
                        gtk_adjustment_set_value(gui->vadjustment, ms);
                        gui->in_scroll_update = FALSE;
                    }
                }

                return TRUE; /* consume — stop GTK autoscroll in empty zone */
            }
        }
    }
    return FALSE;
}

static void on_workspace_click(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data) {
    (void)gesture; (void)n_press; (void)x; (void)y;
    AppGui *gui = (AppGui *)user_data;
    if (gui) {
        if (gui->active_popover) {
            gtk_widget_unparent(gui->active_popover);
        }
        if (gui->sidebar && gtk_widget_get_visible(gui->sidebar)) {
            gtk_widget_set_visible(gui->sidebar, FALSE);
        }
    }
}

static void apply_regex_conceal(GtkTextBuffer *buf, const gchar *text, const gchar *pattern, gint cursor_char, gint delim_len, GtkTextTag *conceal_tag) {
    GError *error = NULL;
    static GRegex *regex_bold = NULL;
    static GRegex *regex_highlight = NULL;
    static GRegex *regex_italic = NULL;
    
    GRegex *regex = NULL;
    if (strcmp(pattern, "\\*\\*([^\\n]+?)\\*\\*") == 0) {
        if (!regex_bold) regex_bold = g_regex_new(pattern, G_REGEX_DEFAULT, 0, NULL);
        regex = regex_bold;
    } else if (strcmp(pattern, "==([^\\n]+?)==") == 0) {
        if (!regex_highlight) regex_highlight = g_regex_new(pattern, G_REGEX_DEFAULT, 0, NULL);
        regex = regex_highlight;
    } else if (strcmp(pattern, "(?<!\\*)\\*([^\\n\\*]+?)\\*(?!\\*)") == 0) {
        if (!regex_italic) regex_italic = g_regex_new(pattern, G_REGEX_DEFAULT, 0, NULL);
        regex = regex_italic;
    } else {
        regex = g_regex_new(pattern, G_REGEX_DEFAULT, 0, NULL);
    }
    
    if (!regex) return;

    GMatchInfo *match_info = NULL;
    gboolean has_match = g_regex_match(regex, text, 0, &match_info);
    while (has_match) {
        gint start_byte = 0;
        gint end_byte = 0;
        if (g_match_info_fetch_pos(match_info, 0, &start_byte, &end_byte)) {
            gint start_char = g_utf8_pointer_to_offset(text, text + start_byte);
            gint end_char = g_utf8_pointer_to_offset(text, text + end_byte);
            
            gboolean cursor_inside = (cursor_char >= start_char && cursor_char <= end_char);
            
            if (!cursor_inside) {
                GtkTextIter start_iter, end_iter;
                gtk_text_buffer_get_iter_at_offset(buf, &start_iter, start_char);
                gtk_text_buffer_get_iter_at_offset(buf, &end_iter, start_char + delim_len);
                gtk_text_buffer_apply_tag(buf, conceal_tag, &start_iter, &end_iter);
                
                gtk_text_buffer_get_iter_at_offset(buf, &start_iter, end_char - delim_len);
                gtk_text_buffer_get_iter_at_offset(buf, &end_iter, end_char);
                gtk_text_buffer_apply_tag(buf, conceal_tag, &start_iter, &end_iter);
            }
        }
        has_match = g_match_info_next(match_info, &error);
    }
    g_match_info_free(match_info);
    if (regex != regex_bold && regex != regex_highlight && regex != regex_italic) {
        g_regex_unref(regex);
    }
}

static void apply_regex_conceal_local(GtkTextBuffer *buf, const gchar *text, gint range_start_offset, const gchar *pattern, gint cursor_char, gint delim_len, GtkTextTag *conceal_tag) {
    GError *error = NULL;
    static GRegex *regex_bold = NULL;
    static GRegex *regex_highlight = NULL;
    static GRegex *regex_italic = NULL;
    
    GRegex *regex = NULL;
    if (strcmp(pattern, "\\*\\*([^\\n]+?)\\*\\*") == 0) {
        if (!regex_bold) regex_bold = g_regex_new(pattern, G_REGEX_DEFAULT, 0, NULL);
        regex = regex_bold;
    } else if (strcmp(pattern, "==([^\\n]+?)==") == 0) {
        if (!regex_highlight) regex_highlight = g_regex_new(pattern, G_REGEX_DEFAULT, 0, NULL);
        regex = regex_highlight;
    } else if (strcmp(pattern, "(?<!\\*)\\*([^\\n\\*]+?)\\*(?!\\*)") == 0) {
        if (!regex_italic) regex_italic = g_regex_new(pattern, G_REGEX_DEFAULT, 0, NULL);
        regex = regex_italic;
    } else {
        regex = g_regex_new(pattern, G_REGEX_DEFAULT, 0, NULL);
    }
    
    if (!regex) return;

    GMatchInfo *match_info = NULL;
    gboolean has_match = g_regex_match(regex, text, 0, &match_info);
    while (has_match) {
        gint start_byte = 0;
        gint end_byte = 0;
        if (g_match_info_fetch_pos(match_info, 0, &start_byte, &end_byte)) {
            gint start_char_local = g_utf8_pointer_to_offset(text, text + start_byte);
            gint end_char_local = g_utf8_pointer_to_offset(text, text + end_byte);
            
            gint start_char = range_start_offset + start_char_local;
            gint end_char = range_start_offset + end_char_local;
            
            gboolean cursor_inside = (cursor_char >= start_char && cursor_char <= end_char);
            
            if (!cursor_inside) {
                GtkTextIter start_iter, end_iter;
                gtk_text_buffer_get_iter_at_offset(buf, &start_iter, start_char);
                gtk_text_buffer_get_iter_at_offset(buf, &end_iter, start_char + delim_len);
                gtk_text_buffer_apply_tag(buf, conceal_tag, &start_iter, &end_iter);
                
                gtk_text_buffer_get_iter_at_offset(buf, &start_iter, end_char - delim_len);
                gtk_text_buffer_get_iter_at_offset(buf, &end_iter, end_char);
                gtk_text_buffer_apply_tag(buf, conceal_tag, &start_iter, &end_iter);
            }
        }
        has_match = g_match_info_next(match_info, &error);
    }
    g_match_info_free(match_info);
    if (regex != regex_bold && regex != regex_highlight && regex != regex_italic) {
        g_regex_unref(regex);
    }
}


static char *replace_anchors_with_hrs(const char *src) {
    if (!src) return NULL;

    GString *str = g_string_new("");
    const char *p = src;
    while (*p) {
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBF && (unsigned char)p[2] == 0xBC) {
            g_string_append(str, "---");
            p += 3;
        } else {
            g_string_append_c(str, *p);
            p++;
        }
    }
    return g_string_free(str, FALSE);
}


/* Idle callback: fires after GTK has re-laid-out the text view following
 * conceal-tag changes, so get_iter_location() returns the correct rect. */


static void on_cursor_position_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    (void)object;
    (void)pspec;
    (void)user_data;
    /* Redundant: on_mark_set already updates formatting layout on cursor moves. */
}


static void on_paste_plain_text_received(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GdkClipboard *clipboard = GDK_CLIPBOARD(source_object);
    GError *error = NULL;
    char *text = gdk_clipboard_read_text_finish(clipboard, res, &error);
    if (error) {
        g_warning("Failed to read text from clipboard: %s", error->message);
        g_clear_error(&error);
        return;
    }
    if (text) {
        AppGui *gui = (AppGui *)user_data;
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
        GtkTextIter start, end;
        gtk_text_buffer_begin_user_action(buf);
        if (gtk_text_buffer_get_selection_bounds(buf, &start, &end)) {
            gtk_text_buffer_delete(buf, &start, &end);
        }
        gtk_text_buffer_insert_at_cursor(buf, text, -1);
        gtk_text_buffer_end_user_action(buf);
        g_free(text);
    }
}

static void on_save_as_dialog_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
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
                    zig_open_file(path);
                } else {
                    gui_set_sync_status("Save As Failed");
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


static AppShortcut app_shortcuts[] = {
    { "bold", "Bold text", "<Control>b", 0, 0 },
    { "italic", "Italic text", "<Control>i", 0, 0 },
    { "underline", "Underline text", "<Control>u", 0, 0 },
    { "strikethrough", "Strikethrough text", "<Control><Shift>x", 0, 0 },
    { "left_align", "Left align text", "<Control><Shift>l", 0, 0 },
    { "center_align", "Center align text", "<Control><Shift>e", 0, 0 },
    { "right_align", "Right align text", "<Control><Shift>r", 0, 0 },
    { "justify", "Justify text", "<Control><Shift>j", 0, 0 },
    { "clear_format", "Clear formatting", "<Control>backslash", 0, 0 },
    { "zoom_in", "Zoom In", "<Control>equal", 0, 0 },
    { "zoom_out", "Zoom Out", "<Control>minus", 0, 0 },
    { "reset_zoom", "Reset Zoom", "<Control>0", 0, 0 },
    { "fullscreen", "Fullscreen / Focus Mode", "F11", 0, 0 },
    { "copy", "Copy selected text", "<Control>c", 0, 0 },
    { "cut", "Cut selected text", "<Control>x", 0, 0 },
    { "paste", "Paste text", "<Control>v", 0, 0 },
    { "paste_plain", "Paste plain text", "<Control><Shift>v", 0, 0 },
    { "undo", "Undo last action", "<Control>z", 0, 0 },
    { "redo", "Redo last action", "<Control>y", 0, 0 },
    { "select_all", "Select all text", "<Control>a", 0, 0 },
    { "duplicate_line", "Duplicate current line", "<Control>d", 0, 0 },
    { "move_line_up", "Move line up", "<Alt>Up", 0, 0 },
    { "move_line_down", "Move line down", "<Alt>Down", 0, 0 },
    { "toggle_comment", "Toggle comment", "<Control>slash", 0, 0 },
    { "delete_prev_word", "Delete previous word", "<Control>BackSpace", 0, 0 },
    { "delete_next_word", "Delete next word", "<Control>Delete", 0, 0 },
    { "delete_line", "Delete current line", "<Control><Shift>k", 0, 0 },
    { "move_word_left", "Move word left", "<Control>Left", 0, 0 },
    { "move_word_right", "Move word right", "<Control>Right", 0, 0 },
    { "move_line_start", "Move to start of line", "Home", 0, 0 },
    { "move_line_end", "Move to end of line", "End", 0, 0 },
    { "move_doc_start", "Move to start of document", "<Control>Home", 0, 0 },
    { "move_doc_end", "Move to end of document", "<Control>End", 0, 0 },
    { "new_file", "Create new file", "<Control>n", 0, 0 },
    { "open_file", "Open existing file", "<Control>o", 0, 0 },
    { "save_file", "Save file", "<Control>s", 0, 0 },
    { "save_file_as", "Save file as...", "<Control><Shift>s", 0, 0 },
    { "export_pdf", "Print / Export PDF", "<Control>p", 0, 0 },
    { "toggle_search", "Toggle search bar", "<Control>f", 0, 0 },
    { "replace_text", "Replace text", "<Control>h", 0, 0 },
    { "close_tab", "Close file / tab", "<Control>w", 0, 0 },
    { "open_settings", "Open settings", "<Control>comma", 0, 0 },
    { "shortcuts_ref", "Shortcuts reference", "<Control>question", 0, 0 },
    { "toggle_sidebar", "Toggle Sidebar", "<Control><Shift>backslash", 0, 0 },
    { "inline_code", "Inline code", "<Control>k", 0, 0 },
    { "highlight", "Highlight", "<Control><Shift>h", 0, 0 },
    { "blockquote", "Blockquote", "<Control>q", 0, 0 },
    { "math", "Math", "<Control>m", 0, 0 },
    { "ordered_list", "Ordered list", "<Control><Shift>o", 0, 0 },
    { "task_list", "Task list", "<Control><Shift>t", 0, 0 }
};
#define NUM_APP_SHORTCUTS (sizeof(app_shortcuts)/sizeof(app_shortcuts[0]))

static int shortcut_listening_index = -1;
static GtkWidget *shortcut_value_labels[NUM_APP_SHORTCUTS] = { NULL };
static GtkWidget *shortcut_edit_buttons[NUM_APP_SHORTCUTS] = { NULL };

static gboolean parse_shortcut_string(const char *str, guint *out_keyval, GdkModifierType *out_state) {
    if (!str || strlen(str) == 0) return FALSE;

    GdkModifierType state = 0;
    const char *p = str;

    while (*p == '<') {
        const char *end = strchr(p, '>');
        if (!end) break;
        
        size_t len = end - p - 1;
        if (g_ascii_strncasecmp(p + 1, "Control", len) == 0) {
            state |= GDK_CONTROL_MASK;
        } else if (g_ascii_strncasecmp(p + 1, "Shift", len) == 0) {
            state |= GDK_SHIFT_MASK;
        } else if (g_ascii_strncasecmp(p + 1, "Alt", len) == 0) {
            state |= GDK_ALT_MASK;
        } else if (g_ascii_strncasecmp(p + 1, "Meta", len) == 0 || g_ascii_strncasecmp(p + 1, "Super", len) == 0) {
            state |= GDK_SUPER_MASK;
        }
        p = end + 1;
    }

    guint keyval = gdk_keyval_from_name(p);
    if (keyval == GDK_KEY_VoidSymbol) {
        if (strcmp(p, "+") == 0) keyval = GDK_KEY_plus;
        else if (strcmp(p, "=") == 0) keyval = GDK_KEY_equal;
        else if (strcmp(p, "-") == 0) keyval = GDK_KEY_minus;
        else if (strcmp(p, "\\") == 0) keyval = GDK_KEY_backslash;
        else return FALSE;
    }

    *out_keyval = keyval;
    *out_state = state;
    return TRUE;
}


static gboolean is_modifier_key(guint keyval) {
    return (keyval == GDK_KEY_Shift_L || keyval == GDK_KEY_Shift_R ||
            keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R ||
            keyval == GDK_KEY_Alt_L || keyval == GDK_KEY_Alt_R ||
            keyval == GDK_KEY_Meta_L || keyval == GDK_KEY_Meta_R ||
            keyval == GDK_KEY_Super_L || keyval == GDK_KEY_Super_R ||
            keyval == GDK_KEY_Hyper_L || keyval == GDK_KEY_Hyper_R ||
            keyval == GDK_KEY_Caps_Lock || keyval == GDK_KEY_Num_Lock ||
            keyval == GDK_KEY_Scroll_Lock);
}

static gchar* get_pretty_shortcut_string(const char *shortcut_str) {
    GString *pretty = g_string_new("");
    const char *p = shortcut_str;
    while (*p) {
        if (strncmp(p, "<Control>", 9) == 0) {
            g_string_append(pretty, "Ctrl + ");
            p += 9;
        } else if (strncmp(p, "<Shift>", 7) == 0) {
            g_string_append(pretty, "Shift + ");
            p += 7;
        } else if (strncmp(p, "<Alt>", 5) == 0) {
            g_string_append(pretty, "Alt + ");
            p += 5;
        } else if (strncmp(p, "<Super>", 7) == 0) {
            g_string_append(pretty, "Super + ");
            p += 7;
        } else {
            if (strlen(p) == 1 && *p >= 'a' && *p <= 'z') {
                g_string_append_c(pretty, *p - 32);
            } else {
                g_string_append(pretty, p);
            }
            break;
        }
    }
    return g_string_free(pretty, FALSE);
}

static gboolean on_settings_key_pressed(GtkEventControllerKey *ctrl,
                                        guint keyval, guint keycode,
                                        GdkModifierType state, gpointer user_data) {
    (void)ctrl;
    (void)keycode;
    (void)user_data;

    if (shortcut_listening_index < 0) return FALSE;

    if (is_modifier_key(keyval)) {
        return TRUE;
    }

    int idx = shortcut_listening_index;
    shortcut_listening_index = -1;

    GString *gstr = g_string_new("");
    if (state & GDK_CONTROL_MASK) g_string_append(gstr, "<Control>");
    if (state & GDK_SHIFT_MASK)   g_string_append(gstr, "<Shift>");
    if (state & GDK_ALT_MASK)     g_string_append(gstr, "<Alt>");
    if (state & GDK_SUPER_MASK)   g_string_append(gstr, "<Super>");

    const char *key_name = gdk_keyval_name(keyval);
    if (!key_name) {
        key_name = "void";
    }
    g_string_append(gstr, key_name);

    strncpy(app_shortcuts[idx].shortcut_str, gstr->str, sizeof(app_shortcuts[idx].shortcut_str) - 1);
    app_shortcuts[idx].shortcut_str[sizeof(app_shortcuts[idx].shortcut_str) - 1] = '\0';
    app_shortcuts[idx].keyval = keyval;
    app_shortcuts[idx].state = state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_ALT_MASK | GDK_SUPER_MASK);

    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) == SQLITE_OK) {
        const char *insert_sql = "INSERT OR REPLACE INTO keyboard_shortcuts (action_name, shortcut_val) VALUES (?, ?);";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, app_shortcuts[idx].action_id, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, app_shortcuts[idx].shortcut_str, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }

    if (shortcut_value_labels[idx]) {
        gchar *pretty_str = get_pretty_shortcut_string(app_shortcuts[idx].shortcut_str);
        gtk_label_set_text(GTK_LABEL(shortcut_value_labels[idx]), pretty_str);
        g_free(pretty_str);
    }

    if (shortcut_edit_buttons[idx]) {
        gtk_button_set_label(GTK_BUTTON(shortcut_edit_buttons[idx]), "Edit");
    }

    g_string_free(gstr, TRUE);

    return TRUE;
}

static void on_edit_shortcut_clicked(GtkButton *btn, gpointer user_data) {
    int idx = GPOINTER_TO_INT(user_data);

    if (shortcut_listening_index >= 0 && shortcut_listening_index != idx) {
        int old_idx = shortcut_listening_index;
        if (shortcut_edit_buttons[old_idx]) {
            gtk_button_set_label(GTK_BUTTON(shortcut_edit_buttons[old_idx]), "Edit");
        }
    }

    shortcut_listening_index = idx;
    gtk_button_set_label(btn, "Press keys...");
}

static void on_kb_window_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    (void)user_data;
    if (shortcut_listening_index >= 0) {
        shortcut_listening_index = -1;
    }
    for (size_t i = 0; i < NUM_APP_SHORTCUTS; i++) {
        shortcut_value_labels[i] = NULL;
        shortcut_edit_buttons[i] = NULL;
    }
}


/* ============================================================
 * HELPERS
 * ============================================================ */

static void set_active_tab(AppGui *gui, GtkWidget *active_btn, const char *page) {
    if (gui->btn_editor) gtk_widget_remove_css_class(gui->btn_editor, "active");
    if (gui->btn_files)  gtk_widget_remove_css_class(gui->btn_files,  "active");
    if (gui->btn_search) gtk_widget_remove_css_class(gui->btn_search, "active");
    if (active_btn) gtk_widget_add_css_class(active_btn, "active");
    if (page) {
        gtk_stack_set_visible_child_name(GTK_STACK(gui->stack), page);
        gboolean is_editor = (strcmp(page, "editor") == 0);
        if (gui->status_pill) gtk_widget_set_visible(gui->status_pill, is_editor);
        if (gui->btn_search) gtk_widget_set_visible(gui->btn_search, is_editor);
        if (gui->path_label) gtk_widget_set_visible(gui->path_label, is_editor);
    }
}

/* ============================================================
 * SESSION TIMER
 * ============================================================ */

static gboolean update_timer(gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    seconds_elapsed++;
    char buf[32];
    snprintf(buf, sizeof(buf), "%ds ~", seconds_elapsed);
    if (global_time_label)
        gtk_label_set_text(GTK_LABEL(global_time_label), buf);
    if (gui->stats_time_val) {
        char full[64];
        snprintf(full, sizeof(full), "%d seconds", seconds_elapsed);
        gtk_label_set_text(GTK_LABEL(gui->stats_time_val), full);
    }
    return TRUE;
}

/* ============================================================
 * FOCUS MODE
 * ============================================================ */

static void on_logo_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppGui *gui = (AppGui *)user_data;
    gboolean visible = gtk_widget_get_visible(gui->sidebar);
    gtk_widget_set_visible(gui->sidebar, !visible);
}

/* ============================================================
 * SEARCH
 * ============================================================ */

static void update_search_match_count(AppGui *gui) {
    if (!gui->search_ctx) return;
    gint count = gtk_source_search_context_get_occurrences_count(gui->search_ctx);
    char buf[48];
    if (count == -1 || count == 0)
        snprintf(buf, sizeof(buf), "no matches");
    else
        snprintf(buf, sizeof(buf), "%d match%s", count, count == 1 ? "" : "es");
    gtk_label_set_text(GTK_LABEL(gui->search_match_label), buf);
}

static void do_search_forward(AppGui *gui) {
    if (!gui->search_ctx) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
    GtkTextIter start, end, ms, me;
    gtk_text_buffer_get_selection_bounds(buf, &start, &end);
    gtk_text_iter_forward_char(&end);
    gboolean wrapped;
    if (gtk_source_search_context_forward(gui->search_ctx, &end, &ms, &me, &wrapped)) {
        gtk_text_buffer_select_range(buf, &ms, &me);
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(gui->source_view),
                                     gtk_text_buffer_get_insert(buf),
                                     0.15, FALSE, 0.0, 0.5);
    }
    update_search_match_count(gui);
}

static void do_search_backward(AppGui *gui) {
    if (!gui->search_ctx) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
    GtkTextIter start, end, ms, me;
    gtk_text_buffer_get_selection_bounds(buf, &start, &end);
    gboolean wrapped;
    if (gtk_source_search_context_backward(gui->search_ctx, &start, &ms, &me, &wrapped)) {
        gtk_text_buffer_select_range(buf, &ms, &me);
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(gui->source_view),
                                     gtk_text_buffer_get_insert(buf),
                                     0.15, FALSE, 0.0, 0.5);
    }
    update_search_match_count(gui);
}

static char *create_arabic_search_regex(const char *input) {
    if (!input || strlen(input) == 0) return g_strdup("");

    GString *pattern = g_string_new("");
    const char *p = input;

    while (*p != '\0') {
        gunichar c = g_utf8_get_char(p);
        const char *next_p = g_utf8_next_char(p);

        // 1. Skip input diacritics so they are ignored/stripped from user query
        if ((c >= 0x064B && c <= 0x065F) || c == 0x0670) {
            p = next_p;
            continue;
        }

        // 2. Check for regex special characters and escape them
        if (strchr(".*+?^${}()|[]\\", (char)c) != NULL) {
            g_string_append_printf(pattern, "\\%c", (char)c);
        }
        // 3. Arabic letter variations
        else if (c == 0x0627 || c == 0x0623 || c == 0x0625 || c == 0x0622 || c == 0x0671) {
            // Alef group: [اأإآٱ]
            g_string_append(pattern, "[اأإآٱ]");
        }
        else if (c == 0x0629 || c == 0x0647) {
            // Teh Marbuta / Heh: [ةه]
            g_string_append(pattern, "[ةه]");
        }
        else if (c == 0x064A || c == 0x0649) {
            // Yeh / Alef Maksura: [يى]
            g_string_append(pattern, "[يى]");
        }
        else {
            // Standard character: append its UTF-8 representation
            char utf8_buf[6] = {0};
            int len = g_unichar_to_utf8(c, utf8_buf);
            g_string_append_len(pattern, utf8_buf, len);
        }

        // 4. Append optional Arabic diacritics matcher after this character
        // Arabic block is 0x0600 to 0x06FF.
        if (c >= 0x0600 && c <= 0x06FF) {
            g_string_append(pattern, "[\\x{064B}-\\x{065F}\\x{0670}]*");
        }

        p = next_p;
    }

    char *result = pattern->str;
    g_string_free(pattern, FALSE);
    return result;
}


/* ============================================================
 * NAV / SETTINGS CALLBACKS
 * ============================================================ */

static void on_editor_clicked(GtkButton *btn, gpointer user_data) {
    set_active_tab((AppGui *)user_data, GTK_WIDGET(btn), "editor");
}

static void on_files_clicked(GtkButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    set_active_tab(gui, GTK_WIDGET(btn), "files");
    populate_explorer(gui);
}

/* stats click handler removed */

static void on_wrap_toggled(GtkCheckButton *btn, gpointer user_data) {
    GtkTextView *view = GTK_TEXT_VIEW(user_data);

    /* Virtual scrolling uses fixed logical line heights, so soft wrap must stay off. */
    g_signal_handlers_block_by_func(btn, G_CALLBACK(on_wrap_toggled), user_data);
    gtk_check_button_set_active(btn, FALSE);
    g_signal_handlers_unblock_by_func(btn, G_CALLBACK(on_wrap_toggled), user_data);
    gtk_text_view_set_wrap_mode(view, GTK_WRAP_NONE);
}

static char current_en_font[64] = "JetBrains Mono";
static char current_ar_font[64] = "Amiri";
static double current_font_size = 16.0;


/* ============================================================
 * FILE EXPLORER — Tree helpers (Obsidian-style)
 * ============================================================ */

static void format_size(off_t bytes, char *out, size_t n) {
    if (bytes < 1024)
        snprintf(out, n, "%ld B", (long)bytes);
    else if (bytes < 1024 * 1024)
        snprintf(out, n, "%.1f KB", (double)bytes / 1024.0);
    else
        snprintf(out, n, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
}

static void format_mtime(time_t mtime, char *out, size_t n) {
    double diff = difftime(time(NULL), mtime);
    if      (diff < 60)        snprintf(out, n, "Just now");
    else if (diff < 3600)      snprintf(out, n, "%dm ago",  (int)(diff / 60));
    else if (diff < 86400)     snprintf(out, n, "%dh ago",  (int)(diff / 3600));
    else if (diff < 86400 * 2) snprintf(out, n, "Yesterday");
    else if (diff < 86400 * 7) {
        struct tm *lt = localtime(&mtime);
        char day[16]; strftime(day, sizeof(day), "%A", lt);
        snprintf(out, n, "%s", day);
    } else {
        struct tm *lt = localtime(&mtime);
        strftime(out, n, "%b %d", lt);
    }
}

/* Global tracker for the currently active tree row button */
static GtkWidget *g_active_tree_row = NULL;

/* Data passed to toggle callback for directory rows */
typedef struct {
    GtkWidget *children_box; /* The collapsible children container */
    GtkWidget *arrow_label;  /* "▶" / "▼" label */
    gboolean   expanded;
} TreeDirData;

static void on_tree_dir_toggle(GtkButton *btn, gpointer user_data) {
    (void)btn;
    TreeDirData *d = (TreeDirData *)user_data;
    d->expanded = !d->expanded;
    gtk_widget_set_visible(d->children_box, d->expanded);
    gtk_label_set_text(GTK_LABEL(d->arrow_label), d->expanded ? "▼" : "▶");
}

static void on_tree_file_clicked(GtkButton *btn, gpointer user_data) {
    /* Clear previous active */
    if (g_active_tree_row && GTK_IS_WIDGET(g_active_tree_row))
        gtk_widget_remove_css_class(g_active_tree_row, "active");
    g_active_tree_row = GTK_WIDGET(btn);
    gtk_widget_add_css_class(GTK_WIDGET(btn), "active");
    zig_open_file((const char *)user_data);
}

/* Forward declaration */
static void tree_build_dir(GtkWidget *parent_box, const char *dir_path, int depth);

/*
 * Build a single file row widget.
 * full_path  – absolute or relative path to the file
 * name       – display name (basename)
 */
static GtkWidget *tree_build_file_row(const char *full_path, const char *name) {
    gboolean is_md = g_str_has_suffix(name, ".md") || g_str_has_suffix(name, ".txt");

    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "tree-row");
    gtk_widget_set_hexpand(btn, TRUE);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_halign(hbox, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(hbox, TRUE);

    /* File icon */
    const char *icon_name = is_md ? "text-x-generic-symbolic" : "text-x-generic-symbolic";
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_widget_add_css_class(icon, is_md ? "tree-icon-md" : "tree-icon");
    gtk_box_append(GTK_BOX(hbox), icon);

    /* Name label */
    GtkWidget *lbl = gtk_label_new(name);
    gtk_widget_add_css_class(lbl, "tree-row-label");
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(hbox), lbl);

    gtk_button_set_child(GTK_BUTTON(btn), hbox);

    /* Store full path on button and connect signal */
    char *path_copy = g_strdup(full_path);
    g_signal_connect_data(btn, "clicked",
                          G_CALLBACK(on_tree_file_clicked),
                          path_copy, (GClosureNotify)g_free, 0);

    /* Store filepath for search */
    g_object_set_data_full(G_OBJECT(btn), "tree_filepath", g_strdup(full_path), g_free);

    return btn;
}

/*
 * Build a directory row with a collapsible children box.
 * Returns the outer wrapper box (row + children).
 */
static GtkWidget *tree_build_dir_row(const char *dir_path, const char *name) {
    GtkWidget *wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Header button row */
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "tree-row");
    gtk_widget_add_css_class(btn, "tree-row-dir");
    gtk_widget_set_hexpand(btn, TRUE);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_hexpand(hbox, TRUE);

    /* Arrow label */
    GtkWidget *arrow = gtk_label_new("▶");
    gtk_widget_add_css_class(arrow, "tree-arrow");
    gtk_box_append(GTK_BOX(hbox), arrow);

    /* Folder icon */
    GtkWidget *icon = gtk_image_new_from_icon_name("folder-symbolic");
    gtk_widget_add_css_class(icon, "tree-icon-folder");
    gtk_box_append(GTK_BOX(hbox), icon);

    /* Name label */
    GtkWidget *lbl = gtk_label_new(name);
    gtk_widget_add_css_class(lbl, "tree-row-label");
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(hbox), lbl);

    gtk_button_set_child(GTK_BUTTON(btn), hbox);
    gtk_box_append(GTK_BOX(wrapper), btn);

    /* Children container (hidden by default) */
    GtkWidget *children_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(children_box, "tree-children");
    gtk_widget_set_visible(children_box, FALSE);
    gtk_box_append(GTK_BOX(wrapper), children_box);

    /* Recursively populate children */
    tree_build_dir(children_box, dir_path, 0);

    /* Toggle data */
    TreeDirData *data = g_new0(TreeDirData, 1);
    data->children_box = children_box;
    data->arrow_label  = arrow;
    data->expanded     = FALSE;

    g_signal_connect_data(btn, "clicked",
                          G_CALLBACK(on_tree_dir_toggle),
                          data, (GClosureNotify)g_free, 0);

    return wrapper;
}

/*
 * String comparison function for qsort — directories first, then alphabetical.
 */
typedef struct { char name[NAME_MAX+1]; gboolean is_dir; } DirEntry;

static int dir_entry_cmp(const void *a, const void *b) {
    const DirEntry *ea = (const DirEntry *)a;
    const DirEntry *eb = (const DirEntry *)b;
    if (ea->is_dir != eb->is_dir)
        return ea->is_dir ? -1 : 1; /* dirs first */
    return strcasecmp(ea->name, eb->name);
}

/*
 * Recursively fill parent_box with tree rows for all entries in dir_path.
 * depth is unused currently but kept for future indent logic.
 */
static void tree_build_dir(GtkWidget *parent_box, const char *dir_path, int depth) {
    (void)depth;

    GError *err = NULL;
    GDir *dir = g_dir_open(dir_path, 0, &err);
    if (!dir) {
        if (err) g_error_free(err);
        return;
    }

    /* Collect entries */
    DirEntry entries[4096];
    int count = 0;
    const char *nm;
    while ((nm = g_dir_read_name(dir)) != NULL && count < 4095) {
        if (nm[0] == '.') continue; /* skip hidden */
        strncpy(entries[count].name, nm, NAME_MAX);
        entries[count].name[NAME_MAX] = '\0';

        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir_path, nm);
        entries[count].is_dir = g_file_test(full, G_FILE_TEST_IS_DIR);
        count++;
    }
    g_dir_close(dir);

    qsort(entries, count, sizeof(DirEntry), dir_entry_cmp);

    for (int i = 0; i < count; i++) {
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir_path, entries[i].name);

        GtkWidget *row_widget;
        if (entries[i].is_dir) {
            row_widget = tree_build_dir_row(full, entries[i].name);
        } else {
            /* Only show relevant text files */
            const char *n = entries[i].name;
            gboolean show = g_str_has_suffix(n, ".md")  ||
                            g_str_has_suffix(n, ".txt") ||
                            g_str_has_suffix(n, ".zig") ||
                            g_str_has_suffix(n, ".c")   ||
                            g_str_has_suffix(n, ".h")   ||
                            g_str_has_suffix(n, ".zon");
            if (!show) continue;
            row_widget = tree_build_file_row(full, entries[i].name);
        }
        gtk_box_append(GTK_BOX(parent_box), row_widget);
    }
}

/* ============================================================
 * populate_explorer  — replaces the old flat-list version
 * ============================================================ */

static GtkWidget *g_tree_container = NULL; /* the root GtkBox inside the scroll */


/* ============================================================
 * Search helpers — simplified for tree (just re-populate on clear,
 * filter visible rows by name on search)
 * ============================================================ */

static guint explorer_search_timeout_id = 0;

/* Recursively walk the tree GtkBox and show/hide file rows matching query */
static int tree_filter_walk(GtkWidget *box, const char *query, int *match_count) {
    GtkWidget *child = gtk_widget_get_first_child(box);
    int visible_in_this_box = 0;
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        if (GTK_IS_BUTTON(child)) {
            /* File row */
            const char *fp = g_object_get_data(G_OBJECT(child), "tree_filepath");
            if (fp) {
                const char *basename = g_path_get_basename(fp); /* leaks, but tiny */
                gboolean match = (query == NULL || strlen(query) == 0 ||
                                  (strcasestr(basename, query) != NULL));
                gtk_widget_set_visible(child, match);
                if (match) { visible_in_this_box++; if (match_count) (*match_count)++; }
            }
        } else if (GTK_IS_BOX(child)) {
            /* Could be a dir wrapper or children_box */
            /* Check if it has the tree-children class → recurse into it */
            if (gtk_widget_has_css_class(child, "tree-children")) {
                int sub = tree_filter_walk(child, query, match_count);
                /* Make children_box visible if it has matches and we are searching */
                if (query && strlen(query) > 0) {
                    gtk_widget_set_visible(child, sub > 0);
                    if (sub > 0) visible_in_this_box++;
                } else {
                    /* Reset to collapsed */
                    gtk_widget_set_visible(child, FALSE);
                }
            } else {
                /* Dir wrapper box (contains button + children_box) */
                int sub = tree_filter_walk(child, query, match_count);
                gtk_widget_set_visible(child, (query == NULL || strlen(query) == 0) || sub > 0);
                if (sub > 0) visible_in_this_box++;
            }
        }
        child = next;
    }
    return visible_in_this_box;
}

static gboolean do_debounced_explorer_search(gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    explorer_search_timeout_id = 0;

    if (!gui || !gui->explorer_listbox) return G_SOURCE_REMOVE;

    const char *query = gtk_editable_get_text(GTK_EDITABLE(gui->exp_search_entry));
    int match_count = 0;
    tree_filter_walk(gui->explorer_listbox, query, &match_count);

    if (gui->exp_count_label) {
        char badge[64];
        if (query && strlen(query) > 0)
            snprintf(badge, sizeof(badge), "Found %d matches", match_count);
        else {
            /* Count top-level items */
            int top = 0;
            GtkWidget *w = gtk_widget_get_first_child(gui->explorer_listbox);
            while (w) { top++; w = gtk_widget_get_next_sibling(w); }
            snprintf(badge, sizeof(badge), "%d items", top);
        }
        gtk_label_set_text(GTK_LABEL(gui->exp_count_label), badge);
    }

    return G_SOURCE_REMOVE;
}


/* Stub sort func — not used with tree box but kept to avoid linker issues */

/* ============================================================
 * BUILD UI
 * ============================================================ */

static gboolean on_window_key_pressed(GtkEventControllerKey *ctrl,
                                      guint keyval, guint keycode,
                                      GdkModifierType state, gpointer user_data) {
    (void)ctrl;
    AppGui *gui = (AppGui *)user_data;
    gboolean ctrl_held = (state & GDK_CONTROL_MASK) != 0;
    gboolean shift_held = (state & GDK_SHIFT_MASK) != 0;

    /* Toggle search bar */
    if (match_app_shortcut("toggle_search", keyval, keycode, state) ||
        match_app_shortcut("replace_text", keyval, keycode, state)) {
        toggle_search(gui);
        return TRUE;
    }

    if (keyval == GDK_KEY_Escape && gui->search_visible) {
        toggle_search(gui);
        return TRUE;
    }

    /* Keybindings reference window */
    if (match_app_shortcut("shortcuts_ref", keyval, keycode, state)) {
        show_keybindings_window(gui);
        return TRUE;
    }

    /* Close file / tab / Quit application */
    if (match_app_shortcut("close_tab", keyval, keycode, state) ||
        (ctrl_held && (keyval == GDK_KEY_q || keyval == GDK_KEY_Q || keycode_matches_latin_keyval(keycode, GDK_KEY_q))) ||
        (keyval == GDK_KEY_F4 && ctrl_held)) {
        g_application_quit(g_application_get_default());
        return TRUE;
    }

    /* Open settings */
    if (match_app_shortcut("open_settings", keyval, keycode, state)) {
        on_settings_btn_clicked(NULL, gui);
        return TRUE;
    }

    /* Toggle Sidebar */
    if (match_app_shortcut("toggle_sidebar", keyval, keycode, state)) {
        on_logo_clicked(NULL, gui);
        return TRUE;
    }

    /* Create New File */
    if (match_app_shortcut("new_file", keyval, keycode, state)) {
        if (global_add_popover_widgets) {
            AddPopoverWidgets *w = (AddPopoverWidgets *)global_add_popover_widgets;
            gtk_widget_set_visible(gui->sidebar, TRUE);
            gtk_popover_popup(GTK_POPOVER(w->popover));
            on_create_new_clicked(NULL, w);
        }
        return TRUE;
    }

    /* Open Existing File */
    if (match_app_shortcut("open_file", keyval, keycode, state)) {
        GtkFileDialog *dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, "Open Existing File");
        gtk_file_dialog_open(dialog, GTK_WINDOW(gui->window), NULL, on_open_dialog_response, gui);
        return TRUE;
    }

    /* Force Save */
    if (match_app_shortcut("save_file", keyval, keycode, state)) {
        zig_force_save();
        return TRUE;
    }

    /* Save As */
    if (match_app_shortcut("save_file_as", keyval, keycode, state)) {
        trigger_save_as(gui);
        return TRUE;
    }

    /* Print / Export PDF */
    if (match_app_shortcut("export_pdf", keyval, keycode, state)) {
        qirtas_export_to_pdf(gui);
        return TRUE;
    }

    /* Fullscreen / Focus Mode */
    if (match_app_shortcut("fullscreen", keyval, keycode, state)) {
        toggle_fullscreen(gui);
        return TRUE;
    }

    /* Zoom In */
    if (match_app_shortcut("zoom_in", keyval, keycode, state)) {
        current_font_size += 1.0;
        update_editor_font(gui);
        return TRUE;
    }

    /* Zoom Out */
    if (match_app_shortcut("zoom_out", keyval, keycode, state)) {
        if (current_font_size > 6.0) {
            current_font_size -= 1.0;
            update_editor_font(gui);
        }
        return TRUE;
    }

    /* Reset Zoom */
    if (match_app_shortcut("reset_zoom", keyval, keycode, state)) {
        current_font_size = 16.0;
        update_editor_font(gui);
        return TRUE;
    }

    return FALSE;
}


static void on_settings_btn_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppGui *gui = (AppGui *)user_data;
    if (gui->settings_window) {
        gtk_window_present(GTK_WINDOW(gui->settings_window));
    }
}

static gboolean on_settings_window_close_request(GtkWindow *window, gpointer user_data) {
    (void)user_data;
    if (shortcut_listening_index >= 0) {
        int old_idx = shortcut_listening_index;
        shortcut_listening_index = -1;
        if (shortcut_edit_buttons[old_idx]) {
            gtk_button_set_label(GTK_BUTTON(shortcut_edit_buttons[old_idx]), "Edit");
        }
    }
    gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    return TRUE;
}


static void on_status_bar_pos_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    AppGui *gui = (AppGui *)user_data;
    GtkDropDown *dropdown = GTK_DROP_DOWN(gobject);
    guint selected = gtk_drop_down_get_selected(dropdown);

    if (gui->main_vertical_box && gui->bottom_bar_widget && gui->sidebar_editor_box) {
        if (selected == 1) { // Top
            gtk_box_reorder_child_after(GTK_BOX(gui->main_vertical_box), gui->bottom_bar_widget, NULL);
        } else { // Bottom
            gtk_box_reorder_child_after(GTK_BOX(gui->main_vertical_box), gui->sidebar_editor_box, NULL);
        }
    }
}

static void on_sidebar_side_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    AppGui *gui = (AppGui *)user_data;
    GtkDropDown *dropdown = GTK_DROP_DOWN(gobject);
    guint selected = gtk_drop_down_get_selected(dropdown);

    if (gui->sidebar_editor_box && gui->sidebar && gui->stack) {
        g_object_ref(gui->sidebar);
        g_object_ref(gui->stack);

        gtk_paned_set_start_child(GTK_PANED(gui->sidebar_editor_box), NULL);
        gtk_paned_set_end_child(GTK_PANED(gui->sidebar_editor_box), NULL);

        if (selected == 0) { // Left
            gtk_paned_set_start_child(GTK_PANED(gui->sidebar_editor_box), gui->sidebar);
            gtk_paned_set_resize_start_child(GTK_PANED(gui->sidebar_editor_box), FALSE);
            gtk_paned_set_shrink_start_child(GTK_PANED(gui->sidebar_editor_box), FALSE);

            gtk_paned_set_end_child(GTK_PANED(gui->sidebar_editor_box), gui->stack);
            gtk_paned_set_resize_end_child(GTK_PANED(gui->sidebar_editor_box), TRUE);
            gtk_paned_set_shrink_end_child(GTK_PANED(gui->sidebar_editor_box), FALSE);

            gtk_paned_set_position(GTK_PANED(gui->sidebar_editor_box), 220);
        } else { // Right
            gtk_paned_set_start_child(GTK_PANED(gui->sidebar_editor_box), gui->stack);
            gtk_paned_set_resize_start_child(GTK_PANED(gui->sidebar_editor_box), TRUE);
            gtk_paned_set_shrink_start_child(GTK_PANED(gui->sidebar_editor_box), FALSE);

            gtk_paned_set_end_child(GTK_PANED(gui->sidebar_editor_box), gui->sidebar);
            gtk_paned_set_resize_end_child(GTK_PANED(gui->sidebar_editor_box), FALSE);
            gtk_paned_set_shrink_end_child(GTK_PANED(gui->sidebar_editor_box), FALSE);

            int width = gtk_widget_get_width(gui->sidebar_editor_box);
            if (width > 220) {
                gtk_paned_set_position(GTK_PANED(gui->sidebar_editor_box), width - 220);
            }
        }

        g_object_unref(gui->sidebar);
        g_object_unref(gui->stack);
    }
}


static int get_line_height(GtkWidget *text_view) {
    PangoLayout *layout = gtk_widget_create_pango_layout(text_view, "A");
    PangoRectangle rect;
    pango_layout_get_pixel_extents(layout, NULL, &rect);
    int pango_height = rect.height;
    g_object_unref(layout);
    
    int above = gtk_text_view_get_pixels_above_lines(GTK_TEXT_VIEW(text_view));
    int below = gtk_text_view_get_pixels_below_lines(GTK_TEXT_VIEW(text_view));
    return pango_height + above + below;
}

/* Recompute cached line height after a font/theme change. Called from
 * apply_theme so dependent layout (read-mode column, spacers) stays accurate. */
void gui_remeasure_line_height(void) {
    if (!global_gui || !global_gui->source_view) return;
    global_gui->line_height = get_line_height(global_gui->source_view);
}

static void on_scroll_changed(GtkAdjustment *adj, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (gui->in_scroll_update) return;

    /* Full-buffer model: the whole document is in the buffer and GtkTextView
     * scrolls it natively. No active-page swap on scroll. Only guard the top
     * boundary against negative values; GTK clamps the bottom itself. */
    double value = gtk_adjustment_get_value(adj);
    if (value < 0.0) {
        gui->in_scroll_update = TRUE;
        gtk_adjustment_set_value(adj, 0.0);
        gui->in_scroll_update = FALSE;
    }
}



/* Extra paper-card geometry macros + focus/dividers FFI (redesign shell). */
#define QIRTAS_DESK_GAP_DEFAULT 32
#define QIRTAS_DESK_GAP_MIN 8
#define QIRTAS_DESK_GAP_MAX 360
#define QIRTAS_RESIZE_HOTZONE 6
void zig_set_layout_dividers(int);
int zig_get_layout_dividers(void);
void zig_set_bottom_margin(int);
int zig_get_bottom_margin(void);
void zig_set_focus_mode(int);
int zig_get_focus_mode(void);
void zig_set_editor_border(int);

/* ===========================================================================
 * Paper-card layout subsystem (ported from the redesign): floating text
 * column width, focus mode, read mode. All widget-presence-guarded — safe to
 * link/run before activate() builds the paper card (editor_card may be NULL).
 * =========================================================================== */

#define QIRTAS_CARD_CHROME       160
#define QIRTAS_TEXT_COLUMN_MIN   420
#define QIRTAS_TEXT_COLUMN_MAX   840
/* Read mode caps the column to a comfortable reading measure regardless of
 * card width — wider margins, shorter lines. */
#define QIRTAS_READ_MODE_MAX_WIDTH 760

/* Last observed paper width signature; -1 forces paper_column_tick to recompute. */
static int s_last_paper_width = -1;

static void reorder_main_layout(AppGui *gui) {
    if (!gui || !gui->main_vertical_box || !gui->bottom_bar_widget || !gui->sidebar_editor_box) return;

    gboolean status_bar_is_top = FALSE; // Default to Bottom
    if (!gui->enable_focus_mode && gui->sb_pos_dropdown) {
        guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(gui->sb_pos_dropdown));
        if (selected == 1) status_bar_is_top = TRUE;
    }

    if (gui->tab_strip)
        gtk_box_reorder_child_after(GTK_BOX(gui->main_vertical_box), gui->tab_strip, NULL);

    GtkWidget *anchor = gui->tab_strip;

    if (status_bar_is_top) {
        gtk_box_reorder_child_after(GTK_BOX(gui->main_vertical_box), gui->bottom_bar_widget, anchor);
        gtk_box_reorder_child_after(GTK_BOX(gui->main_vertical_box), gui->sidebar_editor_box, gui->bottom_bar_widget);
    } else {
        gtk_box_reorder_child_after(GTK_BOX(gui->main_vertical_box), gui->sidebar_editor_box, anchor);
        gtk_box_reorder_child_after(GTK_BOX(gui->main_vertical_box), gui->bottom_bar_widget, gui->sidebar_editor_box);
    }
}

static void apply_editor_border(AppGui *gui) {
    GtkWidget *card = (gui && gui->editor_card) ? gui->editor_card : (gui ? gui->scrolled : NULL);
    if (!card) return;

    gtk_widget_set_hexpand(card, TRUE);
    gtk_widget_set_halign(card, GTK_ALIGN_FILL);
    gtk_widget_set_size_request(card, -1, -1);

    if (gui->enable_focus_mode) {
        gtk_widget_remove_css_class(card, "focus-mode");
        gtk_widget_set_margin_start(card, gui->desk_gap);
        gtk_widget_set_margin_end(card, gui->desk_gap);
        gtk_widget_set_margin_top(card, 28);
        gtk_widget_set_margin_bottom(card, 24);
        return;
    }

    if (gui->enable_editor_border) {
        gtk_widget_remove_css_class(card, "focus-mode");
        int top = gui->compact_mode ? 10 : 28;
        int bot = gui->compact_mode ?  8 : 24;
        gtk_widget_set_margin_start(card, gui->desk_gap);
        gtk_widget_set_margin_end(card, gui->desk_gap);
        gtk_widget_set_margin_top(card, top);
        gtk_widget_set_margin_bottom(card, bot);
    } else {
        gtk_widget_add_css_class(card, "focus-mode");
        gtk_widget_set_margin_start(card, 0);
        gtk_widget_set_margin_end(card, 0);
        gtk_widget_set_margin_top(card, 0);
        gtk_widget_set_margin_bottom(card, 0);
    }
}

static gboolean paper_column_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
    (void)clock;
    AppGui *gui = data;
    if (!gui || !gui->source_view) return G_SOURCE_CONTINUE;
    int width = gtk_widget_get_width(widget);
    if (width <= 1) return G_SOURCE_CONTINUE;

    GtkSourceView *sv = GTK_SOURCE_VIEW(gui->source_view);
    gboolean ln = gui->show_line_numbers;
    GtkWidget *gutter = GTK_WIDGET(gtk_source_view_get_gutter(sv, GTK_TEXT_WINDOW_LEFT));
    int gw = (ln && gutter) ? gtk_widget_get_width(gutter) : 0;

    int sig = width ^ (ln ? 0x40000000 : 0) ^ (gw << 8);
    if (sig == s_last_paper_width) return G_SOURCE_CONTINUE;
    s_last_paper_width = sig;

    int text_w = width - QIRTAS_CARD_CHROME;
    if (text_w < QIRTAS_TEXT_COLUMN_MIN) text_w = QIRTAS_TEXT_COLUMN_MIN;
    if (!gui->text_width_full_page && text_w > QIRTAS_TEXT_COLUMN_MAX)
        text_w = QIRTAS_TEXT_COLUMN_MAX;
    if (gui->read_mode && text_w > QIRTAS_READ_MODE_MAX_WIDTH)
        text_w = QIRTAS_READ_MODE_MAX_WIDTH;
    gui->text_column_width = text_w;

    int margin = (width - text_w) / 2;
    if (margin < 8) margin = 8;

    if (ln) {
        const int GAP = 8;
        int gutter_shift = margin - gw - GAP;
        if (gutter_shift < 0) gutter_shift = 0;
        if (gutter) gtk_widget_set_margin_start(gutter, gutter_shift);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(gui->source_view), GAP);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(gui->source_view), margin);
    } else {
        if (gutter) gtk_widget_set_margin_start(gutter, 0);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(gui->source_view), margin);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(gui->source_view), margin);
    }
    return G_SOURCE_CONTINUE;
}

typedef struct {
    AppGui *gui;
    GtkTextMark *mark;
    guint generation;
} ReadModeScrollData;

static gboolean restore_read_mode_scroll_cb(gpointer user_data) {
    ReadModeScrollData *d = user_data;
    if (d->generation == d->gui->buffer_generation && d->gui->source_view) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->gui->source_view));
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(d->gui->source_view), d->mark, 0.0, TRUE, 0.0, 0.0);
        gtk_text_buffer_delete_mark(buf, d->mark);
    }
    g_free(d);
    return G_SOURCE_REMOVE;
}

void apply_focus_mode(AppGui *gui) {
    if (!gui || !gui->scrolled || !gui->sidebar || !gui->main_vertical_box ||
        !gui->bottom_bar_widget || !gui->sidebar_editor_box) return;

    if (gui->enable_focus_mode) {
        gtk_widget_set_visible(gui->sidebar, FALSE);
        reorder_main_layout(gui);
        apply_editor_border(gui);
        s_last_paper_width = -1;
        if (gui->editor_header) gtk_widget_set_visible(gui->editor_header, FALSE);
        if (gui->sb_pos_dropdown) gtk_widget_set_sensitive(gui->sb_pos_dropdown, FALSE);
        if (gui->sb_side_dropdown) gtk_widget_set_sensitive(gui->sb_side_dropdown, FALSE);
        if (gui->divider_chk) gtk_widget_set_sensitive(gui->divider_chk, FALSE);
        if (gui->btn_sidebar_toggle) gtk_widget_set_sensitive(gui->btn_sidebar_toggle, TRUE);
    } else {
        gtk_widget_set_visible(gui->sidebar, TRUE);
        reorder_main_layout(gui);
        if (gui->editor_header) gtk_widget_set_visible(gui->editor_header, TRUE);
        if (gui->editor_thread) gtk_widget_set_visible(gui->editor_thread, TRUE);
        apply_editor_border(gui);
        if (gui->sb_pos_dropdown) gtk_widget_set_sensitive(gui->sb_pos_dropdown, TRUE);
        if (gui->sb_side_dropdown) gtk_widget_set_sensitive(gui->sb_side_dropdown, TRUE);
        if (gui->divider_chk) gtk_widget_set_sensitive(gui->divider_chk, TRUE);
        if (gui->btn_sidebar_toggle) gtk_widget_set_sensitive(gui->btn_sidebar_toggle, TRUE);
    }

    if (gui->focus_chk) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(gui->focus_chk), gui->enable_focus_mode);
    }
}

void toggle_read_mode(AppGui *gui) {
    if (!gui || !gui->source_view) return;

    GtkTextView *tv = GTK_TEXT_VIEW(gui->source_view);
    GtkTextBuffer *buf = gtk_text_view_get_buffer(tv);

    GdkRectangle visible;
    gtk_text_view_get_visible_rect(tv, &visible);
    GtkTextIter top_iter;
    gtk_text_view_get_iter_at_location(tv, &top_iter, visible.x, visible.y);
    GtkTextMark *scroll_anchor = gtk_text_buffer_create_mark(buf, NULL, &top_iter, TRUE);

    gui->read_mode = !gui->read_mode;

    gtk_text_view_set_editable(tv, !gui->read_mode);
    gtk_text_view_set_cursor_visible(tv, !gui->read_mode);

    if (gui->editor_card) {
        if (gui->read_mode) gtk_widget_add_css_class(gui->editor_card, "read-mode");
        else gtk_widget_remove_css_class(gui->editor_card, "read-mode");
    }
    if (gui->btn_read_mode) {
        if (gui->read_mode) gtk_widget_add_css_class(gui->btn_read_mode, "active");
        else gtk_widget_remove_css_class(gui->btn_read_mode, "active");
    }

    s_last_paper_width = -1;
    if (gui->editor_card) paper_column_tick(gui->editor_card, NULL, gui);
    update_conceal_markdown_all_sync(buf);

    ReadModeScrollData *d = g_new(ReadModeScrollData, 1);
    d->gui = gui;
    d->mark = scroll_anchor;
    d->generation = gui->buffer_generation;
    g_idle_add_full(G_PRIORITY_LOW, restore_read_mode_scroll_cb, d, NULL);
}


/* ===== Redesign UI shell (activate + handlers), ported from gui_conflict.c ===== */
typedef struct { const char *key; const char *classic; const char *modern; } IconPair;
/* forward decls (shell region) */
static void on_status_menu_shortcuts(GtkButton *btn, gpointer user_data);
static void on_status_menu_settings(GtkButton *btn, gpointer user_data);
static void popdown_ancestor_popover(GtkWidget *w);
static void on_status_menu_quit(GtkButton *btn, gpointer user_data);
static void on_status_bar_open_file_clicked(GtkButton *btn, gpointer user_data);
static void on_status_bar_new_file_clicked(GtkButton *btn, gpointer user_data);
static void on_status_menu_find_replace(GtkButton *btn, gpointer user_data);
static void on_status_bar_save_file_clicked(GtkButton *btn, gpointer user_data);
static void on_status_menu_fullscreen(GtkButton *btn, gpointer user_data);
static void on_restart_clicked(GtkButton *btn, gpointer user_data);
static void on_icon_style_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);
static void on_language_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);
static void on_status_menu_copy_file(GtkButton *btn, gpointer user_data);
static void on_status_bar_export_pdf_clicked(GtkButton *btn, gpointer user_data);
static void apply_compact_mode(AppGui *gui);
static GtkWidget *status_menu_item(const char *icon, const char *label, const char *hint, GCallback cb, gpointer user_data);
static void on_status_menu_save_as(GtkButton *btn, gpointer user_data);
static void on_read_mode_toggle_clicked(GtkButton *btn, gpointer user_data);
static void on_editor_border_toggled(GtkCheckButton *chk, gpointer user_data);
static void on_trail_color_custom_toggled(GtkCheckButton *chk, gpointer user_data);
static gboolean paper_column_timeout_wrapper(gpointer data);
static void on_outline_close_clicked(GtkButton *btn, gpointer user_data);
static void on_trail_color_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
static void on_compact_mode_toggled(GtkCheckButton *btn, gpointer user_data);
static void on_highlight_line_toggled(GtkCheckButton *btn, gpointer user_data);
static void on_line_numbers_toggled(GtkCheckButton *btn, gpointer user_data);
static void on_restore_session_toggled(GtkCheckButton *btn, gpointer user_data);
static void on_text_width_mode_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);
static void on_focus_mode_toggled(GtkCheckButton *chk, gpointer user_data);
static void on_pointer_color_custom_toggled(GtkCheckButton *chk, gpointer user_data);
static void on_pointer_color_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
static void apply_editor_prefs(AppGui *gui);
const char *qirtas_icon(const char *key);
static void apply_layout_dividers(AppGui *gui);
static int paper_edge_margin(AppGui *gui, GtkWidget *overlay);
static void on_column_resize_motion(GtkEventControllerMotion *ctrl, gdouble x, gdouble y, gpointer user_data);
static void on_column_resize_begin(GtkGestureDrag *gesture, gdouble x, gdouble y, gpointer user_data);
static void on_column_resize_update(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y, gpointer user_data);
static void on_column_resize_end(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y, gpointer user_data);
static void on_header_outline_clicked(GtkButton *btn, gpointer user_data);
static GtkWidget *editor_header_icon_btn(const char *icon_key, const char *tip, GCallback cb, gpointer data);
static void activate(GtkApplication *app, gpointer user_data);
int run_gui(int argc, char **argv);
void gui_index_all_files(void);
void gui_index_file(const char *filename);
void gui_remove_file_from_index(const char *filename);
char *gui_get_text(void);
void gui_free_text(char *text);
void gui_set_text(const char *text, int len);
void gui_get_cursor_position(int *line, int *col);
void gui_set_cursor_position(int line, int col);
int gui_get_absolute_cursor_line(void);
void gui_trigger_autosave(void);
static void refresh_explorer_idle_cb(void *user_data);
void gui_refresh_explorer(void);
void gui_set_title(const char *title);
void gui_set_sync_status(const char *status);
void gui_set_sync_state(QirtasSyncState state);
static gboolean sync_status_is_busy(const char *status_text);
static gboolean sync_status_is_success(const char *status_text);
static gboolean sync_status_is_error(const char *status_text);
static void set_sync_status_label(GtkWidget *label_widget, const char *status_text);
static void update_sync_status_callback(void *user_data);
void gui_update_sync_status(int connected, const char *status_text);
static void update_dropbox_status_callback(void *user_data);
void gui_update_dropbox_status(int connected, const char *status_text);
static void update_github_status_callback(void *user_data);
void gui_update_github_status(int connected, const char *status_text);
static void update_local_sync_status_callback(void *user_data);
void gui_update_local_sync_status(int connected, const char *status_text);
void gui_show_editor(void);
static gboolean idle_wrapper(gpointer d);
void gui_run_on_main_thread(GuiIdleCallback callback, void *user_data);

/* auto-pulled deps round 2 */
static void on_status_menu_shortcuts(GtkButton *btn, gpointer user_data) {
    popdown_ancestor_popover(GTK_WIDGET(btn));
    show_keybindings_window((AppGui *)user_data);
}

static void on_status_menu_settings(GtkButton *btn, gpointer user_data) {
    popdown_ancestor_popover(GTK_WIDGET(btn));
    on_settings_btn_clicked(NULL, (AppGui *)user_data);
}

static void popdown_ancestor_popover(GtkWidget *w) {
    GtkWidget *pop = gtk_widget_get_ancestor(w, GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
}

static void on_status_menu_quit(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    popdown_ancestor_popover(GTK_WIDGET(btn));
    g_application_quit(g_application_get_default());
}


/* auto-pulled deps round 1 */
static void on_status_bar_open_file_clicked(GtkButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, qirtas_tr("Open Existing File"));
    gtk_file_dialog_open(dialog, GTK_WINDOW(gui->window), NULL, on_open_dialog_response, gui);
}

static void on_status_bar_new_file_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));

    extern void zig_open_file(const char *filename);
    zig_open_file("Untitled");
}

static void on_status_menu_find_replace(GtkButton *btn, gpointer user_data) {
    popdown_ancestor_popover(GTK_WIDGET(btn));
    AppGui *gui = (AppGui *)user_data;
    if (!gui->search_visible) toggle_search(gui);
}

static void on_status_bar_save_file_clicked(GtkButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    gui_manual_save(gui);
}

static void on_status_menu_fullscreen(GtkButton *btn, gpointer user_data) {
    popdown_ancestor_popover(GTK_WIDGET(btn));
    toggle_fullscreen((AppGui *)user_data);
}

static void on_restart_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    char exe[1024] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        gchar *argv[] = { exe, NULL };
        g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL);
    }
    g_application_quit(g_application_get_default());
}

static void on_icon_style_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    AppGui *gui = (AppGui *)user_data;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(gobject));
    qirtas_icon_style = (sel == 1) ? 1 : 0;
    qirtas_pref_set_int("icon_style", qirtas_icon_style);

    /* Live-swap the always-visible icons; popover menus pick the new
     * set up on next launch. */
    if (gui) {
        if (gui->btn_search)
            gtk_button_set_child(GTK_BUTTON(gui->btn_search),
                gtk_image_new_from_icon_name(qirtas_icon("search")));
        if (gui->btn_sidebar_toggle)
            gtk_button_set_child(GTK_BUTTON(gui->btn_sidebar_toggle),
                gtk_image_new_from_icon_name(qirtas_icon("sidebar")));
        if (gui->btn_status_actions)
            gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(gui->btn_status_actions),
                                          qirtas_icon("menu"));
        populate_explorer(gui); /* folder + file icons in the tree */
    }
}

static void on_language_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)pspec; (void)user_data;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(gobject));
    qirtas_app_language = (sel == 1) ? 1 : 0;
    qirtas_pref_set_int("app_language", qirtas_app_language);
    /* Mirror the layout live; labels switch on next launch. */
    gtk_widget_set_default_direction(qirtas_app_language == 1
                                     ? GTK_TEXT_DIR_RTL : GTK_TEXT_DIR_LTR);
    if (global_gui && global_gui->bottom_bar_widget)
        gtk_widget_set_direction(global_gui->bottom_bar_widget, GTK_TEXT_DIR_LTR);
}

static void on_status_menu_copy_file(GtkButton *btn, gpointer user_data) {
    popdown_ancestor_popover(GTK_WIDGET(btn));
    AppGui *gui = (AppGui *)user_data;
    if (!gui || gui->active_tab_index == -1 || gui->active_tab_index >= gui->num_tabs) return;
    const char *path = gui->open_tabs[gui->active_tab_index];
    if (!path || strcmp(path, "Untitled") == 0) return;
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) return;

    /* Put the file itself on the clipboard (text/uri-list) so pasting in
     * a file manager copies the .md file. */
    GFile *file = g_file_new_for_path(path);
    GdkFileList *flist = gdk_file_list_new_from_array(&file, 1);
    GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
    gdk_clipboard_set(cb, GDK_TYPE_FILE_LIST, flist);
    g_boxed_free(GDK_TYPE_FILE_LIST, flist);
    g_object_unref(file);
}

static void on_status_bar_export_pdf_clicked(GtkButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));

    qirtas_export_to_pdf(gui);
}

static void apply_compact_mode(AppGui *gui) {
    if (!gui || !gui->window) return;
    if (gui->compact_mode)
        gtk_widget_add_css_class(gui->window, "compact-ui");
    else
        gtk_widget_remove_css_class(gui->window, "compact-ui");
    apply_editor_border(gui);
}

static GtkWidget *status_menu_item(const char *icon, const char *label,
                                   const char *hint,
                                   GCallback cb, gpointer user_data) {
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "pop-btn");
    gtk_widget_add_css_class(btn, "menu-item-btn");
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *img = gtk_image_new_from_icon_name(icon);
    GtkWidget *lbl = gtk_label_new(label);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_box_append(GTK_BOX(hbox), img);
    gtk_box_append(GTK_BOX(hbox), lbl);
    if (hint) {
        GtkWidget *hint_lbl = gtk_label_new(hint);
        gtk_widget_add_css_class(hint_lbl, "menu-item-hint");
        gtk_widget_set_halign(hint_lbl, GTK_ALIGN_END);
        gtk_box_append(GTK_BOX(hbox), hint_lbl);
    }
    gtk_button_set_child(GTK_BUTTON(btn), hbox);
    g_signal_connect(btn, "clicked", cb, user_data);
    return btn;
}

static void on_status_menu_save_as(GtkButton *btn, gpointer user_data) {
    popdown_ancestor_popover(GTK_WIDGET(btn));
    trigger_save_as((AppGui *)user_data);
}

static void on_read_mode_toggle_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    toggle_read_mode((AppGui *)user_data);
}


/* auto-pulled deps round 0 */
static void on_editor_border_toggled(GtkCheckButton *chk, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gboolean active = gtk_check_button_get_active(chk);
    gui->enable_editor_border = active;
    zig_set_editor_border(active ? 1 : 0);
    apply_editor_border(gui);
}

static void on_trail_color_custom_toggled(GtkCheckButton *chk, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gboolean active = gtk_check_button_get_active(chk);
    gui->use_custom_trail_color = active;
    if (gui->trail_color_btn) {
        gtk_widget_set_sensitive(gui->trail_color_btn, active);
    }
    save_trail_color_settings(gui);
    reset_cursor_trail(gui);
}

static gboolean paper_column_timeout_wrapper(gpointer data) {
    AppGui *gui = data;
    if (!gui || !gui->editor_card) return G_SOURCE_CONTINUE;
    paper_column_tick(gui->editor_card, NULL, gui);
    return G_SOURCE_CONTINUE;
}

static void on_outline_close_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppGui *gui = (AppGui *)user_data;
    if (!gui || !gui->outline_panel) return;
    gui->outline_panel_visible = FALSE;
    gtk_widget_set_visible(gui->outline_panel, FALSE);
    qirtas_pref_set_int("outline_panel_visible", 0);
}

static void on_trail_color_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    AppGui *gui = (AppGui *)user_data;
    if (!gui || !gui->use_custom_trail_color) return;
    GtkColorDialogButton *btn = GTK_COLOR_DIALOG_BUTTON(object);
    const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba(btn);
    if (rgba) {
        gui->custom_trail_color = *rgba;
        save_trail_color_settings(gui);
        reset_cursor_trail(gui);
    }
}

static void on_compact_mode_toggled(GtkCheckButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gui->compact_mode = gtk_check_button_get_active(btn);
    qirtas_pref_set_int("compact_mode", gui->compact_mode ? 1 : 0);
    apply_compact_mode(gui);
}

static void on_highlight_line_toggled(GtkCheckButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gui->highlight_current_line = gtk_check_button_get_active(btn);
    qirtas_pref_set_int("highlight_current_line", gui->highlight_current_line ? 1 : 0);
    apply_editor_prefs(gui);
}

static void on_line_numbers_toggled(GtkCheckButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gui->show_line_numbers = gtk_check_button_get_active(btn);
    qirtas_pref_set_int("show_line_numbers", gui->show_line_numbers ? 1 : 0);
    apply_editor_prefs(gui);
}

static void on_restore_session_toggled(GtkCheckButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gui->restore_session = gtk_check_button_get_active(btn);
    qirtas_pref_set_int("restore_session", gui->restore_session ? 1 : 0);
}

static void on_text_width_mode_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    AppGui *gui = (AppGui *)user_data;
    GtkDropDown *dropdown = GTK_DROP_DOWN(gobject);
    guint selected = gtk_drop_down_get_selected(dropdown);
    gui->text_width_full_page = (selected == 1);
    qirtas_pref_set_int("text_width_full_page", gui->text_width_full_page ? 1 : 0);
    if (gui->editor_card) {
        s_last_paper_width = -1; /* force the column tick to recompute */
        paper_column_tick(gui->editor_card, NULL, gui);
    }
}

static void on_focus_mode_toggled(GtkCheckButton *chk, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gboolean active = gtk_check_button_get_active(chk);
    if (gui->enable_focus_mode == active) return;
    gui->enable_focus_mode = active;
    zig_set_focus_mode(active ? 1 : 0);
    apply_focus_mode(gui);
}

static const IconPair icon_table[] = {
    { "search",      "system-search-symbolic",       "preferences-system-search-symbolic" },
    { "sidebar",     "sidebar-show-symbolic",        "view-dual-symbolic" },
    { "menu",        "open-menu-symbolic",           "view-more-symbolic" },
    { "new",         "document-new-symbolic",        "list-add-symbolic" },
    { "open",        "document-open-symbolic",       "folder-open-symbolic" },
    { "save",        "document-save-symbolic",       "media-floppy-symbolic" },
    { "saveas",      "document-save-as-symbolic",    "document-save-as-symbolic" },
    { "pdf",         "document-print-symbolic",      "x-office-document-symbolic" },
    { "folder",      "folder-symbolic",              "folder-open-symbolic" },
    { "file",        "text-x-generic-symbolic",      "emblem-documents-symbolic" },
    { "findreplace", "edit-find-replace-symbolic",   "edit-find-replace-symbolic" },
    { "fullscreen",  "view-fullscreen-symbolic",     "view-fullscreen-symbolic" },
    { "readmode",    "view-reveal-symbolic",         "view-reveal-symbolic" },
    { "prefs",       "preferences-system-symbolic",  "applications-system-symbolic" },
    { "keyboard",    "input-keyboard-symbolic",      "input-keyboard-symbolic" },
    { "quit",        "window-close-symbolic",        "application-exit-symbolic" },
    { "filemanager", "system-file-manager-symbolic", "system-file-manager-symbolic" },
};

static void on_pointer_color_custom_toggled(GtkCheckButton *chk, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gboolean active = gtk_check_button_get_active(chk);
    gui->use_custom_pointer_color = active;
    if (gui->pointer_color_btn) {
        gtk_widget_set_sensitive(gui->pointer_color_btn, active);
    }
    save_pointer_color_settings(gui);
    apply_theme(gui, current_theme);
    update_editor_font(gui);
}

static void on_pointer_color_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    AppGui *gui = (AppGui *)user_data;
    if (!gui || !gui->use_custom_pointer_color) return;
    GtkColorDialogButton *btn = GTK_COLOR_DIALOG_BUTTON(object);
    const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba(btn);
    if (rgba) {
        gui->custom_pointer_color = *rgba;
        save_pointer_color_settings(gui);
        apply_theme(gui, current_theme);
        update_editor_font(gui);
    }
}

static void apply_editor_prefs(AppGui *gui) {
    if (!gui || !gui->source_view) return;
    GtkTextView   *view = GTK_TEXT_VIEW(gui->source_view);
    GtkSourceView *sv   = GTK_SOURCE_VIEW(gui->source_view);

    gtk_text_view_set_wrap_mode(view, gui->wrap_lines ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);

    /* Flip the RTL-paragraph left-justify override (see update_paragraph_direction)
     * to match the new wrap state — avoids the right-side blank gap on Arabic
     * text when wrap is off. */
    {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(view);
        GtkTextTag *rtl_tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buf), "rtl-tag");
        if (rtl_tag) {
            g_object_set(rtl_tag,
                          "justification", GTK_JUSTIFY_LEFT,
                          "justification-set", !gui->wrap_lines,
                          NULL);
        }
    }

    gtk_source_view_set_show_line_numbers(sv, gui->show_line_numbers);
    gtk_source_view_set_highlight_current_line(sv, gui->highlight_current_line);
    gtk_source_view_set_show_right_margin(sv, gui->show_right_margin);
    if (gui->right_margin_pos < 20)  gui->right_margin_pos = 20;
    if (gui->right_margin_pos > 200) gui->right_margin_pos = 200;
    gtk_source_view_set_right_margin_position(sv, (guint)gui->right_margin_pos);
    if (gui->source_map) gtk_widget_set_visible(gui->source_map, gui->show_overview_map);
}

/* forward decls */
const char *qirtas_icon(const char *key);
static void apply_layout_dividers(AppGui *gui);
static int paper_edge_margin(AppGui *gui, GtkWidget *overlay);
static void on_column_resize_motion(GtkEventControllerMotion *ctrl, gdouble x, gdouble y, gpointer user_data);
static void on_column_resize_begin(GtkGestureDrag *gesture, gdouble x, gdouble y, gpointer user_data);
static void on_column_resize_update(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y, gpointer user_data);
static void on_column_resize_end(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y, gpointer user_data);
static void on_header_outline_clicked(GtkButton *btn, gpointer user_data);
static GtkWidget *editor_header_icon_btn(const char *icon_key, const char *tip, GCallback cb, gpointer data);
static void apply_compact_mode(AppGui *gui);

const char *qirtas_icon(const char *key) {
    for (size_t i = 0; i < G_N_ELEMENTS(icon_table); i++) {
        if (strcmp(icon_table[i].key, key) == 0)
            return qirtas_icon_style == 1 ? icon_table[i].modern : icon_table[i].classic;
    }
    return key;
}

static void apply_layout_dividers(AppGui *gui) {
    if (!gui || !gui->main_vertical_box || !gui->sidebar_editor_box) return;

    if (gui->show_layout_dividers) {
        gtk_widget_add_css_class(gui->main_vertical_box, "layout-dividers-on");
        gtk_widget_remove_css_class(gui->main_vertical_box, "layout-dividers-off");
        gtk_widget_add_css_class(gui->sidebar_editor_box, "layout-dividers-on");
        gtk_widget_remove_css_class(gui->sidebar_editor_box, "layout-dividers-off");
    } else {
        gtk_widget_add_css_class(gui->main_vertical_box, "layout-dividers-off");
        gtk_widget_remove_css_class(gui->main_vertical_box, "layout-dividers-on");
        gtk_widget_add_css_class(gui->sidebar_editor_box, "layout-dividers-off");
        gtk_widget_remove_css_class(gui->sidebar_editor_box, "layout-dividers-on");
    }
}

static int paper_edge_margin(AppGui *gui, GtkWidget *overlay) {
    (void)overlay;
    return gui->desk_gap;
}

static void on_column_resize_motion(GtkEventControllerMotion *ctrl, gdouble x, gdouble y, gpointer user_data) {
    (void)y;
    AppGui *gui = user_data;
    GtkWidget *overlay = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));
    if (gui->resizing_text_column) return;
    int width = gtk_widget_get_width(overlay);
    int margin = paper_edge_margin(gui, overlay);
    gboolean near_edge = (fabs(x - margin) < QIRTAS_RESIZE_HOTZONE) ||
                         (fabs(x - (width - margin)) < QIRTAS_RESIZE_HOTZONE);
    GdkCursor *cursor = near_edge ? gdk_cursor_new_from_name("col-resize", NULL) : NULL;
    gtk_widget_set_cursor(overlay, cursor);
    if (cursor) g_object_unref(cursor);
}

static void on_column_resize_begin(GtkGestureDrag *gesture, gdouble x, gdouble y, gpointer user_data) {
    (void)y;
    AppGui *gui = user_data;
    GtkWidget *overlay = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    int width = gtk_widget_get_width(overlay);
    int margin = paper_edge_margin(gui, overlay);

    if (fabs(x - margin) < QIRTAS_RESIZE_HOTZONE) {
        gui->resize_drag_edge = -1;
    } else if (fabs(x - (width - margin)) < QIRTAS_RESIZE_HOTZONE) {
        gui->resize_drag_edge = 1;
    } else {
        gui->resize_drag_edge = 0;
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }

    gui->resizing_text_column = TRUE;
    gui->resize_drag_start_gap = gui->desk_gap;

    /* Run the margin recompute at full frame rate only for the duration of
     * the drag; steady state relies on the low-frequency timeout instead. */
    if (gui->editor_card && gui->resize_column_tick_id == 0) {
        gui->resize_column_tick_id =
            gtk_widget_add_tick_callback(gui->editor_card, paper_column_tick, gui, NULL);
    }
}

static void on_column_resize_update(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y, gpointer user_data) {
    (void)gesture; (void)offset_y;
    AppGui *gui = user_data;
    if (!gui->resizing_text_column || gui->resize_drag_edge == 0) return;

    /* Dragging an edge outward (away from the desk centre) shrinks that
     * edge's gap; dragging inward grows it. resize_drag_edge flips the sign
     * so either edge behaves the same way relative to its own side. */
    int new_gap = gui->resize_drag_start_gap - gui->resize_drag_edge * (int)offset_x;
    if (new_gap < QIRTAS_DESK_GAP_MIN) new_gap = QIRTAS_DESK_GAP_MIN;
    if (new_gap > QIRTAS_DESK_GAP_MAX) new_gap = QIRTAS_DESK_GAP_MAX;
    if (new_gap != gui->desk_gap) {
        gui->desk_gap = new_gap;
        s_last_paper_width = -1;
        apply_editor_border(gui); /* live-resize the visible paper card */
    }
}

static void on_column_resize_end(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y, gpointer user_data) {
    (void)gesture; (void)offset_x; (void)offset_y;
    AppGui *gui = user_data;
    if (!gui->resizing_text_column) return;
    gui->resizing_text_column = FALSE;
    gui->resize_drag_edge = 0;

    if (gui->editor_card && gui->resize_column_tick_id != 0) {
        gtk_widget_remove_tick_callback(gui->editor_card, gui->resize_column_tick_id);
        gui->resize_column_tick_id = 0;
        /* Final recompute so the margins land at their settled value
         * immediately rather than waiting for the next timeout tick. */
        s_last_paper_width = -1;
        paper_column_tick(gui->editor_card, NULL, gui);
    }

    qirtas_pref_set_int("desk_gap", gui->desk_gap);
}

static void on_header_outline_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppGui *gui = (AppGui *)user_data;
    if (!gui || !gui->outline_panel) return;
    gboolean now = !gui->outline_panel_visible;
    gui->outline_panel_visible = now;
    gtk_widget_set_visible(gui->outline_panel, now);
    qirtas_pref_set_int("outline_panel_visible", now ? 1 : 0);
}

static GtkWidget *editor_header_icon_btn(const char *icon_key, const char *tip,
                                         GCallback cb, gpointer data) {
    GtkWidget *b = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(b), gtk_image_new_from_icon_name(qirtas_icon(icon_key)));
    gtk_widget_add_css_class(b, "editor-header-btn");
    if (tip) gtk_widget_set_tooltip_text(b, tip);
    if (cb) g_signal_connect(b, "clicked", cb, data);
    return b;
}


static void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    init_app_shortcuts();

    g_object_set(gtk_settings_get_default(), 
                 "gtk-cursor-blink", FALSE, 
                 "gtk-cursor-aspect-ratio", 0.02, 
                 NULL);

    /* Language + icon style must be loaded BEFORE any widget is built —
     * qirtas_tr()/qirtas_icon() are called during UI construction. */
    {
        const char *perf_env = g_getenv("QIRTAS_PERF");
        qirtas_perf_enabled = (perf_env && perf_env[0] == '1') ? 1 : 0;
    }
    qirtas_app_language = qirtas_pref_get_int("app_language", 0);
    qirtas_icon_style   = qirtas_pref_get_int("icon_style", 0);
    if (qirtas_app_language == 1) {
        gtk_widget_set_default_direction(GTK_TEXT_DIR_RTL);
    }

    /* ── Window ── */
    GtkWidget *window = adw_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Qirtas");
    gtk_window_set_default_size(GTK_WINDOW(window), 1180, 760);
    gtk_widget_set_size_request(window, 350, 250);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    AppGui *gui = g_new0(AppGui, 1);
    gui->last_scroll_requested_line = -1;
    gui->window       = window;
    g_strlcpy(gui->current_en_font, "Inter", sizeof(gui->current_en_font));
    g_strlcpy(gui->current_ar_font, "Amiri", sizeof(gui->current_ar_font));
    gui->current_font_size = 16.0;
    gui->search_visible = FALSE;
    gui->font_provider  = NULL;
    gui->css_provider   = NULL;
    gui->enable_cursor_trail = zig_get_cursor_trail();
    gui->show_layout_dividers = zig_get_layout_dividers();
    gui->enable_bottom_margin = zig_get_bottom_margin();
    gui->enable_editor_border = zig_get_editor_border();
    gui->enable_focus_mode = zig_get_focus_mode();
    load_trail_color_settings(gui);
    load_pointer_color_settings(gui);
    gui->active_tab_index = -1;

    /* Editor preferences from the app_prefs store */
    gui->wrap_lines             = qirtas_pref_get_int("wrap_lines", 1) != 0;
    gui->show_line_numbers      = qirtas_pref_get_int("show_line_numbers", 0) != 0;
    gui->highlight_current_line = qirtas_pref_get_int("highlight_current_line", 1) != 0;
    gui->show_right_margin      = qirtas_pref_get_int("show_right_margin", 0) != 0;
    gui->right_margin_pos       = qirtas_pref_get_int("right_margin_pos", 80);
    gui->text_width_full_page   = qirtas_pref_get_int("text_width_full_page", 0) != 0;
    gui->show_overview_map      = qirtas_pref_get_int("show_overview_map", 0) != 0;
    gui->restore_session        = qirtas_pref_get_int("restore_session", 1) != 0;
    gui->compact_mode           = qirtas_pref_get_int("compact_mode", 0) != 0;
    gui->desk_gap                = qirtas_pref_get_int("desk_gap", QIRTAS_DESK_GAP_DEFAULT);
    if (gui->desk_gap < QIRTAS_DESK_GAP_MIN) gui->desk_gap = QIRTAS_DESK_GAP_MIN;
    if (gui->desk_gap > QIRTAS_DESK_GAP_MAX) gui->desk_gap = QIRTAS_DESK_GAP_MAX;
    gui->text_column_width       = QIRTAS_TEXT_COLUMN_MIN; /* recomputed by paper_column_tick */


    init_css(gui);

    GtkEventController *win_key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(win_key_ctrl, "key-pressed",
                     G_CALLBACK(on_window_key_pressed), gui);
    gtk_widget_add_controller(window, win_key_ctrl);
    g_signal_connect(window, "close-request", G_CALLBACK(on_window_close_request), gui);

    /*
     * We don't need AdwToolbarView since we don't use AdwHeaderBar.
     * We set main_vertical_box as the content below.
     */

     /* 1. Resolve dynamic icon search path absolute location */
    char exe_path[PATH_MAX];
    char custom_icon_path[PATH_MAX] = "";
    ssize_t link_len = readlink("/proc/self/exe", exe_path, sizeof(exe_path)-1);
    if (link_len != -1) {
        exe_path[link_len] = '\0';
        char *dir = dirname(exe_path);
        // The binary is in <project_root>/zig-out/bin/qirtas, so we go up two levels to get the project root.
        snprintf(custom_icon_path, sizeof(custom_icon_path), "%s/../../src/ui/icons", dir);
    }

    /* 2. Retrieve active icon theme name and dynamically prepare its layout */
    gchar *theme_name = NULL;
    g_object_get(gtk_settings_get_default(), "gtk-icon-theme-name", &theme_name, NULL);

    if (theme_name && strlen(theme_name) > 0 && strlen(custom_icon_path) > 0) {
        char theme_dir[PATH_MAX];
        snprintf(theme_dir, sizeof(theme_dir), "%s/%s", custom_icon_path, theme_name);
        mkdir(theme_dir, 0755);
        
        char scalable_link[PATH_MAX];
        snprintf(scalable_link, sizeof(scalable_link), "%s/scalable", theme_dir);
        unlink(scalable_link);
        symlink("../hicolor/scalable", scalable_link);

        char apps_link[PATH_MAX];
        snprintf(apps_link, sizeof(apps_link), "%s/apps", theme_dir);
        unlink(apps_link);
        symlink("../hicolor/apps", apps_link);

        char actions_link[PATH_MAX];
        snprintf(actions_link, sizeof(actions_link), "%s/actions", theme_dir);
        unlink(actions_link);
        symlink("../hicolor/actions", actions_link);

        char status_link[PATH_MAX];
        snprintf(status_link, sizeof(status_link), "%s/status", theme_dir);
        unlink(status_link);
        symlink("../hicolor/status", status_link);
        
        char index_path[PATH_MAX];
        snprintf(index_path, sizeof(index_path), "%s/index.theme", theme_dir);
        FILE *f = fopen(index_path, "w");
        if (f) {
            fprintf(f, "[Icon Theme]\n");
            fprintf(f, "Name=%s\n", theme_name);
            fprintf(f, "Comment=%s theme\n", theme_name);
            fprintf(f, "Directories=scalable/apps,scalable/symbolic,apps/scalable,apps/symbolic,actions/symbolic,status/symbolic\n\n");
            
            fprintf(f, "[scalable/apps]\n");
            fprintf(f, "Size=256\nMinSize=8\nMaxSize=512\nType=Scalable\n\n");
            
            fprintf(f, "[scalable/symbolic]\n");
            fprintf(f, "Size=16\nMinSize=16\nMaxSize=512\nType=Scalable\nContext=Symbolic\n\n");
            
            fprintf(f, "[apps/scalable]\n");
            fprintf(f, "Size=256\nMinSize=8\nMaxSize=512\nType=Scalable\n\n");
            
            fprintf(f, "[apps/symbolic]\n");
            fprintf(f, "Size=16\nMinSize=16\nMaxSize=512\nType=Scalable\nContext=Symbolic\n\n");
            
            fprintf(f, "[actions/symbolic]\n");
            fprintf(f, "Size=16\nMinSize=16\nMaxSize=512\nType=Scalable\nContext=Symbolic\n\n");
            
            fprintf(f, "[status/symbolic]\n");
            fprintf(f, "Size=16\nMinSize=16\nMaxSize=512\nType=Scalable\nContext=Symbolic\n");
            
            fclose(f);
        }
    }
    g_free(theme_name);

    /* 3. Register custom icon theme paths to default GTK display icon theme */
    GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    gtk_icon_theme_add_search_path(icon_theme, "src/ui/icons");
    if (strlen(custom_icon_path) > 0) {
        char resolved_path[PATH_MAX];
        if (realpath(custom_icon_path, resolved_path) != NULL) {
            gtk_icon_theme_add_search_path(icon_theme, resolved_path);
        } else {
            gtk_icon_theme_add_search_path(icon_theme, custom_icon_path);
        }
    }

    AddPopoverWidgets *w = g_new0(AddPopoverWidgets, 1);
    global_add_popover_widgets = w;
    w->popover = gtk_popover_new();
    w->gui = gui;

    GtkWidget *stack_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    w->box_actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(w->box_actions, 8);
    gtk_widget_set_margin_end(w->box_actions, 8);
    gtk_widget_set_margin_top(w->box_actions, 8);
    gtk_widget_set_margin_bottom(w->box_actions, 8);

    GtkWidget *btn_open = gtk_button_new_with_label("Open Existing File");
    gtk_widget_add_css_class(btn_open, "pop-btn");
    g_signal_connect(btn_open, "clicked", G_CALLBACK(on_open_existing_clicked), gui);
    gtk_box_append(GTK_BOX(w->box_actions), btn_open);

    GtkWidget *btn_new = gtk_button_new_with_label("Create New File");
    gtk_widget_add_css_class(btn_new, "pop-btn");
    g_signal_connect(btn_new, "clicked", G_CALLBACK(on_create_new_clicked), w);
    gtk_box_append(GTK_BOX(w->box_actions), btn_new);



    w->box_input = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_visible(w->box_input, FALSE);
    gtk_widget_set_margin_start(w->box_input, 8);
    gtk_widget_set_margin_end(w->box_input, 8);
    gtk_widget_set_margin_top(w->box_input, 8);
    gtk_widget_set_margin_bottom(w->box_input, 8);

    GtkWidget *lbl_prompt = gtk_label_new(qirtas_tr("Enter file name:"));
    gtk_widget_add_css_class(lbl_prompt, "pop-section-label");
    gtk_widget_set_halign(lbl_prompt, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(w->box_input), lbl_prompt);

    w->entry_name = gtk_entry_new();
    gtk_widget_add_css_class(w->entry_name, "search-entry");
    g_signal_connect(w->entry_name, "activate", G_CALLBACK(on_create_submit), w);
    gtk_box_append(GTK_BOX(w->box_input), w->entry_name);

    gtk_box_append(GTK_BOX(stack_box), w->box_actions);
    gtk_box_append(GTK_BOX(stack_box), w->box_input);

    gtk_popover_set_child(GTK_POPOVER(w->popover), stack_box);
    /* Will be attached to btn_add in sidebar below */

    g_signal_connect(w->popover, "closed", G_CALLBACK(on_popover_closed), w);
    g_signal_connect_swapped(w->popover, "destroy", G_CALLBACK(g_free), w);

    /* ============================================================
     * SIDEBAR & WIDGETS
     * ============================================================ */
    gui->btn_editor = NULL;
    gui->btn_files = NULL;
    gui->btn_search = NULL;
    gui->btn_status_actions = NULL;


    /* ── Root box: main_vertical_box holding (sidebar+stack horizontal box) and bottom_bar ── */
    GtkWidget *main_vertical_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(main_vertical_box, "main-vertical-box");
    gtk_widget_set_vexpand(main_vertical_box, TRUE);
    gtk_widget_set_hexpand(main_vertical_box, TRUE);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), main_vertical_box);
    gui->main_vertical_box = main_vertical_box;
    apply_layout_dividers(gui);

    GtkWidget *sidebar_editor_box = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(sidebar_editor_box, TRUE);
    gtk_widget_set_hexpand(sidebar_editor_box, TRUE);
    gui->sidebar_editor_box = sidebar_editor_box;
    gui->root_box = sidebar_editor_box;
    apply_layout_dividers(gui);

    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(sidebar, "sidebar");
    gtk_widget_set_size_request(sidebar, 220, -1);
    gtk_widget_set_visible(sidebar, TRUE);
    gtk_widget_set_halign(sidebar, GTK_ALIGN_FILL);
    gtk_widget_set_valign(sidebar, GTK_ALIGN_FILL);
    gtk_paned_set_start_child(GTK_PANED(sidebar_editor_box), sidebar);
    gtk_paned_set_resize_start_child(GTK_PANED(sidebar_editor_box), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(sidebar_editor_box), FALSE);
    gtk_paned_set_position(GTK_PANED(sidebar_editor_box), 260);
    gui->sidebar = sidebar;

    /* ── Brand header: قرطاس feather logo + المكتبة label ── */
    {
        GtkWidget *brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(brand, "sidebar-header");

        /* Built once as a plain GtkImage; gui_update_brand_logo() fills in
         * the light- or dark-mode PNG (called here and on every theme
         * change) and forces a recognizable on-screen size — GtkImage
         * doesn't size paintables to their intrinsic dimensions by default. */
        GtkWidget *logo = gtk_image_new();
        gtk_widget_add_css_class(logo, "sidebar-brand-logo");
        gui->logo_image = logo;
        gui_update_brand_logo(gui);
        gtk_box_append(GTK_BOX(brand), logo);

        GtkWidget *libr = gtk_label_new(qirtas_tr("Library"));
        gtk_widget_add_css_class(libr, "sidebar-brand-sub");
        gtk_widget_set_hexpand(libr, TRUE);
        gtk_widget_set_halign(libr, GTK_ALIGN_END);
        gtk_box_append(GTK_BOX(brand), libr);

        gtk_box_append(GTK_BOX(sidebar), brand);
    }

    /* 1. Global Workspace Search Entry */
    GtkWidget *exp_search = gtk_search_entry_new();
    gtk_widget_add_css_class(exp_search, "workspace-search");
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(exp_search), "Search in workspace...");
    gtk_box_append(GTK_BOX(sidebar), exp_search);
    gui->exp_search_entry = exp_search;
    g_signal_connect(exp_search, "search-changed", G_CALLBACK(on_explorer_search_changed), gui);

    /* Parent the add popover to the sidebar so it can be displayed */
    gtk_widget_set_parent(w->popover, sidebar);


    /* The OUTLINE TOC now lives on the desk, left of the paper card
     * (built as gui->outline_panel below), not in the sidebar. */

    /* Notes section header (Title + Count Badge) */
    GtkWidget *exp_title_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(exp_title_row, 12);
    gtk_widget_set_margin_bottom(exp_title_row, 4);

    GtkWidget *exp_title = gtk_label_new(qirtas_tr("Notes"));
    gtk_widget_add_css_class(exp_title, "explorer-title");
    gtk_widget_set_halign(exp_title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(exp_title_row), exp_title);

    gui->exp_count_label = gtk_label_new("0 items");
    gtk_widget_add_css_class(gui->exp_count_label, "explorer-badge");
    gtk_widget_set_halign(gui->exp_count_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(exp_title_row), gui->exp_count_label);

    gtk_box_append(GTK_BOX(sidebar), exp_title_row);

    /* 3. Scrolled Window & Tree Container for File Tree */
    GtkWidget *exp_scroll = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(exp_scroll, TRUE);
    gtk_widget_set_vexpand(exp_scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(exp_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(sidebar), exp_scroll);

    /* Tree root box — replaces the old GtkListBox */
    GtkWidget *tree_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(tree_box, "tree-container");
    gtk_widget_set_hexpand(tree_box, TRUE);
    gtk_widget_set_margin_start(tree_box, 4);
    gtk_widget_set_margin_end(tree_box, 4);
    gtk_widget_set_margin_top(tree_box, 2);
    gtk_widget_set_margin_bottom(tree_box, 4);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(exp_scroll), tree_box);
    gui->explorer_listbox = tree_box;  /* reuse field — now a GtkBox */

    /* 4. Bottom nav group (Settings) */
    GtkWidget *nav_bot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign(nav_bot, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(nav_bot, 4);
    gtk_box_append(GTK_BOX(sidebar), nav_bot);

    /* Vault switcher — moved here from Settings */
    gtk_box_append(GTK_BOX(nav_bot), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *vault_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(vault_row, "sidebar-vault-row");
    gtk_widget_set_margin_top(vault_row, 4);

    char cwd_buf[PATH_MAX];
    if (getcwd(cwd_buf, sizeof(cwd_buf)) == NULL) {
        strcpy(cwd_buf, "Unknown");
    }
    gui->vault_path_lbl_val = gtk_label_new(cwd_buf);
    gtk_widget_add_css_class(gui->vault_path_lbl_val, "stats-value");
    gtk_widget_set_hexpand(gui->vault_path_lbl_val, TRUE);
    gtk_widget_set_halign(gui->vault_path_lbl_val, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(gui->vault_path_lbl_val), PANGO_ELLIPSIZE_START);
    gtk_label_set_max_width_chars(GTK_LABEL(gui->vault_path_lbl_val), 16);
    gtk_widget_set_tooltip_text(gui->vault_path_lbl_val, cwd_buf);

    GtkWidget *vault_open_btn = gtk_button_new_from_icon_name("folder-open-symbolic");
    gtk_widget_add_css_class(vault_open_btn, "flat");
    gtk_widget_set_tooltip_text(vault_open_btn, qirtas_tr("Open Vault"));
    g_signal_connect(vault_open_btn, "clicked", G_CALLBACK(on_open_vault_clicked), gui);

    gtk_box_append(GTK_BOX(vault_row), gui->vault_path_lbl_val);
    gtk_box_append(GTK_BOX(vault_row), vault_open_btn);
    gtk_box_append(GTK_BOX(nav_bot), vault_row);

    gui->btn_stats = NULL;

    /* Settings moved to the status-bar menu (Ctrl+, still works). */
    gui->btn_settings = NULL;

    /* ============================================================
     * STACK
     * ============================================================ */
    GtkWidget *stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(stack), 180);
    gtk_widget_set_hexpand(stack, TRUE);
    gtk_widget_set_vexpand(stack, TRUE);
    gtk_widget_add_css_class(stack, "workspace");
    gtk_paned_set_end_child(GTK_PANED(sidebar_editor_box), stack);
    gtk_paned_set_resize_end_child(GTK_PANED(sidebar_editor_box), TRUE);
    gtk_paned_set_shrink_end_child(GTK_PANED(sidebar_editor_box), FALSE);
    gui->stack = stack;

    GtkGesture *workspace_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(workspace_click), GDK_BUTTON_PRIMARY);
    g_signal_connect(workspace_click, "pressed", G_CALLBACK(on_workspace_click), gui);
    gtk_widget_add_controller(stack, GTK_EVENT_CONTROLLER(workspace_click));

    /* ============================================================
     * PAGE 1 — EDITOR
     * ============================================================ */
    GtkWidget *editor_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(editor_page, "editor-page");
    gtk_stack_add_named(GTK_STACK(stack), editor_page, "editor");
    gui->editor_page = editor_page;

    /* Search revealer (configured to show at bottom, slide up) */
    GtkWidget *search_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(search_revealer),
                                      GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_transition_duration(GTK_REVEALER(search_revealer), 200);
    gtk_revealer_set_reveal_child(GTK_REVEALER(search_revealer), FALSE);
    gui->search_revealer = search_revealer;

    GtkWidget *sbar_outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(sbar_outer, "search-bar-box");
    gtk_revealer_set_child(GTK_REVEALER(search_revealer), sbar_outer);

    GtkWidget *sbar_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(sbar_outer), sbar_box);

    GtkWidget *sbar_icon = gtk_image_new_from_icon_name(qirtas_icon("search"));
    gtk_box_append(GTK_BOX(sbar_box), sbar_icon);

    GtkWidget *search_entry = gtk_search_entry_new();
    gtk_widget_add_css_class(search_entry, "search-entry");
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search_entry),
                                           qirtas_tr("Search in file…"));
    gtk_box_append(GTK_BOX(sbar_box), search_entry);
    gui->search_entry = search_entry;

    GtkWidget *match_lbl = gtk_label_new("");
    gtk_widget_add_css_class(match_lbl, "search-match-label");
    gtk_box_append(GTK_BOX(sbar_box), match_lbl);
    gui->search_match_label = match_lbl;

    GtkWidget *btn_prev = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_accessible_update_property(GTK_ACCESSIBLE(btn_prev),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Previous match", -1);
    gtk_widget_add_css_class(btn_prev, "search-nav-btn");
    gtk_widget_set_tooltip_text(btn_prev, qirtas_tr("Previous match"));
    gtk_box_append(GTK_BOX(sbar_box), btn_prev);

    GtkWidget *btn_next = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_accessible_update_property(GTK_ACCESSIBLE(btn_next),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Next match", -1);
    gtk_widget_add_css_class(btn_next, "search-nav-btn");
    gtk_widget_set_tooltip_text(btn_next, qirtas_tr("Next match"));
    gtk_box_append(GTK_BOX(sbar_box), btn_next);

    GtkWidget *btn_close_s = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_accessible_update_property(GTK_ACCESSIBLE(btn_close_s),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Close search bar", -1);
    gtk_widget_add_css_class(btn_close_s, "search-nav-btn");
    gtk_widget_set_tooltip_text(btn_close_s, qirtas_tr("Close (Esc)"));
    gtk_widget_set_hexpand(btn_close_s, TRUE);
    gtk_widget_set_halign(btn_close_s, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(sbar_box), btn_close_s);

    /* ── Replace row ── */
    GtkWidget *rbar_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(sbar_outer), rbar_box);

    GtkWidget *rbar_icon = gtk_image_new_from_icon_name("edit-find-replace-symbolic");
    gtk_box_append(GTK_BOX(rbar_box), rbar_icon);

    GtkWidget *replace_entry = gtk_entry_new();
    gtk_widget_add_css_class(replace_entry, "search-entry");
    gtk_entry_set_placeholder_text(GTK_ENTRY(replace_entry), qirtas_tr("Replace with…"));
    gtk_box_append(GTK_BOX(rbar_box), replace_entry);
    gui->replace_entry = replace_entry;

    GtkWidget *btn_replace = gtk_button_new_with_label(qirtas_tr("Replace"));
    gtk_widget_add_css_class(btn_replace, "search-nav-btn");
    gtk_widget_set_tooltip_text(btn_replace, qirtas_tr("Replace current match"));
    g_signal_connect(btn_replace, "clicked", G_CALLBACK(on_replace_clicked), gui);
    gtk_box_append(GTK_BOX(rbar_box), btn_replace);

    GtkWidget *btn_replace_all = gtk_button_new_with_label(qirtas_tr("All"));
    gtk_widget_add_css_class(btn_replace_all, "search-nav-btn");
    gtk_widget_set_tooltip_text(btn_replace_all, qirtas_tr("Replace all matches"));
    g_signal_connect(btn_replace_all, "clicked", G_CALLBACK(on_replace_all_clicked), gui);
    gtk_box_append(GTK_BOX(rbar_box), btn_replace_all);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gui->scrolled = scrolled;
    /* Keep the scrollbar on the right regardless of UI language (see the
     * matching gtk_widget_set_direction on source_view below). */
    gtk_widget_set_direction(scrolled, GTK_TEXT_DIR_LTR);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(scrolled, TRUE);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_widget_set_halign(scrolled, GTK_ALIGN_FILL);
    gtk_widget_set_valign(scrolled, GTK_ALIGN_FILL);
    /* Margins live on the .editor-card wrapper now (apply_editor_border). */
    gtk_widget_add_css_class(scrolled, "editor-scroll");

    /* ── Cursor-trail Overlay ──
     * We wrap `scrolled` inside a GtkOverlay so that the transparent
     * GtkDrawingArea (cursor_trail_area) can float on top of the text,
     * receiving paint calls every frame via the tick callback while
     * passing all pointer/keyboard events straight through to the editor.
     */
    GtkWidget *editor_overlay = gtk_overlay_new();
    gtk_widget_set_hexpand(editor_overlay, TRUE);
    gtk_widget_set_vexpand(editor_overlay, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(editor_overlay), scrolled);

    /* The drawing area sits on top — transparent, non-interactive */
    GtkWidget *trail_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(trail_area, TRUE);
    gtk_widget_set_vexpand(trail_area, TRUE);
    gtk_widget_set_can_target(trail_area, FALSE);   /* pass events through  */
    gtk_widget_set_focusable(trail_area, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(editor_overlay), trail_area);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(editor_overlay), trail_area, FALSE);
    gui->cursor_trail_area = trail_area;

    /* Resizable centred text column: cursor hint + drag handles on the
     * paper's left/right margins (see on_column_resize_* above). */
    GtkEventController *col_motion = gtk_event_controller_motion_new();
    g_signal_connect(col_motion, "motion", G_CALLBACK(on_column_resize_motion), gui);
    gtk_widget_add_controller(editor_overlay, col_motion);

    GtkGesture *col_drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(col_drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(col_drag, "drag-begin", G_CALLBACK(on_column_resize_begin), gui);
    g_signal_connect(col_drag, "drag-update", G_CALLBACK(on_column_resize_update), gui);
    g_signal_connect(col_drag, "drag-end", G_CALLBACK(on_column_resize_end), gui);
    gtk_widget_add_controller(editor_overlay, GTK_EVENT_CONTROLLER(col_drag));

    /* ── Paper card wrapper ──
     * The thread, the header band, and the scrolling text now live inside
     * one card so they read as a single sheet of paper. The card carries
     * the border / radius / shadow / desk margins (apply_editor_border). */
    GtkWidget *editor_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(editor_card, "editor-card");
    gtk_widget_set_hexpand(editor_card, TRUE);
    gtk_widget_set_vexpand(editor_card, TRUE);
    gui->editor_card = editor_card;

    /* 2px gold/navy thread along the very top edge of the card. */
    GtkWidget *editor_thread = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(editor_thread, "editor-thread");
    gtk_widget_set_size_request(editor_thread, -1, 2);
    gui->editor_thread = editor_thread;
    gtk_box_append(GTK_BOX(editor_card), editor_thread);

    /* 46px header band: breadcrumb on the start side, icon cluster on the
     * end side. Kept LTR so the icon order is stable. */
    GtkWidget *editor_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(editor_header, "editor-header");
    gtk_widget_set_size_request(editor_header, -1, 46);
    gui->editor_header = editor_header;

    /* Sidebar (files) toggle sits right beside the path — at the start of the
     * header, so it lands to the right of the path under Arabic (RTL) and to
     * its left under English (LTR). The search / outline / menu cluster lives
     * at the opposite end. */
    gtk_box_append(GTK_BOX(editor_header),
        editor_header_icon_btn("sidebar", qirtas_tr("Toggle Sidebar (Ctrl+\\)"),
                               G_CALLBACK(on_logo_clicked), gui));

    GtkWidget *breadcrumb = gtk_label_new("");
    gtk_widget_add_css_class(breadcrumb, "editor-breadcrumb");
    gtk_label_set_ellipsize(GTK_LABEL(breadcrumb), PANGO_ELLIPSIZE_START);
    gtk_widget_set_halign(breadcrumb, GTK_ALIGN_START);
    gtk_widget_set_hexpand(breadcrumb, TRUE);
    gui->breadcrumb_label = breadcrumb;
    gtk_box_append(GTK_BOX(editor_header), breadcrumb);

    /* End cluster: outline toggle (list icon). The 🔍 search and ⋮ menu are
     * the real status widgets, reparented in from the bottom-bar section. */
    gtk_box_append(GTK_BOX(editor_header),
        editor_header_icon_btn("view-list-symbolic", qirtas_tr("Toggle Outline"),
                               G_CALLBACK(on_header_outline_clicked), gui));
    gtk_box_append(GTK_BOX(editor_card), editor_header);

    gtk_box_append(GTK_BOX(editor_card), editor_overlay);

    /* ── Desk: paper card + outline panel, both resizable ──
     * A GtkPaned so the divider between the paper and the outline can be
     * dragged. Code order [card][outline]; under RTL the outline lands on
     * the LEFT of the paper (marginalia), under LTR on the right. */
    GtkWidget *desk_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(desk_paned, "desk-paned");
    gtk_widget_set_hexpand(desk_paned, TRUE);
    gtk_widget_set_vexpand(desk_paned, TRUE);
    gtk_paned_set_start_child(GTK_PANED(desk_paned), editor_card);
    gtk_paned_set_resize_start_child(GTK_PANED(desk_paned), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(desk_paned), FALSE);

    /* Outline panel — a plain box (toggled via visibility, sized via the
     * paned handle). Kept narrow: about a heading line's width. */
    GtkWidget *outline_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(outline_panel, "outline-panel");
    gtk_widget_set_size_request(outline_panel, 150, -1);
    gui->outline_panel = outline_panel;
    gui->outline_panel_inner = outline_panel;

    GtkWidget *outline_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *outline_title = gtk_label_new(qirtas_tr("Outline"));
    gtk_widget_add_css_class(outline_title, "outline-panel-title");
    gtk_widget_set_halign(outline_title, GTK_ALIGN_START);
    gtk_widget_set_hexpand(outline_title, TRUE);
    gtk_box_append(GTK_BOX(outline_head), outline_title);

    GtkWidget *outline_close = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(outline_close, "outline-close-btn");
    gtk_widget_set_tooltip_text(outline_close, qirtas_tr("Close Outline"));
    g_signal_connect(outline_close, "clicked", G_CALLBACK(on_outline_close_clicked), gui);
    gtk_box_append(GTK_BOX(outline_head), outline_close);
    gtk_box_append(GTK_BOX(outline_panel), outline_head);

    GtkWidget *outline_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(outline_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(outline_scroll, TRUE);

    GtkWidget *outline_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_add_css_class(outline_box, "tree-container");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(outline_scroll), outline_box);
    gui->outline_box = outline_box;
    gtk_box_append(GTK_BOX(outline_panel), outline_scroll);

    gtk_paned_set_end_child(GTK_PANED(desk_paned), outline_panel);
    gtk_paned_set_resize_end_child(GTK_PANED(desk_paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(desk_paned), FALSE);

    /* Restore the saved visibility (default: shown). */
    gui->outline_panel_visible = qirtas_pref_get_int("outline_panel_visible", 1) != 0;
    gtk_widget_set_visible(outline_panel, gui->outline_panel_visible);

    gtk_box_append(GTK_BOX(editor_page), desk_paned);

    /* Drive the centred text column. Steady state polls at ~8fps (the sig
     * check inside paper_column_tick makes this a no-op when nothing
     * changed); a 60fps tick callback runs only during an active drag, see
     * on_column_resize_begin/_end. */
    paper_column_tick(editor_card, NULL, gui);
    g_timeout_add(120, paper_column_timeout_wrapper, gui);

    /* Append search bar after content so it sits at the bottom */
    gtk_box_append(GTK_BOX(editor_page), search_revealer);

    /* The source view is the scrolled window's DIRECT child, so it acts
     * as a native GtkScrollable. GTK then validates Pango layout lazily
     * (visible lines only) instead of allocating the widget at full
     * document height — that full-height layout froze the UI for
     * seconds when opening big files, and made every keystroke
     * re-validate the whole document. It also lets GtkTextView's own
     * scroll-to-cursor logic work, so the viewport tracks the caret in
     * the same frame. Do NOT wrap it in a GtkBox again. */
    GtkWidget *source_view = gtk_source_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(source_view), GTK_WRAP_WORD_CHAR);
    /* Keep the line-number gutter and scrollbar on the left/right exactly as
     * in the English layout, even when app_language=Arabic flips the rest of
     * the UI to RTL via gtk_widget_set_default_direction(). Per-paragraph
     * Arabic text direction is handled separately by the rtl-tag/ltr-tag
     * text tags, so this only affects widget-chrome placement, not text. */
    gtk_widget_set_direction(source_view, GTK_TEXT_DIR_LTR);
    gtk_widget_set_hexpand(source_view, TRUE);
    gtk_widget_set_vexpand(source_view, TRUE);
    gtk_widget_set_halign(source_view, GTK_ALIGN_FILL);
    gtk_widget_set_valign(source_view, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(source_view, "editor-source");
    gui->source_view = source_view;

    /* Legacy aliases from the old virtual-layout box; coordinate
     * conversions that targeted the box now resolve to the view itself. */
    gui->virtual_layout_box = source_view;
    gui->top_spacer = NULL;
    gui->bottom_spacer = NULL;

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), source_view);

    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
    gui->vadjustment = vadj;

    /* Overview map — floats over the right edge of the paper card */
    GtkWidget *source_map = gtk_source_map_new();
    gtk_source_map_set_view(GTK_SOURCE_MAP(source_map), GTK_SOURCE_VIEW(source_view));
    gtk_widget_add_css_class(source_map, "overview-map");
    gtk_widget_set_halign(source_map, GTK_ALIGN_END);
    gtk_widget_set_valign(source_map, GTK_ALIGN_FILL);
    gtk_widget_set_margin_end(source_map, 30);
    gtk_widget_set_margin_top(source_map, 26);
    gtk_widget_set_margin_bottom(source_map, 22);
    gtk_widget_set_visible(source_map, gui->show_overview_map);
    gtk_overlay_add_overlay(GTK_OVERLAY(editor_overlay), source_map);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(editor_overlay), source_map, FALSE);
    gui->source_map = source_map;

    apply_editor_prefs(gui);


    /* Typography & Render spacing (line height equivalent) */
    /* Left/right margins are recomputed every frame by paper_column_tick to
     * keep the text column centred at ~680px; these are just initial values. */
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(source_view), 64);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(source_view), 64);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(source_view), 56);
    gtk_text_view_set_pixels_above_lines(GTK_TEXT_VIEW(source_view), 5);
    gtk_text_view_set_pixels_below_lines(GTK_TEXT_VIEW(source_view), 5);
    gtk_text_view_set_pixels_inside_wrap(GTK_TEXT_VIEW(source_view), 6);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(source_view), gui->enable_bottom_margin ? 160 : 0);

    apply_editor_border(gui);


    /* Highlight Current Line & Show Line Numbers */
    gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(source_view), TRUE);
    gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(source_view), TRUE);
    gtk_source_view_set_auto_indent(GTK_SOURCE_VIEW(source_view), TRUE);
    gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(source_view), 4);
    gtk_source_view_set_indent_width(GTK_SOURCE_VIEW(source_view), 4);
    gtk_source_view_set_insert_spaces_instead_of_tabs(GTK_SOURCE_VIEW(source_view), TRUE);
    gtk_source_view_set_smart_backspace(GTK_SOURCE_VIEW(source_view), TRUE);

    /* Re-apply stored editor prefs AFTER the hard-coded defaults above
     * so user settings (wrap, line numbers, margins…) win. */
    apply_editor_prefs(gui);

    /* Markdown syntax highlighting */
    GtkSourceBuffer *src_buf =
        GTK_SOURCE_BUFFER(gtk_text_view_get_buffer(GTK_TEXT_VIEW(source_view)));
    gtk_text_buffer_set_enable_undo(GTK_TEXT_BUFFER(src_buf), FALSE);
    gtk_source_buffer_set_highlight_syntax(src_buf, TRUE);
    GtkSourceLanguageManager *lm = gtk_source_language_manager_get_default();
    /* Try absolute path first (works regardless of CWD), then relative fallback */
    {
        char abs_ui_path[1024];
        char exe_path[512] = {0};
        ssize_t exe_len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (exe_len > 0) {
            exe_path[exe_len] = '\0';
            /* Strip binary name to get the directory */
            char *last_slash = strrchr(exe_path, '/');
            if (last_slash) *last_slash = '\0';
            /* Navigate up from zig-out/bin/ to project root, then into src/ui */
            snprintf(abs_ui_path, sizeof(abs_ui_path), "%s/../../src/ui", exe_path);
            gtk_source_language_manager_append_search_path(lm, abs_ui_path);
        }
    }
    gtk_source_language_manager_append_search_path(lm, "src/ui");
    GtkSourceLanguage *lang = gtk_source_language_manager_get_language(lm, "qirtas_markdown");
    if (!lang) lang = gtk_source_language_manager_get_language(lm, "markdown");
    if (lang) gtk_source_buffer_set_language(src_buf, lang);

    GtkSourceStyleSchemeManager *sm = gtk_source_style_scheme_manager_get_default();
    {
        char abs_ui_path[1024];
        char exe_path[512] = {0};
        ssize_t exe_len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (exe_len > 0) {
            exe_path[exe_len] = '\0';
            char *last_slash = strrchr(exe_path, '/');
            if (last_slash) *last_slash = '\0';
            snprintf(abs_ui_path, sizeof(abs_ui_path), "%s/../../src/ui", exe_path);
            gtk_source_style_scheme_manager_append_search_path(sm, abs_ui_path);
        }
    }
    gtk_source_style_scheme_manager_append_search_path(sm, "src/ui");
    GtkSourceStyleScheme *scheme =
        gtk_source_style_scheme_manager_get_scheme(sm, "qirtas-dark");
    if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "adwaita-dark");
    if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "oblivion");
    if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "solarized-dark");
    if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "classic-dark");
    if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "cobalt");
    if (scheme) gtk_source_buffer_set_style_scheme(src_buf, scheme);

    /* Search context */
    GtkSourceSearchSettings *ss = gtk_source_search_settings_new();
    gtk_source_search_settings_set_case_sensitive(ss, FALSE);
    gtk_source_search_settings_set_wrap_around(ss, TRUE);
    gtk_source_search_settings_set_regex_enabled(ss, TRUE);
    gui->search_settings = ss;
    gui->search_ctx = gtk_source_search_context_new(src_buf, ss);
    g_signal_connect(gui->search_ctx, "notify::occurrences-count",
                     G_CALLBACK(on_search_occurrences_changed), gui);

    /* Wire search and text buffer direction signals */
    g_signal_connect(src_buf, "insert-text", G_CALLBACK(on_insert_text_before), gui);
    g_signal_connect_after(src_buf, "insert-text", G_CALLBACK(on_insert_text_after), gui);
    g_signal_connect(src_buf, "delete-range", G_CALLBACK(on_delete_range_before), gui);
    g_signal_connect_after(src_buf, "delete-range", G_CALLBACK(on_delete_range_after), gui);
    g_signal_connect(src_buf, "mark-set", G_CALLBACK(on_mark_set), gui);
    g_signal_connect(src_buf, "notify::cursor-position", G_CALLBACK(on_cursor_position_changed), gui);

    g_signal_connect(search_entry, "search-changed",
                     G_CALLBACK(on_search_text_changed), gui);
    g_signal_connect(btn_prev, "clicked", G_CALLBACK(on_search_prev_clicked), gui);
    g_signal_connect(btn_next, "clicked", G_CALLBACK(on_search_next_clicked), gui);
    g_signal_connect(btn_close_s, "clicked", G_CALLBACK(on_close_search_clicked), gui);

    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed",
                     G_CALLBACK(on_search_entry_key), gui);
    gtk_widget_add_controller(search_entry, key_ctrl);

    GtkEventController *editor_key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(editor_key_ctrl, "key-pressed",
                     G_CALLBACK(on_editor_key_pressed), gui);
    gtk_widget_add_controller(source_view, editor_key_ctrl);

    /* ── Cursor-trail: wire draw function + frame-clock tick ── */
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(gui->cursor_trail_area),
        draw_cursor_trail,
        gui,
        NULL  /* GDestroyNotify — not needed */
    );
    cursor_trail_wake(gui);



    /* ============================================================
     * SETTINGS WINDOW
     * ============================================================ */
    GtkWidget *pop_box  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(pop_box, "settings-sheet-body");
    gtk_widget_set_margin_start(pop_box, 14);
    gtk_widget_set_margin_end(pop_box, 14);
    gtk_widget_set_margin_top(pop_box, 14);
    gtk_widget_set_margin_bottom(pop_box, 14);

    GtkWidget *th_lbl = gtk_label_new(qirtas_tr("APPEARANCE"));
    gtk_widget_add_css_class(th_lbl, "pop-section-label");
    gtk_widget_set_halign(th_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pop_box), th_lbl);

    GtkWidget *theme_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *theme_label = gtk_label_new(qirtas_tr("Theme"));
    gtk_widget_set_hexpand(theme_label, TRUE);
    gtk_widget_set_halign(theme_label, GTK_ALIGN_START);

    const char *themes[] = {
        "Classic Sepia",
        "Typewriter Light",
        "Typewriter Dark",
        "Qirtas Ink",
        "Qirtas Ink Dark",
        "Paper & Ink Navy",
        "Add Custom Theme...",
        NULL
    };
    GtkWidget *theme_dropdown = gtk_drop_down_new_from_strings(themes);

    int theme_idx = 0;
    if (strcmp(current_theme, "sepia") == 0) theme_idx = 0;
    else if (strcmp(current_theme, "typewriter-light") == 0) theme_idx = 1;
    else if (strcmp(current_theme, "typewriter-dark") == 0) theme_idx = 2;
    else if (strcmp(current_theme, "qirtas") == 0) theme_idx = 3;
    else if (strcmp(current_theme, "qirtas-dark") == 0) theme_idx = 4;
    else if (strcmp(current_theme, "navy") == 0) theme_idx = 5;
    else if (strcmp(current_theme, "custom") == 0) theme_idx = 6;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(theme_dropdown), theme_idx);
    
    g_signal_connect(theme_dropdown, "notify::selected", G_CALLBACK(on_theme_dropdown_changed), gui);
    
    gtk_box_append(GTK_BOX(theme_row), theme_label);
    gtk_box_append(GTK_BOX(theme_row), theme_dropdown);
    gtk_box_append(GTK_BOX(pop_box), theme_row);

    GtkWidget *font_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *font_lbl = gtk_label_new(qirtas_tr("Font Size"));
    gtk_widget_set_hexpand(font_lbl, TRUE);
    gtk_widget_set_halign(font_lbl, GTK_ALIGN_START);
    GtkWidget *font_spin = gtk_spin_button_new_with_range(10.0, 26.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(font_spin), 16.0);
    g_signal_connect(font_spin, "value-changed", G_CALLBACK(on_font_size_changed), gui);
    on_font_size_changed(GTK_SPIN_BUTTON(font_spin), gui);
    gtk_box_append(GTK_BOX(font_row), font_lbl);
    gtk_box_append(GTK_BOX(font_row), font_spin);
    gtk_box_append(GTK_BOX(pop_box), font_row);

    // English Font Selection
    GtkWidget *en_font_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *en_font_lbl = gtk_label_new(qirtas_tr("English Font"));
    gtk_widget_set_hexpand(en_font_lbl, TRUE);
    gtk_widget_set_halign(en_font_lbl, GTK_ALIGN_START);
    const char *en_fonts[] = {
        "Inter", "Lora", "Merriweather", "JetBrains Mono",
        "Roboto", "Fira Code", "Source Code Pro", "Add Custom Font...", NULL
    };
    GtkWidget *en_font_dropdown = gtk_drop_down_new_from_strings(en_fonts);
    int en_idx = 0;
    for (int i = 0; i < 7; i++) {
        if (strcmp(gui->current_en_font, en_fonts[i]) == 0) { en_idx = i; break; }
    }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(en_font_dropdown), en_idx);
    g_signal_connect(en_font_dropdown, "notify::selected", G_CALLBACK(on_en_font_changed), gui);
    gtk_box_append(GTK_BOX(en_font_row), en_font_lbl);
    gtk_box_append(GTK_BOX(en_font_row), en_font_dropdown);
    gtk_box_append(GTK_BOX(pop_box), en_font_row);

    // Arabic Font Selection
    GtkWidget *ar_font_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *ar_font_lbl = gtk_label_new(qirtas_tr("Arabic Font"));
    gtk_widget_set_hexpand(ar_font_lbl, TRUE);
    gtk_widget_set_halign(ar_font_lbl, GTK_ALIGN_START);
    const char *ar_fonts_dd[] = {
        "Amiri", "Cairo", "IBM Plex Sans Arabic",
        "KFGQPC Uthman Taha Naskh", "Noto Naskh Arabic", NULL
    };
    GtkWidget *ar_font_dropdown = gtk_drop_down_new_from_strings(ar_fonts_dd);
    int ar_idx = 0;
    for (int i = 0; i < 5; i++) {
        if (strcmp(gui->current_ar_font, ar_fonts_dd[i]) == 0) { ar_idx = i; break; }
    }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(ar_font_dropdown), ar_idx);
    g_signal_connect(ar_font_dropdown, "notify::selected", G_CALLBACK(on_ar_font_changed), gui);
    gtk_box_append(GTK_BOX(ar_font_row), ar_font_lbl);
    gtk_box_append(GTK_BOX(ar_font_row), ar_font_dropdown);
    gtk_box_append(GTK_BOX(pop_box), ar_font_row);

    GtkWidget *compact_chk = gtk_check_button_new_with_label(qirtas_tr("Compact Layout"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(compact_chk), gui->compact_mode);
    g_signal_connect(compact_chk, "toggled", G_CALLBACK(on_compact_mode_toggled), gui);
    gtk_box_append(GTK_BOX(pop_box), compact_chk);

    GtkWidget *border_chk = gtk_check_button_new_with_label(qirtas_tr("Show editor border"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(border_chk), gui->enable_editor_border);
    g_signal_connect(border_chk, "toggled", G_CALLBACK(on_editor_border_toggled), gui);
    gtk_box_append(GTK_BOX(pop_box), border_chk);

    gui->focus_chk = gtk_check_button_new_with_label(qirtas_tr("Focus Mode"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(gui->focus_chk), gui->enable_focus_mode);
    g_signal_connect(gui->focus_chk, "toggled", G_CALLBACK(on_focus_mode_toggled), gui);
    gtk_box_append(GTK_BOX(pop_box), gui->focus_chk);

    GtkWidget *trail_chk = gtk_check_button_new_with_label(qirtas_tr("Pointer Trail Animation"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(trail_chk), gui->enable_cursor_trail);
    g_signal_connect(trail_chk, "toggled", G_CALLBACK(on_trail_toggled), gui);
    gtk_box_append(GTK_BOX(pop_box), trail_chk);

    GtkWidget *trail_color_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *trail_color_lbl = gtk_label_new(qirtas_tr("Trail Color"));
    gtk_widget_set_hexpand(trail_color_lbl, TRUE);
    gtk_widget_set_halign(trail_color_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(trail_color_row), trail_color_lbl);

    GtkColorDialog *color_dialog = gtk_color_dialog_new();
    gtk_color_dialog_set_modal(color_dialog, TRUE);
    GtkWidget *trail_color_btn = gtk_color_dialog_button_new(color_dialog);
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(trail_color_btn), &gui->custom_trail_color);
    gtk_widget_set_sensitive(trail_color_btn, gui->use_custom_trail_color);
    g_signal_connect(trail_color_btn, "notify::rgba", G_CALLBACK(on_trail_color_changed), gui);
    gtk_box_append(GTK_BOX(trail_color_row), trail_color_btn);
    gui->trail_color_btn = trail_color_btn;

    GtkWidget *trail_color_custom_chk = gtk_check_button_new_with_label(qirtas_tr("Custom"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(trail_color_custom_chk), gui->use_custom_trail_color);
    g_signal_connect(trail_color_custom_chk, "toggled", G_CALLBACK(on_trail_color_custom_toggled), gui);
    gtk_box_append(GTK_BOX(trail_color_row), trail_color_custom_chk);
    gui->trail_color_chk = trail_color_custom_chk;

    gtk_box_append(GTK_BOX(pop_box), trail_color_row);

    GtkWidget *pointer_color_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *pointer_color_lbl = gtk_label_new(qirtas_tr("Pointer Color"));
    gtk_widget_set_hexpand(pointer_color_lbl, TRUE);
    gtk_widget_set_halign(pointer_color_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pointer_color_row), pointer_color_lbl);

    GtkWidget *pointer_color_btn = gtk_color_dialog_button_new(color_dialog);
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(pointer_color_btn), &gui->custom_pointer_color);
    gtk_widget_set_sensitive(pointer_color_btn, gui->use_custom_pointer_color);
    g_signal_connect(pointer_color_btn, "notify::rgba", G_CALLBACK(on_pointer_color_changed), gui);
    gtk_box_append(GTK_BOX(pointer_color_row), pointer_color_btn);
    gui->pointer_color_btn = pointer_color_btn;

    GtkWidget *pointer_color_custom_chk = gtk_check_button_new_with_label(qirtas_tr("Custom"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(pointer_color_custom_chk), gui->use_custom_pointer_color);
    g_signal_connect(pointer_color_custom_chk, "toggled", G_CALLBACK(on_pointer_color_custom_toggled), gui);
    gtk_box_append(GTK_BOX(pointer_color_row), pointer_color_custom_chk);
    gui->pointer_color_chk = pointer_color_custom_chk;

    gtk_box_append(GTK_BOX(pop_box), pointer_color_row);


    gtk_box_append(GTK_BOX(pop_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    /* ── EDITOR ── */
    GtkWidget *ed_lbl = gtk_label_new(qirtas_tr("EDITOR"));
    gtk_widget_add_css_class(ed_lbl, "pop-section-label");
    gtk_widget_set_halign(ed_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pop_box), ed_lbl);

    GtkWidget *wrap_chk = gtk_check_button_new_with_label(qirtas_tr("Wrap Lines Automatically"));
    gui->wrap_chk = wrap_chk;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(wrap_chk), gui->wrap_lines);
    g_signal_connect(wrap_chk, "toggled", G_CALLBACK(on_wrap_toggled), gui);
    gtk_box_append(GTK_BOX(pop_box), wrap_chk);

    GtkWidget *ln_chk = gtk_check_button_new_with_label(qirtas_tr("Display Line Numbers"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(ln_chk), gui->show_line_numbers);
    g_signal_connect(ln_chk, "toggled", G_CALLBACK(on_line_numbers_toggled), gui);
    gtk_box_append(GTK_BOX(pop_box), ln_chk);

    GtkWidget *hl_chk = gtk_check_button_new_with_label(qirtas_tr("Highlight Current Line"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(hl_chk), gui->highlight_current_line);
    g_signal_connect(hl_chk, "toggled", G_CALLBACK(on_highlight_line_toggled), gui);
    gtk_box_append(GTK_BOX(pop_box), hl_chk);

    GtkWidget *restore_chk = gtk_check_button_new_with_label(qirtas_tr("Restore Session on Startup"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(restore_chk), gui->restore_session);
    g_signal_connect(restore_chk, "toggled", G_CALLBACK(on_restore_session_toggled), gui);
    gtk_box_append(GTK_BOX(pop_box), restore_chk);

    const char *text_width_modes[] = { "Centered (Fixed Width)", "Full Page Width", NULL };
    GtkWidget *text_width_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *text_width_lbl = gtk_label_new(qirtas_tr("Text Width"));
    gtk_widget_set_hexpand(text_width_lbl, TRUE);
    gtk_widget_set_halign(text_width_lbl, GTK_ALIGN_START);
    gui->text_width_dropdown = gtk_drop_down_new_from_strings(text_width_modes);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(gui->text_width_dropdown), gui->text_width_full_page ? 1 : 0);
    g_signal_connect(gui->text_width_dropdown, "notify::selected", G_CALLBACK(on_text_width_mode_changed), gui);
    gtk_box_append(GTK_BOX(text_width_row), text_width_lbl);
    gtk_box_append(GTK_BOX(text_width_row), gui->text_width_dropdown);
    gtk_box_append(GTK_BOX(pop_box), text_width_row);

    const char *sidebar_sides[] = { "Left", "Right", NULL };
    GtkWidget *sb_side_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *sb_side_lbl = gtk_label_new(qirtas_tr("Sidebar Side"));
    gtk_widget_set_hexpand(sb_side_lbl, TRUE);
    gtk_widget_set_halign(sb_side_lbl, GTK_ALIGN_START);
    gui->sb_side_dropdown = gtk_drop_down_new_from_strings(sidebar_sides);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(gui->sb_side_dropdown), 0); // Default Left
    g_signal_connect(gui->sb_side_dropdown, "notify::selected", G_CALLBACK(on_sidebar_side_changed), gui);
    gtk_box_append(GTK_BOX(sb_side_row), sb_side_lbl);
    gtk_box_append(GTK_BOX(sb_side_row), gui->sb_side_dropdown);
    gtk_box_append(GTK_BOX(pop_box), sb_side_row);

    // Sync & Cloud Layout
    gtk_box_append(GTK_BOX(pop_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *sync_lbl = gtk_label_new(qirtas_tr("SYNC & CLOUD"));
    gtk_widget_add_css_class(sync_lbl, "pop-section-label");
    gtk_widget_set_halign(sync_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pop_box), sync_lbl);

    // --- GOOGLE DRIVE SYNC ---
    GtkWidget *gd_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(gd_card, "sync-card");
    gtk_box_append(GTK_BOX(pop_box), gd_card);

    GtkWidget *gd_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(gd_row, "sync-card-header");
    GtkWidget *gd_lbl = gtk_label_new(qirtas_tr("Google Drive:"));
    gtk_widget_add_css_class(gd_lbl, "stats-label");
    gtk_widget_add_css_class(gd_lbl, "sync-card-title");
    gui->sync_status_lbl = gtk_label_new(qirtas_tr("Disconnected"));
    gtk_widget_add_css_class(gui->sync_status_lbl, "stats-value");
    gtk_widget_add_css_class(gui->sync_status_lbl, "sync-card-status");
    gtk_widget_set_halign(gui->sync_status_lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(gui->sync_status_lbl, TRUE);
    gui->sync_connect_btn = gtk_button_new_with_label(qirtas_tr("Connect to Google Drive"));
    gtk_widget_add_css_class(gui->sync_connect_btn, "pop-btn");
    gtk_widget_add_css_class(gui->sync_connect_btn, "sync-card-action");
    gtk_widget_set_hexpand(gui->sync_connect_btn, TRUE);
    gtk_widget_set_halign(gui->sync_connect_btn, GTK_ALIGN_FILL);
    g_signal_connect(gui->sync_connect_btn, "clicked", G_CALLBACK(on_sync_connect_clicked), gui);
    gtk_box_append(GTK_BOX(gd_row), gd_lbl);
    gtk_box_append(GTK_BOX(gd_row), gui->sync_status_lbl);
    gtk_box_append(GTK_BOX(gd_card), gd_row);
    gtk_box_append(GTK_BOX(gd_card), gui->sync_connect_btn);

    gui->sync_now_btn = gtk_button_new_with_label(qirtas_tr("Sync Now"));
    gtk_widget_add_css_class(gui->sync_now_btn, "sync-now-btn");
    gtk_widget_add_css_class(gui->sync_now_btn, "sync-card-action");
    gtk_widget_set_sensitive(gui->sync_now_btn, FALSE);
    gtk_widget_set_visible(gui->sync_now_btn, FALSE);
    gtk_widget_set_hexpand(gui->sync_now_btn, TRUE);
    gtk_widget_set_halign(gui->sync_now_btn, GTK_ALIGN_FILL);
    g_signal_connect(gui->sync_now_btn, "clicked", G_CALLBACK(on_sync_now_clicked), gui);
    gtk_box_append(GTK_BOX(gd_card), gui->sync_now_btn);

    // --- DROPBOX SYNC ---
    GtkWidget *db_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(db_card, "sync-card");
    gtk_box_append(GTK_BOX(pop_box), db_card);

    GtkWidget *db_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(db_row, "sync-card-header");
    GtkWidget *db_lbl_sec = gtk_label_new(qirtas_tr("Dropbox:"));
    gtk_widget_add_css_class(db_lbl_sec, "stats-label");
    gtk_widget_add_css_class(db_lbl_sec, "sync-card-title");
    gui->dropbox_status_lbl = gtk_label_new(qirtas_tr("Disconnected"));
    gtk_widget_add_css_class(gui->dropbox_status_lbl, "stats-value");
    gtk_widget_add_css_class(gui->dropbox_status_lbl, "sync-card-status");
    gtk_widget_set_halign(gui->dropbox_status_lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(gui->dropbox_status_lbl, TRUE);
    gui->dropbox_connect_btn = gtk_button_new_with_label(qirtas_tr("Connect to Dropbox"));
    gtk_widget_set_tooltip_text(gui->dropbox_connect_btn,
        "Conflict-safe: if a note changed on two machines, both versions are kept "
        "(the local one as <name>_conflict). See docs/SYNC.md.");
    gtk_widget_add_css_class(gui->dropbox_connect_btn, "pop-btn");
    gtk_widget_add_css_class(gui->dropbox_connect_btn, "sync-card-action");
    gtk_widget_set_hexpand(gui->dropbox_connect_btn, TRUE);
    gtk_widget_set_halign(gui->dropbox_connect_btn, GTK_ALIGN_FILL);
    g_signal_connect(gui->dropbox_connect_btn, "clicked", G_CALLBACK(on_dropbox_connect_clicked), gui);
    gtk_box_append(GTK_BOX(db_row), db_lbl_sec);
    gtk_box_append(GTK_BOX(db_row), gui->dropbox_status_lbl);
    gtk_box_append(GTK_BOX(db_card), db_row);
    gtk_box_append(GTK_BOX(db_card), gui->dropbox_connect_btn);

    gui->dropbox_now_btn = gtk_button_new_with_label(qirtas_tr("Sync Now"));
    gtk_widget_add_css_class(gui->dropbox_now_btn, "sync-now-btn");
    gtk_widget_add_css_class(gui->dropbox_now_btn, "sync-card-action");
    gtk_widget_set_sensitive(gui->dropbox_now_btn, FALSE);
    gtk_widget_set_visible(gui->dropbox_now_btn, FALSE);
    gtk_widget_set_hexpand(gui->dropbox_now_btn, TRUE);
    gtk_widget_set_halign(gui->dropbox_now_btn, GTK_ALIGN_FILL);
    g_signal_connect(gui->dropbox_now_btn, "clicked", G_CALLBACK(on_dropbox_now_clicked), gui);
    gtk_box_append(GTK_BOX(db_card), gui->dropbox_now_btn);

    // --- GITHUB SYNC ---
    GtkWidget *gh_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(gh_card, "sync-card");
    gtk_box_append(GTK_BOX(pop_box), gh_card);

    GtkWidget *gh_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(gh_row, "sync-card-header");
    GtkWidget *gh_lbl_sec = gtk_label_new(qirtas_tr("GitHub:"));
    gtk_widget_add_css_class(gh_lbl_sec, "stats-label");
    gtk_widget_add_css_class(gh_lbl_sec, "sync-card-title");
    gui->github_status_lbl = gtk_label_new(qirtas_tr("Disconnected"));
    gtk_widget_add_css_class(gui->github_status_lbl, "stats-value");
    gtk_widget_add_css_class(gui->github_status_lbl, "sync-card-status");
    gtk_widget_set_halign(gui->github_status_lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(gui->github_status_lbl, TRUE);
    gui->github_connect_btn = gtk_button_new_with_label(qirtas_tr("Connect to GitHub"));
    gtk_widget_add_css_class(gui->github_connect_btn, "pop-btn");
    gtk_widget_add_css_class(gui->github_connect_btn, "sync-card-action");
    gtk_widget_set_hexpand(gui->github_connect_btn, TRUE);
    gtk_widget_set_halign(gui->github_connect_btn, GTK_ALIGN_FILL);
    g_signal_connect(gui->github_connect_btn, "clicked", G_CALLBACK(on_github_connect_clicked), gui);
    gtk_box_append(GTK_BOX(gh_row), gh_lbl_sec);
    gtk_box_append(GTK_BOX(gh_row), gui->github_status_lbl);
    gtk_box_append(GTK_BOX(gh_card), gh_row);

    gui->github_repo_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(gui->github_repo_entry), qirtas_tr("Repo name (default: qirtas-notes)"));
    gtk_widget_set_hexpand(gui->github_repo_entry, TRUE);
    gtk_box_append(GTK_BOX(gh_card), gui->github_repo_entry);

    gtk_box_append(GTK_BOX(gh_card), gui->github_connect_btn);

    gui->github_now_btn = gtk_button_new_with_label(qirtas_tr("Sync Now"));
    gtk_widget_add_css_class(gui->github_now_btn, "sync-now-btn");
    gtk_widget_add_css_class(gui->github_now_btn, "sync-card-action");
    gtk_widget_set_sensitive(gui->github_now_btn, FALSE);
    gtk_widget_set_visible(gui->github_now_btn, FALSE);
    gtk_widget_set_hexpand(gui->github_now_btn, TRUE);
    gtk_widget_set_halign(gui->github_now_btn, GTK_ALIGN_FILL);
    g_signal_connect(gui->github_now_btn, "clicked", G_CALLBACK(on_github_now_clicked), gui);
    gtk_box_append(GTK_BOX(gh_card), gui->github_now_btn);

    // --- LOCAL / SYNCED FOLDER ---
    GtkWidget *local_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(local_card, "sync-card");
    gtk_box_append(GTK_BOX(pop_box), local_card);

    GtkWidget *local_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(local_row, "sync-card-header");
    GtkWidget *local_lbl_sec = gtk_label_new(qirtas_tr("Local / Syncthing:"));
    gtk_widget_add_css_class(local_lbl_sec, "stats-label");
    gtk_widget_add_css_class(local_lbl_sec, "sync-card-title");
    gui->local_sync_status_lbl = gtk_label_new("~/QirtasSync");
    gtk_widget_add_css_class(gui->local_sync_status_lbl, "stats-value");
    gtk_widget_add_css_class(gui->local_sync_status_lbl, "sync-card-status");
    gtk_widget_set_halign(gui->local_sync_status_lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(gui->local_sync_status_lbl, TRUE);
    gui->local_sync_btn = gtk_button_new_with_label(qirtas_tr("Sync Folder"));
    gtk_widget_set_tooltip_text(gui->local_sync_btn,
        "Conflict-safe: if a note changed on both sides, both versions are kept "
        "(the local one as <name>_conflict). See docs/SYNC.md.");
    gtk_widget_add_css_class(gui->local_sync_btn, "pop-btn");
    gtk_widget_add_css_class(gui->local_sync_btn, "sync-card-action");
    gtk_widget_set_hexpand(gui->local_sync_btn, TRUE);
    gtk_widget_set_halign(gui->local_sync_btn, GTK_ALIGN_FILL);
    g_signal_connect(gui->local_sync_btn, "clicked", G_CALLBACK(on_local_sync_clicked), gui);
    gtk_box_append(GTK_BOX(local_row), local_lbl_sec);
    gtk_box_append(GTK_BOX(local_row), gui->local_sync_status_lbl);
    gtk_box_append(GTK_BOX(local_card), local_row);
    gtk_box_append(GTK_BOX(local_card), gui->local_sync_btn);

    /* ── GENERAL ── */
    gtk_box_append(GTK_BOX(pop_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *gen_lbl = gtk_label_new(qirtas_tr("GENERAL"));
    gtk_widget_add_css_class(gen_lbl, "pop-section-label");
    gtk_widget_set_halign(gen_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pop_box), gen_lbl);

    GtkWidget *lang_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *lang_lbl = gtk_label_new(qirtas_tr("Language"));
    gtk_widget_set_hexpand(lang_lbl, TRUE);
    gtk_widget_set_halign(lang_lbl, GTK_ALIGN_START);
    const char *languages[] = { "English", "العربية", NULL };
    GtkWidget *lang_dropdown = gtk_drop_down_new_from_strings(languages);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(lang_dropdown), qirtas_app_language == 1 ? 1 : 0);
    gtk_widget_set_tooltip_text(lang_dropdown,
        qirtas_tr("Layout flips immediately; press Apply & Restart for the labels"));
    g_signal_connect(lang_dropdown, "notify::selected", G_CALLBACK(on_language_changed), gui);
    gtk_box_append(GTK_BOX(lang_row), lang_lbl);
    gtk_box_append(GTK_BOX(lang_row), lang_dropdown);
    GtkWidget *restart_btn = gtk_button_new_with_label(qirtas_tr("Apply & Restart"));
    gtk_widget_add_css_class(restart_btn, "pop-btn");
    gtk_widget_set_tooltip_text(restart_btn,
        qirtas_tr("Labels are built at startup — restart to apply the language everywhere. Your tabs are restored."));
    g_signal_connect(restart_btn, "clicked", G_CALLBACK(on_restart_clicked), gui);
    gtk_box_append(GTK_BOX(lang_row), restart_btn);
    gtk_box_append(GTK_BOX(pop_box), lang_row);

    GtkWidget *icon_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *icon_lbl = gtk_label_new(qirtas_tr("Icon Style"));
    gtk_widget_set_hexpand(icon_lbl, TRUE);
    gtk_widget_set_halign(icon_lbl, GTK_ALIGN_START);
    const char *icon_styles[] = { "Classic", "Modern", NULL };
    GtkWidget *icon_dropdown = gtk_drop_down_new_from_strings(icon_styles);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(icon_dropdown), qirtas_icon_style == 1 ? 1 : 0);
    gtk_widget_set_tooltip_text(icon_dropdown, qirtas_tr("Takes effect on next launch"));
    g_signal_connect(icon_dropdown, "notify::selected", G_CALLBACK(on_icon_style_changed), gui);
    gtk_box_append(GTK_BOX(icon_row), icon_lbl);
    gtk_box_append(GTK_BOX(icon_row), icon_dropdown);
    gtk_box_append(GTK_BOX(pop_box), icon_row);

    GtkWidget *kb_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *kb_lbl_widget = gtk_label_new(qirtas_tr("Keyboard Shortcuts"));
    gtk_widget_set_hexpand(kb_lbl_widget, TRUE);
    gtk_widget_set_halign(kb_lbl_widget, GTK_ALIGN_START);
    GtkWidget *kb_open_btn = gtk_button_new_with_label(qirtas_tr("Open Reference  (Ctrl+?)"));
    gtk_widget_add_css_class(kb_open_btn, "pop-btn");
    g_signal_connect_swapped(kb_open_btn, "clicked", G_CALLBACK(show_keybindings_window), gui);
    gtk_box_append(GTK_BOX(kb_row), kb_lbl_widget);
    gtk_box_append(GTK_BOX(kb_row), kb_open_btn);
    gtk_box_append(GTK_BOX(pop_box), kb_row);

    GtkWidget *pop_scroll = gtk_scrolled_window_new();
    gtk_widget_add_css_class(pop_scroll, "settings-sheet-scroll");
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(pop_scroll), 420);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(pop_scroll), 520);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(pop_scroll), TRUE);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(pop_scroll), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pop_scroll), pop_box);

    GtkWidget *settings_win = gtk_window_new();
    gtk_widget_add_css_class(settings_win, "settings-sheet-window");
    gtk_window_set_title(GTK_WINDOW(settings_win), qirtas_tr("Settings"));
    gtk_window_set_default_size(GTK_WINDOW(settings_win), 500, 620);
    gtk_window_set_transient_for(GTK_WINDOW(settings_win), GTK_WINDOW(window));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(settings_win), TRUE);

    /* Explicit header bar with a high-contrast title + visible close button —
     * the default CSD title/controls washed out on the light themes. */
    GtkWidget *settings_header = gtk_header_bar_new();
    gtk_widget_add_css_class(settings_header, "settings-header");
    GtkWidget *settings_title = gtk_label_new(qirtas_tr("Settings"));
    gtk_widget_add_css_class(settings_title, "settings-title");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(settings_header), settings_title);
    gtk_window_set_titlebar(GTK_WINDOW(settings_win), settings_header);

    gtk_window_set_child(GTK_WINDOW(settings_win), pop_scroll);
    g_signal_connect(settings_win, "close-request", G_CALLBACK(on_settings_window_close_request), NULL);

    gui->settings_window = settings_win;

    /* ============================================================
     * WIRE ALL SIGNALS
     * ============================================================ */
    if (gui->btn_editor) g_signal_connect(gui->btn_editor,   "clicked", G_CALLBACK(on_editor_clicked), gui);
    if (gui->btn_files)    g_signal_connect(gui->btn_files,    "clicked", G_CALLBACK(on_files_clicked),  gui);

    // Track text changes to update word/character counts
    g_signal_connect(src_buf, "changed", G_CALLBACK(on_buffer_changed), gui);
    g_signal_connect(src_buf, "modified-changed", G_CALLBACK(on_buffer_modified_changed), gui);

    // Setup right-click popover gesture on editor
    GtkGesture *right_click_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click_gesture), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(right_click_gesture), GTK_PHASE_CAPTURE);
    g_signal_connect(right_click_gesture, "pressed", G_CALLBACK(on_editor_right_click), gui);
    gtk_widget_add_controller(source_view, GTK_EVENT_CONTROLLER(right_click_gesture));

    // Setup left-click wiki-link gesture on editor
    GtkGesture *left_click_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(left_click_gesture), GDK_BUTTON_PRIMARY);
    g_signal_connect(left_click_gesture, "pressed", G_CALLBACK(on_editor_left_click), gui);
    gtk_widget_add_controller(source_view, GTK_EVENT_CONTROLLER(left_click_gesture));

    GtkEventController *editor_motion_ctrl = gtk_event_controller_motion_new();
    g_signal_connect(editor_motion_ctrl, "motion", G_CALLBACK(on_editor_motion), gui);
    gtk_widget_add_controller(source_view, editor_motion_ctrl);

    // Setup mouse event controller to clamp cursor and scrollbar past document end
    GtkEventController *editor_mouse_ctrl = gtk_event_controller_legacy_new();
    gtk_event_controller_set_propagation_phase(editor_mouse_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(editor_mouse_ctrl, "event", G_CALLBACK(on_editor_mouse_event), gui);
    gtk_widget_add_controller(source_view, editor_mouse_ctrl);

    /* ============================================================
     * BOTTOM BAR
     * ============================================================ */
    GtkWidget *bottom_bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(bottom_bar, "bottom-bar");

    GtkWidget *status_info_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(status_info_row, "status-info-row");
    gtk_widget_set_hexpand(status_info_row, TRUE);
    gtk_widget_set_halign(status_info_row, GTK_ALIGN_FILL);

    GtkWidget *bottom_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(bottom_spacer, TRUE);


    /* ── Far bottom-left: sidebar toggle icon button ── */
    gui->btn_sidebar_toggle = gtk_button_new();
    gtk_accessible_update_property(GTK_ACCESSIBLE(gui->btn_sidebar_toggle),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Toggle sidebar", -1);
    gtk_button_set_child(GTK_BUTTON(gui->btn_sidebar_toggle),
        gtk_image_new_from_icon_name(qirtas_icon("sidebar")));
    gtk_widget_add_css_class(gui->btn_sidebar_toggle, "bottom-icon-btn");
    gtk_widget_set_tooltip_text(gui->btn_sidebar_toggle, qirtas_tr("Toggle Sidebar (Ctrl+\\)"));
    g_signal_connect(gui->btn_sidebar_toggle, "clicked",
                     G_CALLBACK(on_logo_clicked), gui);

    gui->path_label = gtk_label_new("Economics_Notes.md");
    gtk_widget_add_css_class(gui->path_label, "bottom-path");



    /* ── Bottom-right: Read mode toggle icon button ── */
    gui->btn_read_mode = gtk_button_new();
    gtk_accessible_update_property(GTK_ACCESSIBLE(gui->btn_read_mode),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Toggle read mode", -1);
    gtk_button_set_child(GTK_BUTTON(gui->btn_read_mode),
        gtk_image_new_from_icon_name(qirtas_icon("readmode")));
    gtk_widget_add_css_class(gui->btn_read_mode, "bottom-icon-btn");
    gtk_widget_set_tooltip_text(gui->btn_read_mode, qirtas_tr("Toggle read mode (Ctrl+E)"));
    g_signal_connect(gui->btn_read_mode, "clicked", G_CALLBACK(on_read_mode_toggle_clicked), gui);

    /* ── Bottom-right: Search icon button ── */
    gui->btn_search = gtk_button_new();
    gtk_accessible_update_property(GTK_ACCESSIBLE(gui->btn_search),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Search in file", -1);
    gtk_button_set_child(GTK_BUTTON(gui->btn_search),
        gtk_image_new_from_icon_name(qirtas_icon("search")));
    gtk_widget_add_css_class(gui->btn_search, "bottom-icon-btn");
    gtk_widget_set_tooltip_text(gui->btn_search, qirtas_tr("Search in file (Ctrl+F)"));
    g_signal_connect(gui->btn_search, "clicked", G_CALLBACK(on_search_icon_clicked), gui);

    /* ── Bottom-right: Menu icon button for quick file actions ── */
    gui->btn_status_actions = gtk_menu_button_new();
    gtk_accessible_update_property(GTK_ACCESSIBLE(gui->btn_status_actions),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Quick file actions", -1);
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(gui->btn_status_actions), qirtas_icon("menu"));
    gtk_widget_add_css_class(gui->btn_status_actions, "bottom-icon-btn");
    gtk_widget_set_tooltip_text(gui->btn_status_actions, qirtas_tr("Menu"));

    GtkWidget *actions_popover = gtk_popover_new();
    GtkWidget *actions_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(actions_box, 6);
    gtk_widget_set_margin_end(actions_box, 6);
    gtk_widget_set_margin_top(actions_box, 6);
    gtk_widget_set_margin_bottom(actions_box, 6);

    // 1. Add New File button
    GtkWidget *btn_pop_new = gtk_button_new();
    gtk_widget_add_css_class(btn_pop_new, "pop-btn");
    GtkWidget *hbox_new = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(hbox_new, GTK_ALIGN_START);
    GtkWidget *img_new = gtk_image_new_from_icon_name(qirtas_icon("new"));
    GtkWidget *lbl_new = gtk_label_new(qirtas_tr("Add New File"));
    gtk_box_append(GTK_BOX(hbox_new), img_new);
    gtk_box_append(GTK_BOX(hbox_new), lbl_new);
    gtk_button_set_child(GTK_BUTTON(btn_pop_new), hbox_new);
    g_signal_connect(btn_pop_new, "clicked", G_CALLBACK(on_status_bar_new_file_clicked), gui);
    gtk_box_append(GTK_BOX(actions_box), btn_pop_new);

    // 2. Open File button
    GtkWidget *btn_pop_open = gtk_button_new();
    gtk_widget_add_css_class(btn_pop_open, "pop-btn");
    GtkWidget *hbox_open = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(hbox_open, GTK_ALIGN_START);
    GtkWidget *img_open = gtk_image_new_from_icon_name(qirtas_icon("open"));
    GtkWidget *lbl_open = gtk_label_new(qirtas_tr("Open File"));
    gtk_box_append(GTK_BOX(hbox_open), img_open);
    gtk_box_append(GTK_BOX(hbox_open), lbl_open);
    gtk_button_set_child(GTK_BUTTON(btn_pop_open), hbox_open);
    g_signal_connect(btn_pop_open, "clicked", G_CALLBACK(on_status_bar_open_file_clicked), gui);
    gtk_box_append(GTK_BOX(actions_box), btn_pop_open);

    // 3. Save File button
    GtkWidget *btn_pop_save = gtk_button_new();
    gtk_widget_add_css_class(btn_pop_save, "pop-btn");
    GtkWidget *hbox_save = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(hbox_save, GTK_ALIGN_START);
    GtkWidget *img_save = gtk_image_new_from_icon_name(qirtas_icon("save"));
    GtkWidget *lbl_save = gtk_label_new(qirtas_tr("Save File"));
    gtk_box_append(GTK_BOX(hbox_save), img_save);
    gtk_box_append(GTK_BOX(hbox_save), lbl_save);
    gtk_button_set_child(GTK_BUTTON(btn_pop_save), hbox_save);
    g_signal_connect(btn_pop_save, "clicked", G_CALLBACK(on_status_bar_save_file_clicked), gui);
    gtk_box_append(GTK_BOX(actions_box), btn_pop_save);

    // 4. Export as PDF button
    GtkWidget *btn_pop_pdf = gtk_button_new();
    gtk_widget_add_css_class(btn_pop_pdf, "pop-btn");
    GtkWidget *hbox_pdf = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(hbox_pdf, GTK_ALIGN_START);
    GtkWidget *img_pdf = gtk_image_new_from_icon_name(qirtas_icon("pdf"));
    GtkWidget *lbl_pdf = gtk_label_new(qirtas_tr("Export as PDF"));
    gtk_box_append(GTK_BOX(hbox_pdf), img_pdf);
    gtk_box_append(GTK_BOX(hbox_pdf), lbl_pdf);
    gtk_button_set_child(GTK_BUTTON(btn_pop_pdf), hbox_pdf);
    g_signal_connect(btn_pop_pdf, "clicked", G_CALLBACK(on_status_bar_export_pdf_clicked), gui);
    gtk_box_append(GTK_BOX(actions_box), btn_pop_pdf);

    gtk_box_append(GTK_BOX(actions_box),
        status_menu_item("edit-copy-symbolic", qirtas_tr("Copy File"), NULL,
                         G_CALLBACK(on_status_menu_copy_file), gui));

    gtk_box_append(GTK_BOX(actions_box),
        status_menu_item(qirtas_icon("saveas"), qirtas_tr("Save As…"), "Ctrl+Shift+S",
                         G_CALLBACK(on_status_menu_save_as), gui));

    gtk_box_append(GTK_BOX(actions_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gtk_box_append(GTK_BOX(actions_box),
        status_menu_item(qirtas_icon("findreplace"), qirtas_tr("Find / Replace…"), "Ctrl+F",
                         G_CALLBACK(on_status_menu_find_replace), gui));
    gtk_box_append(GTK_BOX(actions_box),
        status_menu_item(qirtas_icon("fullscreen"), qirtas_tr("Fullscreen"), "F11",
                         G_CALLBACK(on_status_menu_fullscreen), gui));

    gtk_box_append(GTK_BOX(actions_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gtk_box_append(GTK_BOX(actions_box),
        status_menu_item(qirtas_icon("prefs"), qirtas_tr("Preferences"), "Ctrl+,",
                         G_CALLBACK(on_status_menu_settings), gui));
    gtk_box_append(GTK_BOX(actions_box),
        status_menu_item(qirtas_icon("keyboard"), qirtas_tr("Keyboard Shortcuts"), "Ctrl+?",
                         G_CALLBACK(on_status_menu_shortcuts), gui));

    gtk_box_append(GTK_BOX(actions_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gtk_box_append(GTK_BOX(actions_box),
        status_menu_item(qirtas_icon("quit"), qirtas_tr("Quit Qirtas"), "Ctrl+Q",
                         G_CALLBACK(on_status_menu_quit), gui));

    gtk_popover_set_child(GTK_POPOVER(actions_popover), actions_box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(gui->btn_status_actions), actions_popover);

    gui->lbl_words = gtk_label_new(qirtas_tr("0 words"));
    gtk_widget_add_css_class(gui->lbl_words, "bottom-bar-lbl");

    gui->lbl_chars = gtk_label_new(qirtas_tr("0 chars"));
    gtk_widget_add_css_class(gui->lbl_chars, "bottom-bar-lbl");

    gui->lbl_lines = gtk_label_new(qirtas_tr("0 lines"));
    gtk_widget_add_css_class(gui->lbl_lines, "bottom-bar-lbl");

    /* ── Far bottom-right: sync status dot button ── */
    gui->btn_sync_icon_bottom = gtk_button_new();
    gtk_accessible_update_property(GTK_ACCESSIBLE(gui->btn_sync_icon_bottom),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Sync document", -1);
    gui->sync_dot = gtk_label_new("●");
    gtk_widget_add_css_class(gui->sync_dot, "status-dot");
    gtk_widget_add_css_class(gui->sync_dot, "status-saved");
    gtk_button_set_child(GTK_BUTTON(gui->btn_sync_icon_bottom), gui->sync_dot);
    gtk_widget_add_css_class(gui->btn_sync_icon_bottom, "bottom-icon-btn");
    gtk_widget_set_tooltip_text(gui->btn_sync_icon_bottom, qirtas_tr("Sync Now"));
    g_signal_connect(gui->btn_sync_icon_bottom, "clicked",
                     G_CALLBACK(on_sync_now_clicked), gui);

    GtkWidget *tab_strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(tab_strip, "tab-strip");
    gtk_widget_set_hexpand(tab_strip, TRUE);
    gtk_widget_set_halign(tab_strip, GTK_ALIGN_FILL);
    gui->tab_strip = tab_strip;
    /* Tab strip stays LTR even when the app runs RTL (Arabic). */
    gtk_widget_set_direction(tab_strip, GTK_TEXT_DIR_LTR);

    gui->btn_tab_scroll_left = gtk_button_new_from_icon_name("pan-start-symbolic");
    gtk_accessible_update_property(GTK_ACCESSIBLE(gui->btn_tab_scroll_left),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Scroll tabs left", -1);
    gtk_widget_add_css_class(gui->btn_tab_scroll_left, "tab-scroll-btn");
    gtk_widget_set_tooltip_text(gui->btn_tab_scroll_left, qirtas_tr("Scroll tabs left"));

    gui->tab_bar_scroll = gtk_scrolled_window_new();
    gtk_widget_add_css_class(gui->tab_bar_scroll, "tab-bar-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(gui->tab_bar_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(gui->tab_bar_scroll), TRUE);
    gtk_widget_set_hexpand(gui->tab_bar_scroll, TRUE);
    gtk_widget_set_hexpand_set(gui->tab_bar_scroll, TRUE);

    GtkWidget *tab_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(tab_bar, "tab-bar");
    gtk_widget_set_valign(tab_bar, GTK_ALIGN_CENTER);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(gui->tab_bar_scroll), tab_bar);
    gui->tab_bar_box = tab_bar;

    gui->btn_tab_scroll_right = gtk_button_new_from_icon_name("pan-end-symbolic");
    gtk_accessible_update_property(GTK_ACCESSIBLE(gui->btn_tab_scroll_right),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Scroll tabs right", -1);
    gtk_widget_add_css_class(gui->btn_tab_scroll_right, "tab-scroll-btn");
    gtk_widget_set_tooltip_text(gui->btn_tab_scroll_right, qirtas_tr("Scroll tabs right"));

    gtk_box_append(GTK_BOX(tab_strip), gui->btn_tab_scroll_left);
    gtk_box_append(GTK_BOX(tab_strip), gui->tab_bar_scroll);
    gtk_box_append(GTK_BOX(tab_strip), gui->btn_tab_scroll_right);
    gui_tabs_setup_viewport(gui);

    /* The 🔍 search, 📖 read mode, and ⋮ menu live in the card header now.
     * Reparent the real widgets (keeps their wiring + the actions popover
     * intact). */
    gtk_widget_add_css_class(gui->btn_search, "editor-header-btn");
    gtk_widget_add_css_class(gui->btn_read_mode, "editor-header-btn");
    gtk_widget_add_css_class(gui->btn_status_actions, "editor-header-btn");
    if (gui->editor_header) {
        gtk_box_append(GTK_BOX(gui->editor_header), gui->btn_read_mode);
        gtk_box_append(GTK_BOX(gui->editor_header), gui->btn_search);
        gtk_box_append(GTK_BOX(gui->editor_header), gui->btn_status_actions);
        /* Order the cluster as [📖][🔍][≡ outline][⋮]: read mode first, menu last. */
        gtk_box_reorder_child_after(GTK_BOX(gui->editor_header), gui->btn_read_mode, gui->breadcrumb_label);
        gtk_box_reorder_child_after(GTK_BOX(gui->editor_header), gui->btn_search, gui->btn_read_mode);
    }

    /* The status row is now a small pill that FLOATS on the paper card,
     * pinned to the bottom-end corner: bottom-left under Arabic (RTL end),
     * bottom-right under English (LTR end). Only sync dot + word + char
     * counts remain. Sidebar toggle drops to Ctrl+\\; the path lives in the
     * card-header breadcrumb. */
    (void)bottom_spacer;
    g_object_ref_sink(gui->btn_sidebar_toggle); /* kept alive, not shown */
    g_object_ref_sink(gui->path_label);         /* used by title/search code */

    gtk_widget_add_css_class(status_info_row, "status-pill");
    gtk_box_append(GTK_BOX(status_info_row), gui->btn_sync_icon_bottom);
    gtk_box_append(GTK_BOX(status_info_row), gui->lbl_words);
    gtk_box_append(GTK_BOX(status_info_row), gui->lbl_chars);
    gtk_box_append(GTK_BOX(status_info_row), gui->lbl_lines);

    gtk_widget_set_halign(status_info_row, GTK_ALIGN_END);
    gtk_widget_set_valign(status_info_row, GTK_ALIGN_END);
    gtk_widget_set_margin_end(status_info_row, 16);
    gtk_widget_set_margin_bottom(status_info_row, 14);
    gtk_overlay_add_overlay(GTK_OVERLAY(editor_overlay), status_info_row);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(editor_overlay), status_info_row, FALSE);

    /* bottom_bar is retained as an empty 0-height tray so the existing
     * layout/focus reordering code keeps a valid widget to move; the visible
     * status now lives in the floating pill above. */
    gui->bottom_bar_widget = bottom_bar;
    gtk_widget_set_direction(bottom_bar, GTK_TEXT_DIR_LTR);

    /* Redesign layout: tab strip pinned to the very top, then the
     * sidebar+desk paned, then the (now empty) status tray. */
    gtk_box_append(GTK_BOX(main_vertical_box), tab_strip);
    gtk_box_append(GTK_BOX(main_vertical_box), sidebar_editor_box);
    gtk_box_append(GTK_BOX(main_vertical_box), bottom_bar);

    reorder_main_layout(gui);

    /* ============================================================
     * GLOBALS + PRESENT
     * ============================================================ */
    global_gui         = gui;
    global_window      = window;
    global_source_view = source_view;
    gui->status_pill   = bottom_bar;
    global_sync_label  = gui->sync_dot;
    global_path_label  = gui->path_label;

    g_object_set_data_full(G_OBJECT(window), "app-gui", gui, g_free);

    populate_explorer(gui);
    apply_theme(gui, current_theme);
    apply_focus_mode(gui);
    gtk_window_present(GTK_WINDOW(window));
    zig_on_gui_ready();

    /* Restore Session: reopen every tab from last run, active file last */
    if (gui->restore_session) {
        extern void zig_open_file(const char *filename);
        char *last = qirtas_pref_get_string("last_file");
        char *tabs = qirtas_pref_get_string("session_tabs");
        if (tabs && tabs[0] != '\0') {
            gchar **paths = g_strsplit(tabs, "\n", -1);
            for (int i = 0; paths[i]; i++) {
                if (paths[i][0] == '\0') continue;
                if (last && strcmp(paths[i], last) == 0) continue; /* opened last */
                if (g_file_test(paths[i], G_FILE_TEST_EXISTS)) zig_open_file(paths[i]);
            }
            g_strfreev(paths);
        }
        if (last && last[0] != '\0' && g_file_test(last, G_FILE_TEST_EXISTS)) {
            zig_open_file(last);
        }
        g_free(last);
        g_free(tabs);
    }

    apply_compact_mode(gui);

    load_sync_credentials(gui);
    int connected = zig_sync_check_status();
    gui_update_sync_status(connected, connected ? "Connected" : "Disconnected");

    int db_connected = zig_dropbox_check_status();
    gui_update_dropbox_status(db_connected, db_connected ? "Connected" : "Disconnected");

    int gh_connected = zig_github_check_status();
    gui_update_github_status(gh_connected, gh_connected ? "Connected" : "Disconnected");

    if (gui->github_repo_entry) {
        char gh_tok_buf[512];
        char gh_repo_buf[256];
        if (zig_get_github_credentials_decrypted(gh_tok_buf, sizeof(gh_tok_buf), gh_repo_buf, sizeof(gh_repo_buf))) {
            gtk_editable_set_text(GTK_EDITABLE(gui->github_repo_entry), gh_repo_buf);
        }
    }

}


/* ============================================================
 * APPLICATION ENTRY POINT
 * ============================================================ */

int run_gui(int argc, char **argv) {
    gtk_source_init();
    adw_init();
    AdwApplication *app = adw_application_new("org.qirtas.notebook",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    gtk_source_finalize();
    return status;
}

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

/* ============================================================
 * FFI — CALLED FROM ZIG (signatures MUST stay identical)
 * ============================================================ */

char *gui_get_text(void) {
    if (!global_source_view) return NULL;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    char *raw = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    if (!raw) return NULL;
    char *clean = replace_anchors_with_hrs(raw);
    g_free(raw);
    return clean;
}

void gui_free_text(char *text) { g_free(text); }

void gui_set_text(const char *text, int len) {
    if (!global_source_view || !global_gui) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));
    
    g_signal_handlers_block_by_func(buf, on_insert_text_before, global_gui);
    g_signal_handlers_block_by_func(buf, on_insert_text_after, global_gui);
    g_signal_handlers_block_by_func(buf, on_delete_range_after, global_gui);
    g_signal_handlers_block_by_func(buf, on_buffer_changed, global_gui);
    
    gtk_text_buffer_set_text(buf, text, len);
    reset_cursor_trail(global_gui);
    
    parse_and_render_hrs(buf, global_gui);
    
    g_signal_handlers_unblock_by_func(buf, on_insert_text_before, global_gui);
    g_signal_handlers_unblock_by_func(buf, on_insert_text_after, global_gui);
    g_signal_handlers_unblock_by_func(buf, on_delete_range_after, global_gui);
    g_signal_handlers_unblock_by_func(buf, on_buffer_changed, global_gui);
    
    update_all_paragraphs_direction(buf);
    apply_wiki_link_tags(buf);
}

void gui_get_cursor_position(int *line, int *col) {
    if (!global_source_view) {
        *line = 1;
        *col = 0;
        return;
    }
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));
    GtkTextIter iter;
    GtkTextMark *mark = gtk_text_buffer_get_insert(buf);
    gtk_text_buffer_get_iter_at_mark(buf, &iter, mark);
    
    int rel_line = gtk_text_iter_get_line(&iter);
    *line = rel_line + 1;
    *col = gtk_text_iter_get_line_offset(&iter);
}

void gui_set_cursor_position(int line, int col) {
    if (!global_source_view || !global_gui) return;
    
    int target_abs_line = line - 1; // 0-indexed
    if (target_abs_line < 0) target_abs_line = 0;

    /* Full-buffer model: the whole document lives in the buffer, so the
     * absolute line is the buffer line directly (no active-page offset). */
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));
    GtkTextIter iter;
    int rel_line = target_abs_line;
    if (rel_line < 0) rel_line = 0;

    int total_lines = gtk_text_buffer_get_line_count(buf);
    if (rel_line >= total_lines) {
        rel_line = total_lines - 1;
    }
    if (rel_line < 0) rel_line = 0;

    gtk_text_buffer_get_iter_at_line(buf, &iter, rel_line);
    for (int i = 0; i < col; i++) {
        if (gtk_text_iter_ends_line(&iter) || gtk_text_iter_is_end(&iter)) {
            break;
        }
        gtk_text_iter_forward_char(&iter);
    }
    gtk_text_buffer_select_range(buf, &iter, &iter);
    reset_cursor_trail(global_gui);
}

int gui_get_absolute_cursor_line(void) {
    if (!global_gui || !global_source_view) return 1;
    int line = 1, col = 0;
    gui_get_cursor_position(&line, &col);
    return line;
}

void gui_trigger_autosave(void) {
    if (!global_gui || !global_source_view) return;
    
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));
    gui_set_sync_status("Saving...");
    
    GtkTextIter start_iter, end_iter;
    gtk_text_buffer_get_bounds(buf, &start_iter, &end_iter);
    char *page_text = gtk_text_buffer_get_text(buf, &start_iter, &end_iter, FALSE);

    /* Full-buffer model: the buffer holds the whole document, so save the
     * full line range [0, line_count) rather than an active page window. */
    extern int zig_save_active_page(int start_line, int end_line, const char *text);
    int status = zig_save_active_page(0, gtk_text_buffer_get_line_count(buf), page_text);
    g_free(page_text);
    
    if (status == 0) {
        gui_set_sync_status("Saved");
    } else {
        gui_set_sync_status("Save Failed");
    }
}

static void refresh_explorer_idle_cb(void *user_data) {
    (void)user_data;
    if (global_gui && global_gui->explorer_listbox && GTK_IS_BOX(global_gui->explorer_listbox)) {
        populate_explorer(global_gui);
    }
}

void gui_refresh_explorer(void) {
    gui_run_on_main_thread(refresh_explorer_idle_cb, NULL);
}

void gui_set_title(const char *title) {
    if (global_window)
        gtk_window_set_title(GTK_WINDOW(global_window), title);
    if (global_path_label) {
        char fname[256];
        strncpy(fname, title, sizeof(fname));
        fname[sizeof(fname) - 1] = '\0';
        char *sfx = strstr(fname, " - Qirtas");
        if (sfx) *sfx = '\0';
        gtk_label_set_text(GTK_LABEL(global_path_label), fname);
        if (global_gui && global_gui->stats_file_val)
            gtk_label_set_text(GTK_LABEL(global_gui->stats_file_val), fname);
    }
}

void gui_set_sync_status(const char *status) {
    if (!global_sync_label) return;
    
    const char *display_status = status;
    if (strcmp(status, "Saved") == 0 || strcmp(status, "Synced") == 0 || strcmp(status, "Updated") == 0) {
        display_status = "Synced";
    } else if (strcmp(status, "Saving...") == 0) {
        display_status = "Saving...";
    } else {
        display_status = "Not Synced";
    }

    gtk_widget_remove_css_class(global_sync_label, "status-saved");
    gtk_widget_remove_css_class(global_sync_label, "status-saving");
    gtk_widget_remove_css_class(global_sync_label, "status-failed");

    if (strcmp(display_status, "Synced") == 0)
        gtk_widget_add_css_class(global_sync_label, "status-saved");
    else if (strcmp(display_status, "Saving...") == 0)
        gtk_widget_add_css_class(global_sync_label, "status-saving");
    else
        gtk_widget_add_css_class(global_sync_label, "status-failed");
}

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

void gui_show_editor(void) {
    if (global_gui && global_gui->stack && global_gui->btn_editor)
        set_active_tab(global_gui, global_gui->btn_editor, "editor");
}

typedef struct { GuiIdleCallback cb; void *data; } IdleData;

static gboolean idle_wrapper(gpointer d) {
    IdleData *id = (IdleData *)d;
    id->cb(id->data);
    g_free(id);
    return G_SOURCE_REMOVE;
}

void gui_run_on_main_thread(GuiIdleCallback callback, void *user_data) {
    IdleData *id  = g_new(IdleData, 1);
    id->cb   = callback;
    id->data = user_data;
    g_idle_add(idle_wrapper, id);
}