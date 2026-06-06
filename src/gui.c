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

#define DB_PATH "/home/.config/qirtas/vault.db"

/* ============================================================
 * DATA STRUCTURES
 * ============================================================ */

typedef struct {
    GtkWidget *window;

    /* Layout roots */
    GtkWidget *root_box;
    GtkWidget *sidebar;
    GtkWidget *stack;

    /* Sidebar buttons */
    GtkWidget *btn_editor;
    GtkWidget *btn_files;
    GtkWidget *btn_stats;
    GtkWidget *btn_search;
    GtkWidget *btn_settings;

    /* Editor page */
    GtkWidget *editor_page;
    GtkWidget *time_pill;
    GtkWidget *path_label;
    GtkWidget *status_pill; // Can be kept or unused, but we'll use sync_dot instead
    GtkWidget *lbl_words;
    GtkWidget *lbl_chars;
    GtkWidget *sync_dot;

    /* Search */
    GtkWidget *search_revealer;
    GtkWidget *search_entry;
    GtkWidget *search_match_label;
    GtkSourceSearchContext  *search_ctx;
    GtkSourceSearchSettings *search_settings;
    gboolean   search_visible;

    /* Source view */
    GtkWidget *source_view;
    GtkWidget *active_popover;

    /* Explorer */
    GtkWidget *explorer_listbox;
    GtkWidget *exp_count_label;
    GtkWidget *exp_search_entry;

    /* Stats */
    GtkWidget *stats_time_val;
    GtkWidget *stats_file_val;

    /* Font size provider */
    GtkCssProvider *font_provider;
    GtkCssProvider *css_provider;

    /* Sync Engine UI components */
    GtkWidget *sync_status_lbl;
    GtkWidget *sync_connect_btn;
    GtkWidget *sync_now_btn;
    GtkWidget *sync_code_entry;
    GtkWidget *sync_submit_btn;
    GtkWidget *sync_code_box;
    GtkWidget *client_id_entry;
    GtkWidget *client_secret_entry;

    GtkWidget *settings_window;

    /* Bottom-bar quick-access buttons */
    GtkWidget *btn_sidebar_toggle;  /* far bottom-left  — opens/closes sidebar */
    GtkWidget *btn_sync_icon_bottom; /* far bottom-right — triggers sync now   */

    /* Virtual Document Layout */
    GtkWidget *virtual_layout_box;
    GtkWidget *top_spacer;
    GtkWidget *bottom_spacer;
    GtkAdjustment *vadjustment;
    gboolean in_scroll_update;
    double last_v_offset;
    int total_virtual_lines;
    int active_page_start_line;
    int active_page_end_line;
    int estimated_line_height;
} AppGui;

/* ============================================================
 * GLOBALS
 * ============================================================ */

static AppGui    *global_gui         = NULL;
static GtkWidget *global_window      = NULL;
static GtkWidget *global_source_view = NULL;
static GtkWidget *global_sync_label  = NULL;
static GtkWidget *global_path_label  = NULL;
static GtkWidget *global_time_label  = NULL;

static int seconds_elapsed = 0;

extern void zig_on_gui_ready(void);
extern void zig_open_file(const char *filename);
extern void zig_search_workspace(const char *query);
extern const char *zig_get_search_snippet(const char *filepath);
extern int zig_get_search_rank(const char *filepath);

/* ============================================================
 * FORWARD DECLARATIONS
 * ============================================================ */

static void populate_explorer(AppGui *gui);
static void set_active_tab(AppGui *gui, GtkWidget *active_btn, const char *page);
static void toggle_search(AppGui *gui);
static void on_insert_text_before(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data);
static gboolean on_editor_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
static void on_editor_right_click(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data);
static void apply_regex_conceal(GtkTextBuffer *buf, const gchar *text, const gchar *pattern, gint cursor_char, gint delim_len, GtkTextTag *conceal_tag);
static void update_conceal_markdown(GtkTextBuffer *buf);
static void on_buffer_changed(GtkTextBuffer *buf, gpointer user_data);
static void on_mark_set(GtkTextBuffer *buf, GtkTextIter *location, GtkTextMark *mark, gpointer user_data);
static void on_cursor_position_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
static void on_format_clicked(GtkButton *btn, gpointer user_data);
static void on_para_clicked(GtkButton *btn, gpointer user_data);
static void apply_wiki_link_tags(GtkTextBuffer *buf);
static void on_editor_left_click(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data);
static void show_keybindings_window(AppGui *gui);
static void on_settings_btn_clicked(GtkButton *btn, gpointer user_data);
static gboolean on_settings_window_close_request(GtkWindow *window, gpointer user_data);
static void on_export_pdf_clicked(GtkButton *btn, gpointer user_data);
void qirtas_export_to_pdf(AppGui *gui);
static void update_editor_font(AppGui *gui);
static int get_line_height(GtkWidget *text_view);



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

/* ============================================================
 * CSS — Deep Slate Dark Theme
 * NOTE: GTK4 CSS does NOT support:
 *   - !important
 *   - max-width / max-height
 *   - outline
 * ============================================================ */

static const char *CSS_DEEP_SLATE = 
    "* { font-family: 'JetBrains Mono', 'Fira Mono', 'DejaVu Sans Mono', monospace; }\n"
    "window { background-color: #111318; }\n"
    ".sidebar {\n"
    "  background-color: #0c0e13;\n"
    "  border-right: 1px solid #1e2025;\n"
    "  min-width: 280px;\n"
    "  padding: 16px 12px;\n"
    "}\n"
    ".logo-btn {\n"
    "  background-color: #282a2f;\n"
    "  color: #ffafd7;\n"
    "  font-weight: 700;\n"
    "  font-size: 14px;\n"
    "  border-radius: 8px;\n"
    "  padding: 4px 12px;\n"
    "  margin: 0 4px;\n"
    "}\n"
    ".nav-btn {\n"
    "  background: transparent;\n"
    "  border: none;\n"
    "  box-shadow: none;\n"
    "  border-radius: 10px;\n"
    "  padding: 10px;\n"
    "  margin: 2px 8px;\n"
    "  color: #6b6e7a;\n"
    "  transition: background 180ms, color 180ms;\n"
    "}\n"
    ".nav-btn:hover {\n"
    "  background-color: #1e2025;\n"
    "  color: #dac0ca;\n"
    "}\n"
    ".nav-btn image {\n"
    "  -gtk-icon-size: 18px;\n"
    "  color: inherit;\n"
    "}\n"
    ".nav-btn.active {\n"
    "  background-color: #282a2f;\n"
    "  color: #ffafd7;\n"
    "  border-left: 2px solid #ff79c6;\n"
    "}\n"
    ".workspace { background-color: #111318; }\n"
    ".pill {\n"
    "  font-size: 11px;\n"
    "  font-weight: 500;\n"
    "  padding: 4px 14px;\n"
    "  border-radius: 9999px;\n"
    "  background-color: #1e2025;\n"
    "  color: #dac0ca;\n"
    "}\n"
    ".pill-time { color: #75d4e8; }\n"
    ".status-saved  { color: #31e368; }\n"
    ".status-saving { color: #75d4e8; }\n"
    ".status-failed { color: #ffb4ab; }\n"
    ".path-label {\n"
    "  color: #6b6e7a;\n"
    "  font-size: 12px;\n"
    "}\n"
    ".search-bar-box {\n"
    "  background-color: #0c0e13;\n"
    "  border-top: 1px solid #1e2025;\n"
    "  padding: 6px 16px;\n"
    "}\n"
    "entry.search-entry {\n"
    "  background-color: #1a1c21;\n"
    "  color: #e2e2e9;\n"
    "  border: 1px solid #282a2f;\n"
    "  border-radius: 8px;\n"
    "  padding: 4px 12px;\n"
    "  font-size: 13px;\n"
    "  min-width: 260px;\n"
    "  box-shadow: none;\n"
    "}\n"
    "entry.search-entry:focus {\n"
    "  border-color: #ff79c6;\n"
    "  box-shadow: none;\n"
    "}\n"
    ".search-match-label {\n"
    "  color: #6b6e7a;\n"
    "  font-size: 11px;\n"
    "  margin: 0 8px;\n"
    "}\n"
    ".search-nav-btn {\n"
    "  background: transparent;\n"
    "  border: none;\n"
    "  box-shadow: none;\n"
    "  border-radius: 6px;\n"
    "  padding: 4px 8px;\n"
    "  color: #dac0ca;\n"
    "}\n"
    ".search-nav-btn:hover { background-color: #1e2025; color: #ffafd7; }\n"
    "textview, textview text, textview.source-view {\n"
    "  background-color: #111318;\n"
    "  color: #ffffff;\n"
    "  caret-color: #ff79c6;\n"
    "}\n"
    "textview text:selected {\n"
    "  background-color: #2e3038;\n"
    "  color: #ffafd7;\n"
    "}\n"
    "scrolledwindow {\n"
    "  border: none;\n"
    "  box-shadow: none;\n"
    "  background-color: #111318;\n"
    "}\n"
    ".editor-scroll {\n"
    "  border: 1px solid #1e2025;\n"
    "  border-radius: 8px;\n"
    "}\n"
    ".explorer-header { padding: 24px 28px 12px 28px; }\n"
    ".explorer-title {\n"
    "  font-size: 15px;\n"
    "  font-weight: 700;\n"
    "  color: #ffafd7;\n"
    "}\n"
    ".explorer-badge {\n"
    "  font-size: 10px;\n"
    "  font-weight: 500;\n"
    "  padding: 2px 10px;\n"
    "  border-radius: 9999px;\n"
    "  background-color: #1e2025;\n"
    "  color: #6b6e7a;\n"
    "}\n"
    ".explorer-card {\n"
    "  background-color: #1a1c21;\n"
    "  border: 1px solid #282a2f;\n"
    "  border-radius: 10px;\n"
    "  padding: 14px 16px;\n"
    "  min-width: 180px;\n"
    "  transition: background 150ms, border-color 150ms;\n"
    "}\n"
    ".explorer-card:hover {\n"
    "  background-color: #20232a;\n"
    "  border-color: #ff79c6;\n"
    "}\n"
    ".icon-folder  { color: #75d4e8; }\n"
    ".icon-file-md { color: #ff79c6; }\n"
    ".icon-file    { color: #6b6e7a; }\n"
    ".card-name {\n"
    "  font-size: 13px;\n"
    "  font-weight: 600;\n"
    "  color: #e2e2e9;\n"
    "}\n"
    ".card-meta {\n"
    "  font-size: 10px;\n"
    "  color: #6b6e7a;\n"
    "  margin-top: 2px;\n"
    "}\n"
    ".stats-card {\n"
    "  background-color: #0c0e13;\n"
    "  border: 1px solid #1e2025;\n"
    "  border-radius: 12px;\n"
    "  padding: 32px;\n"
    "  min-width: 420px;\n"
    "}\n"
    ".stats-title {\n"
    "  font-size: 11px;\n"
    "  font-weight: 700;\n"
    "  letter-spacing: 0.12em;\n"
    "  color: #ff79c6;\n"
    "  margin-bottom: 20px;\n"
    "}\n"
    ".stats-label { font-size: 12px; font-weight: 600; color: #6b6e7a; }\n"
    ".stats-value { font-size: 12px; font-weight: 500; color: #75d4e8; }\n"
    "headerbar {\n"
    "  background-color: #0c0e13;\n"
    "  border-bottom: 1px solid #1e2025;\n"
    "  box-shadow: none;\n"
    "  min-height: 46px;\n"
    "}\n"
    ".pop-section-label {\n"
    "  font-size: 10px;\n"
    "  font-weight: 700;\n"
    "  letter-spacing: 0.1em;\n"
    "  color: #6b6e7a;\n"
    "}\n"
    ".pop-btn {\n"
    "  background-color: #1a1c21;\n"
    "  border: 1px solid #282a2f;\n"
    "  border-radius: 8px;\n"
    "  color: #dac0ca;\n"
    "  font-size: 12px;\n"
    "  padding: 6px 14px;\n"
    "}\n"
    ".pop-btn:hover {\n"
    "  background-color: #282a2f;\n"
    "  color: #ffafd7;\n"
    "  border-color: #ff79c6;\n"
    "}\n"
    "scrollbar { background-color: transparent; border: none; }\n"
    "scrollbar slider {\n"
    "  background-color: #282a2f;\n"
    "  border-radius: 9999px;\n"
    "  min-width: 4px;\n"
    "  min-height: 4px;\n"
    "}\n"
    "scrollbar slider:hover { background-color: #ff79c6; }\n"
    ".bottom-bar {\n"
    "  background-color: #0c0e13;\n"
    "  border-top: 1px solid #1e2025;\n"
    "  padding: 6px 16px;\n"
    "}\n"
    ".bottom-bar-lbl {\n"
    "  color: #6b6e7a;\n"
    "  font-size: 11px;\n"
    "}\n"
    ".status-dot {\n"
    "  font-size: 14px;\n"
    "}\n"
    ".bottom-path {\n"
    "  color: #00f3ff;\n"
    "  font-size: 11px;\n"
    "  font-weight: 600;\n"
    "  margin-left: 6px;\n"
    "}\n"
    ".sync-card {\n"
    "  background-color: #111318;\n"
    "  border: 1px solid #1e2025;\n"
    "  border-radius: 8px;\n"
    "  padding: 10px;\n"
    "}\n"
    ".pop-entry {\n"
    "  background-color: #1a1c21;\n"
    "  border: 1px solid #282a2f;\n"
    "  border-radius: 6px;\n"
    "  color: #f8f8f2;\n"
    "  padding: 4px 8px;\n"
    "}\n"
    ".sync-now-btn {\n"
    "  background-color: #1a1c21;\n"
    "  border: 1px solid #ff79c6;\n"
    "  border-radius: 8px;\n"
    "  color: #ffafd7;\n"
    "  font-size: 12px;\n"
    "  padding: 6px 14px;\n"
    "  font-weight: bold;\n"
    "}\n"
    ".sync-now-btn:hover {\n"
    "  background-color: #ff79c6;\n"
    "  color: #0c0e13;\n"
    "}\n"
    ".kb-dialog {\n"
    "  background-color: #111318;\n"
    "  border-radius: 14px;\n"
    "}\n"
    ".kb-section-label {\n"
    "  font-size: 10px;\n"
    "  font-weight: 700;\n"
    "  letter-spacing: 0.12em;\n"
    "  color: #ff79c6;\n"
    "  margin-top: 10px;\n"
    "}\n"
    ".kb-key {\n"
    "  background-color: #1e2025;\n"
    "  border: 1px solid #282a2f;\n"
    "  border-radius: 5px;\n"
    "  color: #75d4e8;\n"
    "  font-size: 11px;\n"
    "  font-weight: 700;\n"
    "  padding: 2px 8px;\n"
    "  min-width: 120px;\n"
    "}\n"
    ".kb-desc {\n"
    "  color: #dac0ca;\n"
    "  font-size: 12px;\n"
    "  padding-left: 8px;\n"
    "}\n"
    ".add-action-btn {\n"
    "  background-color: #282a2f;\n"
    "  color: #ffafd7;\n"
    "  font-weight: 700;\n"
    "  border: 1px solid #282a2f;\n"
    "  border-radius: 8px;\n"
    "  padding: 8px 16px;\n"
    "}\n"
    ".add-action-btn:hover {\n"
    "  background-color: #1e2025;\n"
    "  color: #ffffff;\n"
    "}\n"
    ".bottom-toggle-btn {\n"
    "  background: transparent;\n"
    "  border: none;\n"
    "  box-shadow: none;\n"
    "  padding: 2px 6px;\n"
    "  color: #6b6e7a;\n"
    "}\n"
    ".bottom-toggle-btn:hover {\n"
    "  color: #ff79c6;\n"
    "}\n"
    ".bottom-icon-btn {\n"
    "  background: transparent;\n"
    "  border: none;\n"
    "  box-shadow: none;\n"
    "  border-radius: 6px;\n"
    "  padding: 3px 6px;\n"
    "  color: #6b6e7a;\n"
    "  transition: background 150ms, color 150ms;\n"
    "}\n"
    ".bottom-icon-btn:hover {\n"
    "  background-color: #1e2025;\n"
    "  color: #ffafd7;\n"
    "}\n"
    ".bottom-icon-btn.active {\n"
    "  background-color: #282a2f;\n"
    "  color: #ff79c6;\n"
    "}\n"
    ".bottom-icon-btn image { -gtk-icon-size: 14px; color: inherit; }\n"
    "list { background-color: transparent; border: none; box-shadow: none; }\n"
    "row { background-color: transparent; border: none; box-shadow: none; padding: 0; margin: 0; }\n"
    "row:hover { background-color: transparent; border: none; box-shadow: none; }\n"
    "row:selected { background-color: transparent; border: none; box-shadow: none; }\n";

static const char *CSS_CLASSIC_SEPIA = 
    "* { font-family: 'JetBrains Mono', 'Fira Mono', 'DejaVu Sans Mono', monospace; }\n"
    "window { background-color: #fdf6e3; }\n"
    ".sidebar {\n"
    "  background-color: #eee8d5;\n"
    "  border-right: 1px solid #d3c7a9;\n"
    "  min-width: 280px;\n"
    "  padding: 16px 12px;\n"
    "}\n"
    ".logo-btn {\n"
    "  background-color: #e4dbbe;\n"
    "  color: #a42e79;\n"
    "  font-weight: 700;\n"
    "  font-size: 14px;\n"
    "  border-radius: 8px;\n"
    "  padding: 4px 12px;\n"
    "  margin: 0 4px;\n"
    "}\n"
    ".nav-btn {\n"
    "  background: transparent;\n"
    "  border: none;\n"
    "  box-shadow: none;\n"
    "  border-radius: 10px;\n"
    "  padding: 10px;\n"
    "  margin: 2px 8px;\n"
    "  color: #586e75;\n"
    "  transition: background 180ms, color 180ms;\n"
    "}\n"
    ".nav-btn:hover {\n"
    "  background-color: #e4dbbe;\n"
    "  color: #268bd2;\n"
    "}\n"
    ".nav-btn image {\n"
    "  -gtk-icon-size: 18px;\n"
    "  color: inherit;\n"
    "}\n"
    ".nav-btn.active {\n"
    "  background-color: #e4dbbe;\n"
    "  color: #a42e79;\n"
    "  border-left: 2px solid #a42e79;\n"
    "}\n"
    ".workspace { background-color: #fdf6e3; }\n"
    ".pill {\n"
    "  font-size: 11px;\n"
    "  font-weight: 500;\n"
    "  padding: 4px 14px;\n"
    "  border-radius: 9999px;\n"
    "  background-color: #e4dbbe;\n"
    "  color: #586e75;\n"
    "}\n"
    ".pill-time { color: #268bd2; }\n"
    ".status-saved  { color: #859900; }\n"
    ".status-saving { color: #268bd2; }\n"
    ".status-failed { color: #dc322f; }\n"
    ".path-label {\n"
    "  color: #586e75;\n"
    "  font-size: 12px;\n"
    "}\n"
    ".search-bar-box {\n"
    "  background-color: #eee8d5;\n"
    "  border-top: 1px solid #d3c7a9;\n"
    "  padding: 6px 16px;\n"
    "}\n"
    "entry.search-entry {\n"
    "  background-color: #fdf6e3;\n"
    "  color: #073642;\n"
    "  border: 1px solid #d3c7a9;\n"
    "  border-radius: 8px;\n"
    "  padding: 4px 12px;\n"
    "  font-size: 13px;\n"
    "  min-width: 260px;\n"
    "  box-shadow: none;\n"
    "}\n"
    "entry.search-entry:focus {\n"
    "  border-color: #a42e79;\n"
    "  box-shadow: none;\n"
    "}\n"
    ".search-match-label {\n"
    "  color: #586e75;\n"
    "  font-size: 11px;\n"
    "  margin: 0 8px;\n"
    "}\n"
    ".search-nav-btn {\n"
    "  background: transparent;\n"
    "  border: none;\n"
    "  box-shadow: none;\n"
    "  border-radius: 6px;\n"
    "  padding: 4px 8px;\n"
    "  color: #586e75;\n"
    "}\n"
    ".search-nav-btn:hover { background-color: #e4dbbe; color: #a42e79; }\n"
    "textview, textview text, textview.source-view {\n"
    "  background-color: #fdf6e3;\n"
    "  color: #586e75;\n"
    "  caret-color: #a42e79;\n"
    "}\n"
    "textview text:selected {\n"
    "  background-color: #eee8d5;\n"
    "  color: #a42e79;\n"
    "}\n"
    "scrolledwindow {\n"
    "  border: none;\n"
    "  box-shadow: none;\n"
    "  background-color: #fdf6e3;\n"
    "}\n"
    ".editor-scroll {\n"
    "  border: 1px solid #eee8d5;\n"
    "  border-radius: 8px;\n"
    "}\n"
    ".explorer-header { padding: 24px 28px 12px 28px; }\n"
    ".explorer-title {\n"
    "  font-size: 15px;\n"
    "  font-weight: 700;\n"
    "  color: #a42e79;\n"
    "}\n"
    ".explorer-badge {\n"
    "  font-size: 10px;\n"
    "  font-weight: 500;\n"
    "  padding: 2px 10px;\n"
    "  border-radius: 9999px;\n"
    "  background-color: #e4dbbe;\n"
    "  color: #586e75;\n"
    "}\n"
    ".explorer-card {\n"
    "  background-color: #fdf6e3;\n"
    "  border: 1px solid #d3c7a9;\n"
    "  border-radius: 10px;\n"
    "  padding: 14px 16px;\n"
    "  min-width: 180px;\n"
    "  transition: background 150ms, border-color 150ms;\n"
    "}\n"
    ".explorer-card:hover {\n"
    "  background-color: #f5ecda;\n"
    "  border-color: #a42e79;\n"
    "}\n"
    ".icon-folder  { color: #268bd2; }\n"
    ".icon-file-md { color: #a42e79; }\n"
    ".icon-file    { color: #586e75; }\n"
    ".card-name {\n"
    "  font-size: 13px;\n"
    "  font-weight: 600;\n"
    "  color: #073642;\n"
    "}\n"
    ".card-meta {\n"
    "  font-size: 10px;\n"
    "  color: #586e75;\n"
    "  margin-top: 2px;\n"
    "}\n"
    ".stats-card {\n"
    "  background-color: #eee8d5;\n"
    "  border: 1px solid #d3c7a9;\n"
    "  border-radius: 12px;\n"
    "  padding: 32px;\n"
    "  min-width: 420px;\n"
    "}\n"
    ".stats-title {\n"
    "  font-size: 11px;\n"
    "  font-weight: 700;\n"
    "  letter-spacing: 0.12em;\n"
    "  color: #a42e79;\n"
    "  margin-bottom: 20px;\n"
    "}\n"
    ".stats-label { font-size: 12px; font-weight: 600; color: #586e75; }\n"
    ".stats-value { font-size: 12px; font-weight: 500; color: #268bd2; }\n"
    "headerbar {\n"
    "  background-color: #eee8d5;\n"
    "  border-bottom: 1px solid #d3c7a9;\n"
    "  box-shadow: none;\n"
    "  min-height: 46px;\n"
    "}\n"
    ".pop-section-label {\n"
    "  font-size: 10px;\n"
    "  font-weight: 700;\n"
    "  letter-spacing: 0.1em;\n"
    "  color: #586e75;\n"
    "}\n"
    ".pop-btn {\n"
    "  background-color: #fdf6e3;\n"
    "  border: 1px solid #d3c7a9;\n"
    "  border-radius: 8px;\n"
    "  color: #586e75;\n"
    "  font-size: 12px;\n"
    "  padding: 6px 14px;\n"
    "}\n"
    ".pop-btn:hover {\n"
    "  background-color: #e4dbbe;\n"
    "  color: #a42e79;\n"
    "  border-color: #a42e79;\n"
    "}\n"
    "scrollbar { background-color: transparent; border: none; }\n"
    "scrollbar slider {\n"
    "  background-color: #d3c7a9;\n"
    "  border-radius: 9999px;\n"
    "  min-width: 4px;\n"
    "  min-height: 4px;\n"
    "}\n"
    "scrollbar slider:hover { background-color: #a42e79; }\n"
    ".bottom-bar {\n"
    "  background-color: #eee8d5;\n"
    "  border-top: 1px solid #d3c7a9;\n"
    "  padding: 6px 16px;\n"
    "}\n"
    ".bottom-bar-lbl {\n"
    "  color: #586e75;\n"
    "  font-size: 11px;\n"
    "}\n"
    ".status-dot {\n"
    "  font-size: 14px;\n"
    "}\n"
    ".bottom-path {\n"
    "  color: #268bd2;\n"
    "  font-size: 11px;\n"
    "  font-weight: 600;\n"
    "  margin-left: 6px;\n"
    "}\n"
    ".sync-card {\n"
    "  background-color: #f5edd5;\n"
    "  border: 1px solid #d3c7a9;\n"
    "  border-radius: 8px;\n"
    "  padding: 10px;\n"
    "}\n"
    ".pop-entry {\n"
    "  background-color: #fdf6e3;\n"
    "  border: 1px solid #d3c7a9;\n"
    "  border-radius: 6px;\n"
    "  color: #586e75;\n"
    "  padding: 4px 8px;\n"
    "}\n"
    ".sync-now-btn {\n"
    "  background-color: #fdf6e3;\n"
    "  border: 1px solid #a42e79;\n"
    "  border-radius: 8px;\n"
    "  color: #a42e79;\n"
    "  font-size: 12px;\n"
    "  padding: 6px 14px;\n"
    "  font-weight: bold;\n"
    "}\n"
    ".sync-now-btn:hover {\n"
    "  background-color: #a42e79;\n"
    "  color: #fdf6e3;\n"
    "}\n"
    ".kb-dialog {\n"
    "  background-color: #fdf6e3;\n"
    "  border-radius: 14px;\n"
    "}\n"
    ".kb-section-label {\n"
    "  font-size: 10px;\n"
    "  font-weight: 700;\n"
    "  letter-spacing: 0.12em;\n"
    "  color: #a42e79;\n"
    "  margin-top: 10px;\n"
    "}\n"
    ".kb-key {\n"
    "  background-color: #e4dbbe;\n"
    "  border: 1px solid #d3c7a9;\n"
    "  border-radius: 5px;\n"
    "  color: #268bd2;\n"
    "  font-size: 11px;\n"
    "  font-weight: 700;\n"
    "  padding: 2px 8px;\n"
    "  min-width: 120px;\n"
    "}\n"
    ".kb-desc {\n"
    "  color: #586e75;\n"
    "  font-size: 12px;\n"
    "  padding-left: 8px;\n"
    "}\n"
    ".add-action-btn {\n"
    "  background-color: #e4dbbe;\n"
    "  color: #a42e79;\n"
    "  font-weight: 700;\n"
    "  border: 1px solid #d3c7a9;\n"
    "  border-radius: 8px;\n"
    "  padding: 8px 16px;\n"
    "}\n"
    ".add-action-btn:hover {\n"
    "  background-color: #eee8d5;\n"
    "  color: #a42e79;\n"
    "}\n"
    ".bottom-toggle-btn {\n"
    "  background: transparent;\n"
    "  border: none;\n"
    "  box-shadow: none;\n"
    "  padding: 2px 6px;\n"
    "  color: #586e75;\n"
    "}\n"
    ".bottom-toggle-btn:hover {\n"
    "  color: #a42e79;\n"
    "}\n"
    ".bottom-icon-btn {\n"
    "  background: transparent;\n"
    "  border: none;\n"
    "  box-shadow: none;\n"
    "  border-radius: 6px;\n"
    "  padding: 3px 6px;\n"
    "  color: #586e75;\n"
    "  transition: background 150ms, color 150ms;\n"
    "}\n"
    ".bottom-icon-btn:hover {\n"
    "  background-color: #d3c7a9;\n"
    "  color: #a42e79;\n"
    "}\n"
    ".bottom-icon-btn.active {\n"
    "  background-color: #e4dbbe;\n"
    "  color: #a42e79;\n"
    "}\n"
    ".bottom-icon-btn image { -gtk-icon-size: 14px; color: inherit; }\n"
    "list { background-color: transparent; border: none; box-shadow: none; }\n"
    "row { background-color: transparent; border: none; box-shadow: none; padding: 0; margin: 0; }\n"
    "row:hover { background-color: transparent; border: none; box-shadow: none; }\n"
    "row:selected { background-color: transparent; border: none; box-shadow: none; }\n";

static const char *CSS_MIDNIGHT = 
    "* { font-family: 'JetBrains Mono', 'Fira Mono', 'DejaVu Sans Mono', monospace; }\n"
    "window { background-color: #000000; }\n"
    ".sidebar {\n"
    "  background-color: #080808;\n"
    "  border-right: 1px solid #1c1c1c;\n"
    "  min-width: 280px;\n"
    "  padding: 16px 12px;\n"
    "}\n"
    ".logo-btn {\n"
    "  background-color: #1a1a1a;\n"
    "  color: #ffffff;\n"
    "  font-weight: 700;\n"
    "  font-size: 14px;\n"
    "  border-radius: 8px;\n"
    "  padding: 4px 12px;\n"
    "  margin: 0 4px;\n"
    "}\n"
    ".nav-btn {\n"
    "  background: transparent;\n"
    "  border: none;\n"
    "  box-shadow: none;\n"
    "  border-radius: 10px;\n"
    "  padding: 10px;\n"
    "  margin: 2px 8px;\n"
    "  color: #606060;\n"
    "  transition: background 180ms, color 180ms;\n"
    "}\n"
    ".nav-btn:hover {\n"
    "  background-color: #222222;\n"
    "  color: #d0d0d0;\n"
    "}\n"
    ".nav-btn image {\n"
    "  -gtk-icon-size: 18px;\n"
    "  color: inherit;\n"
    "}\n"
    ".nav-btn.active {\n"
    "  background-color: #1a1a1a;\n"
    "  color: #ffffff;\n"
    "  border-left: 2px solid #ff79c6;\n"
    "}\n"
    ".workspace { background-color: #000000; }\n"
    ".pill {\n"
    "  font-size: 11px;\n"
    "  font-weight: 500;\n"
    "  padding: 4px 14px;\n"
    "  border-radius: 9999px;\n"
    "  background-color: #161616;\n"
    "  color: #888888;\n"
    "}\n"
    ".pill-time { color: #50fa7b; }\n"
    ".status-saved  { color: #50fa7b; }\n"
    ".status-saving { color: #f1fa8c; }\n"
    ".status-failed { color: #ff5555; }\n"
    ".path-label {\n"
    "  color: #888888;\n"
    "  font-size: 12px;\n"
    "}\n"
    ".search-bar-box {\n"
    "  background-color: #080808;\n"
    "  border-top: 1px solid #1c1c1c;\n"
    "  padding: 6px 16px;\n"
    "}\n"
    "entry.search-entry {\n"
    "  background-color: #121212;\n"
    "  color: #ffffff;\n"
    "  border: 1px solid #242424;\n"
    "  border-radius: 8px;\n"
    "  padding: 4px 12px;\n"
    "  font-size: 13px;\n"
    "  min-width: 260px;\n"
    "  box-shadow: none;\n"
    "}\n"
    "entry.search-entry:focus {\n"
    "  border-color: #ff79c6;\n"
    "  box-shadow: none;\n"
    "}\n"
    ".search-match-label {\n"
    "  color: #888888;\n"
    "  font-size: 11px;\n"
    "  margin: 0 8px;\n"
    "}\n"
    ".search-nav-btn {\n"
    "  background: transparent;\n"
    "  border: none;\n"
    "  box-shadow: none;\n"
    "  border-radius: 6px;\n"
    "  padding: 4px 8px;\n"
    "  color: #888888;\n"
    "}\n"
    ".search-nav-btn:hover { background-color: #222222; color: #ffffff; }\n"
    "textview, textview text, textview.source-view {\n"
    "  background-color: #000000;\n"
    "  color: #ffffff;\n"
    "  caret-color: #ff79c6;\n"
    "}\n"
    "textview text:selected {\n"
    "  background-color: #222222;\n"
    "  color: #ff79c6;\n"
    "}\n"
    "scrolledwindow {\n"
    "  border: none;\n"
    "  box-shadow: none;\n"
    "  background-color: #000000;\n"
    "}\n"
    ".editor-scroll {\n"
    "  border: 1px solid #222222;\n"
    "  border-radius: 8px;\n"
    "}\n"
    ".explorer-header { padding: 24px 28px 12px 28px; }\n"
    ".explorer-title {\n"
    "  font-size: 15px;\n"
    "  font-weight: 700;\n"
    "  color: #ffffff;\n"
    "}\n"
    ".explorer-badge {\n"
    "  font-size: 10px;\n"
    "  font-weight: 500;\n"
    "  padding: 2px 10px;\n"
    "  border-radius: 9999px;\n"
    "  background-color: #161616;\n"
    "  color: #888888;\n"
    "}\n"
    ".explorer-card {\n"
    "  background-color: #0c0c0c;\n"
    "  border: 1px solid #1c1c1c;\n"
    "  border-radius: 10px;\n"
    "  padding: 14px 16px;\n"
    "  min-width: 180px;\n"
    "  transition: background 150ms, border-color 150ms;\n"
    "}\n"
    ".explorer-card:hover {\n"
    "  background-color: #161616;\n"
    "  border-color: #ff79c6;\n"
    "}\n"
    ".icon-folder  { color: #50fa7b; }\n"
    ".icon-file-md { color: #ff79c6; }\n"
    ".icon-file    { color: #888888; }\n"
    ".card-name {\n"
    "  font-size: 13px;\n"
    "  font-weight: 600;\n"
    "  color: #ffffff;\n"
    "}\n"
    ".card-meta {\n"
    "  font-size: 10px;\n"
    "  color: #888888;\n"
    "  margin-top: 2px;\n"
    "}\n"
    ".stats-card {\n"
    "  background-color: #080808;\n"
    "  border: 1px solid #1c1c1c;\n"
    "  border-radius: 12px;\n"
    "  padding: 32px;\n"
    "  min-width: 420px;\n"
    "}\n"
    ".stats-title {\n"
    "  font-size: 11px;\n"
    "  font-weight: 700;\n"
    "  letter-spacing: 0.12em;\n"
    "  color: #ff79c6;\n"
    "  margin-bottom: 20px;\n"
    "}\n"
    ".stats-label { font-size: 12px; font-weight: 600; color: #888888; }\n"
    ".stats-value { font-size: 12px; font-weight: 500; color: #50fa7b; }\n"
    "headerbar {\n"
    "  background-color: #080808;\n"
    "  border-bottom: 1px solid #1c1c1c;\n"
    "  box-shadow: none;\n"
    "  min-height: 46px;\n"
    "}\n"
    ".pop-section-label {\n"
    "  font-size: 10px;\n"
    "  font-weight: 700;\n"
    "  letter-spacing: 0.1em;\n"
    "  color: #888888;\n"
    "}\n"
    ".pop-btn {\n"
    "  background-color: #0c0c0c;\n"
    "  border: 1px solid #1c1c1c;\n"
    "  border-radius: 8px;\n"
    "  color: #888888;\n"
    "  font-size: 12px;\n"
    "  padding: 6px 14px;\n"
    "}\n"
    ".pop-btn:hover {\n"
    "  background-color: #1a1a1a;\n"
    "  color: #ffffff;\n"
    "  border-color: #ff79c6;\n"
    "}\n"
    "scrollbar { background-color: transparent; border: none; }\n"
    "scrollbar slider {\n"
    "  background-color: #1c1c1c;\n"
    "  border-radius: 9999px;\n"
    "  min-width: 4px;\n"
    "  min-height: 4px;\n"
    "}\n"
    "scrollbar slider:hover { background-color: #ff79c6; }\n"
    ".bottom-bar {\n"
    "  background-color: #080808;\n"
    "  border-top: 1px solid #1c1c1c;\n"
    "  padding: 6px 16px;\n"
    "}\n"
    ".bottom-bar-lbl {\n"
    "  color: #888888;\n"
    "  font-size: 11px;\n"
    "}\n"
    ".status-dot {\n"
    "  font-size: 14px;\n"
    "}\n"
    ".bottom-path {\n"
    "  color: #00ffff;\n"
    "  font-size: 11px;\n"
    "  font-weight: 600;\n"
    "  margin-left: 6px;\n"
    "}\n"
    ".sync-card {\n"
    "  background-color: #040404;\n"
    "  border: 1px solid #1c1c1c;\n"
    "  border-radius: 8px;\n"
    "  padding: 10px;\n"
    "}\n"
    ".pop-entry {\n"
    "  background-color: #080808;\n"
    "  border: 1px solid #1c1c1c;\n"
    "  border-radius: 6px;\n"
    "  color: #ffffff;\n"
    "  padding: 4px 8px;\n"
    "}\n"
    ".sync-now-btn {\n"
    "  background-color: #080808;\n"
    "  border: 1px solid #ff79c6;\n"
    "  border-radius: 8px;\n"
    "  color: #ffafd7;\n"
    "  font-size: 12px;\n"
    "  padding: 6px 14px;\n"
    "  font-weight: bold;\n"
    "}\n"
    ".sync-now-btn:hover {\n"
    "  background-color: #ff79c6;\n"
    "  color: #000000;\n"
    "}\n"
    ".kb-dialog {\n"
    "  background-color: #000000;\n"
    "  border-radius: 14px;\n"
    "}\n"
    ".kb-section-label {\n"
    "  font-size: 10px;\n"
    "  font-weight: 700;\n"
    "  letter-spacing: 0.12em;\n"
    "  color: #ff79c6;\n"
    "  margin-top: 10px;\n"
    "}\n"
    ".kb-key {\n"
    "  background-color: #161616;\n"
    "  border: 1px solid #242424;\n"
    "  border-radius: 5px;\n"
    "  color: #50fa7b;\n"
    "  font-size: 11px;\n"
    "  font-weight: 700;\n"
    "  padding: 2px 8px;\n"
    "  min-width: 120px;\n"
    "}\n"
    ".kb-desc {\n"
    "  color: #d0d0d0;\n"
    "  font-size: 12px;\n"
    "  padding-left: 8px;\n"
    "}\n"
    ".add-action-btn {\n"
    "  background-color: #161616;\n"
    "  color: #ffffff;\n"
    "  font-weight: 700;\n"
    "  border: 1px solid #242424;\n"
    "  border-radius: 8px;\n"
    "  padding: 8px 16px;\n"
    "}\n"
    ".add-action-btn:hover {\n"
    "  background-color: #242424;\n"
    "  color: #ff79c6;\n"
    "}\n"
    ".bottom-toggle-btn {\n"
    "  background: transparent;\n"
    "  border: none;\n"
    "  box-shadow: none;\n"
    "  padding: 2px 6px;\n"
    "  color: #6c6c6c;\n"
    "}\n"
    ".bottom-toggle-btn:hover {\n"
    "  color: #ff79c6;\n"
    "}\n"
    ".bottom-icon-btn {\n"
    "  background: transparent;\n"
    "  border: none;\n"
    "  box-shadow: none;\n"
    "  border-radius: 6px;\n"
    "  padding: 3px 6px;\n"
    "  color: #6c6c6c;\n"
    "  transition: background 150ms, color 150ms;\n"
    "}\n"
    ".bottom-icon-btn:hover {\n"
    "  background-color: #242424;\n"
    "  color: #ff79c6;\n"
    "}\n"
    ".bottom-icon-btn.active {\n"
    "  background-color: #1a1a1a;\n"
    "  color: #ff79c6;\n"
    "}\n"
    ".bottom-icon-btn image { -gtk-icon-size: 14px; color: inherit; }\n"
    "list { background-color: transparent; border: none; box-shadow: none; }\n"
    "row { background-color: transparent; border: none; box-shadow: none; padding: 0; margin: 0; }\n"
    "row:hover { background-color: transparent; border: none; box-shadow: none; }\n"
    "row:selected { background-color: transparent; border: none; box-shadow: none; }\n";

static char current_theme[32] = "dark";

static void apply_theme(AppGui *gui, const char *theme_name) {
    strncpy(current_theme, theme_name, sizeof(current_theme) - 1);
    current_theme[sizeof(current_theme) - 1] = '\0';

    const char *base_css = NULL;
    const char *gutter_color = "#555555";
    const char *active_num_color = "#ff79c6";
    if (strcmp(theme_name, "sepia") == 0) {
        base_css = CSS_CLASSIC_SEPIA;
        gutter_color = "#586e75";
        active_num_color = "#a42e79";
    } else if (strcmp(theme_name, "midnight") == 0) {
        base_css = CSS_MIDNIGHT;
        gutter_color = "#555555";
        active_num_color = "#ff79c6";
    } else {
        base_css = CSS_DEEP_SLATE;
        gutter_color = "#555555";
        active_num_color = "#ff79c6";
    }
    
    if (gui->css_provider) {
        gchar *full_css = g_strdup_printf(
            "%s\n"
            "textview selection { padding: 0px; }\n"
            "textview.sourceview selection { padding: 0px; }\n"
            "gutterview { background-color: transparent; background: transparent; color: %s; }\n"
            "gutterview > line { color: %s; }\n"
            "gutterview > line.current-line-number {\n"
            "  color: %s;\n"
            "  font-weight: bold;\n"
            "}\n"
            "gutter { background-color: transparent; background: transparent; color: %s; }\n"
            "gutter > line { color: %s; }\n"
            "gutter > line.current-line-number {\n"
            "  color: %s;\n"
            "  font-weight: bold;\n"
            "}\n",
            base_css, gutter_color, gutter_color, active_num_color,
            gutter_color, gutter_color, active_num_color
        );
        gtk_css_provider_load_from_string(gui->css_provider, full_css);
        g_free(full_css);
    }
    
    if (gui->source_view) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
        GtkSourceBuffer *src_buf = GTK_SOURCE_BUFFER(buf);
        GtkSourceStyleSchemeManager *sm = gtk_source_style_scheme_manager_get_default();
        GtkSourceStyleScheme *scheme = NULL;
        if (strcmp(theme_name, "sepia") == 0) {
            scheme = gtk_source_style_scheme_manager_get_scheme(sm, "qirtas-sepia");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "classic");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "solarized-light");
        } else if (strcmp(theme_name, "midnight") == 0) {
            scheme = gtk_source_style_scheme_manager_get_scheme(sm, "qirtas-midnight");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "oblivion");
        } else {
            scheme = gtk_source_style_scheme_manager_get_scheme(sm, "qirtas-dark");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "adwaita-dark");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "oblivion");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "solarized-dark");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "classic-dark");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "cobalt");
        }
        gtk_source_buffer_set_style_scheme(src_buf, scheme);
    }


}

static void init_css(AppGui *gui) {
    /* 1. Load external assets/style.css stylesheet */
    GtkCssProvider *file_provider = gtk_css_provider_new();
    GFile *file = g_file_new_for_path("assets/style.css");
    if (g_file_query_exists(file, NULL)) {
        gtk_css_provider_load_from_file(file_provider, file);
    }
    g_object_unref(file);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(file_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(file_provider);

    /* 2. Load dynamic theme provider */
    GtkCssProvider *p = gtk_css_provider_new();
    gui->css_provider = p;
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(p),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(p);
    
    apply_theme(gui, "dark");
    update_editor_font(gui);
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

    GtkTextIter start = *iter;
    gtk_text_iter_set_line_offset(&start, 0);
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

static void update_all_paragraphs_direction(GtkTextBuffer *buf) {
    GtkTextIter iter;
    gtk_text_buffer_get_start_iter(buf, &iter);
    while (TRUE) {
        update_paragraph_direction(buf, &iter);
        if (!gtk_text_iter_forward_line(&iter)) {
            break;
        }
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
static void apply_format(GtkTextBuffer *buf, const char *prefix, const char *suffix) {
    GtkTextIter start, end;
    gint saved_start, saved_end;
    if (gtk_text_buffer_get_selection_bounds(buf, &start, &end)) {
        saved_start = gtk_text_iter_get_offset(&start);
        saved_end   = gtk_text_iter_get_offset(&end);
    } else {
        GtkTextMark *insert_mark = gtk_text_buffer_get_insert(buf);
        GtkTextIter cursor_iter;
        gtk_text_buffer_get_iter_at_mark(buf, &cursor_iter, insert_mark);
        saved_start = saved_end = gtk_text_iter_get_offset(&cursor_iter);
    }
    apply_format_with_saved(buf, prefix, suffix, saved_start, saved_end);
}

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
}

/* Entry point for keyboard shortcuts — uses live selection */
static void apply_paragraph_format(GtkTextBuffer *buf, const char *prefix) {
    GtkTextIter start, end;
    if (!gtk_text_buffer_get_selection_bounds(buf, &start, &end)) {
        gtk_text_buffer_get_iter_at_mark(buf, &start, gtk_text_buffer_get_insert(buf));
        end = start;
    }
    gint start_line = gtk_text_iter_get_line(&start);
    gint end_line   = gtk_text_iter_get_line(&end);
    apply_paragraph_format_core(buf, prefix, start_line, end_line);
}

static void on_format_clicked(GtkButton *btn, gpointer user_data) {
    PopoverData *pd = (PopoverData *)user_data;
    const char *prefix = g_object_get_data(G_OBJECT(btn), "prefix");
    const char *suffix = g_object_get_data(G_OBJECT(btn), "suffix");
    apply_format_with_saved(pd->buf, prefix, suffix, pd->saved_start, pd->saved_end);
    gtk_popover_popdown(GTK_POPOVER(pd->popover));
}

static void on_para_clicked(GtkButton *btn, gpointer user_data) {
    PopoverData *pd = (PopoverData *)user_data;
    const char *prefix = g_object_get_data(G_OBJECT(btn), "prefix");
    apply_paragraph_format_with_saved(pd->buf, prefix, pd->saved_start, pd->saved_end);
    gtk_popover_popdown(GTK_POPOVER(pd->popover));
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

static void on_editor_right_click(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data) {
    (void)n_press;
    AppGui *gui = (AppGui *)user_data;
    
    if (gui->active_popover) {
        gtk_widget_unparent(gui->active_popover);
    }
    
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
    
    GtkWidget *popover = gtk_popover_new();
    gtk_widget_set_parent(popover, gui->source_view);
    gui->active_popover = popover;
    g_signal_connect(popover, "destroy", G_CALLBACK(on_popover_destroy), gui);
    g_signal_connect(popover, "closed", G_CALLBACK(on_editor_popover_closed), NULL);
    
    GdkRectangle rect = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    
    PopoverData *pd = g_new(PopoverData, 1);
    pd->buf = buf;
    pd->popover = popover;

    /* Save selection NOW, before gtk_popover_popup() steals focus and clears it */
    {
        GtkTextIter sel_start, sel_end;
        if (gtk_text_buffer_get_selection_bounds(buf, &sel_start, &sel_end)) {
            pd->saved_start = gtk_text_iter_get_offset(&sel_start);
            pd->saved_end   = gtk_text_iter_get_offset(&sel_end);
        } else {
            GtkTextMark *ins = gtk_text_buffer_get_insert(buf);
            GtkTextIter cursor;
            gtk_text_buffer_get_iter_at_mark(buf, &cursor, ins);
            pd->saved_start = pd->saved_end = gtk_text_iter_get_offset(&cursor);
        }
    }

    g_signal_connect_swapped(popover, "destroy", G_CALLBACK(g_free), pd);
    
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(main_box, 8);
    gtk_widget_set_margin_end(main_box, 8);
    gtk_widget_set_margin_top(main_box, 8);
    gtk_widget_set_margin_bottom(main_box, 8);
    
    // Left: FORMAT
    GtkWidget *format_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *lbl_format = gtk_label_new("FORMAT");
    gtk_widget_add_css_class(lbl_format, "pop-section-label");
    gtk_widget_set_halign(lbl_format, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(format_box), lbl_format);
    
    char *f_labels[] = { "Bold", "Italic", "Strikethrough", "Highlight", "Code", "Comment", "Math", "Quote" };
    char *f_prefixes[] = { "**", "*", "~~", "==", "`", "<!-- ", "$", "> " };
    char *f_suffixes[] = { "**", "*", "~~", "==", "`", " -->", "$", "" };
    
    for (int i = 0; i < 8; i++) {
        GtkWidget *btn = gtk_button_new_with_label(f_labels[i]);
        gtk_widget_add_css_class(btn, "pop-btn");
        gtk_widget_set_halign(btn, GTK_ALIGN_FILL);
        
        if (i == 7) {
            g_object_set_data(G_OBJECT(btn), "prefix", "> ");
            g_signal_connect(btn, "clicked", G_CALLBACK(on_para_clicked), pd);
        } else {
            g_object_set_data(G_OBJECT(btn), "prefix", f_prefixes[i]);
            g_object_set_data(G_OBJECT(btn), "suffix", f_suffixes[i]);
            g_signal_connect(btn, "clicked", G_CALLBACK(on_format_clicked), pd);
        }
        gtk_box_append(GTK_BOX(format_box), btn);
    }
    
    // Right: PARAGRAPH
    GtkWidget *para_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *lbl_para = gtk_label_new("PARAGRAPH");
    gtk_widget_add_css_class(lbl_para, "pop-section-label");
    gtk_widget_set_halign(lbl_para, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(para_box), lbl_para);
    
    // Headings row
    GtkWidget *h_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_set_homogeneous(GTK_BOX(h_box), TRUE);
    char *h_labels[] = { "H1", "H2", "H3", "H4", "H5", "H6" };
    char *h_prefixes[] = { "# ", "## ", "### ", "#### ", "##### ", "###### " };
    for (int i = 0; i < 6; i++) {
        GtkWidget *h_btn = gtk_button_new_with_label(h_labels[i]);
        gtk_widget_add_css_class(h_btn, "pop-btn");
        g_object_set_data(G_OBJECT(h_btn), "prefix", h_prefixes[i]);
        g_signal_connect(h_btn, "clicked", G_CALLBACK(on_para_clicked), pd);
        gtk_box_append(GTK_BOX(h_box), h_btn);
    }
    gtk_box_append(GTK_BOX(para_box), h_box);
    
    char *p_labels[] = { "Body", "Bullet List", "Task List", "Numbered List" };
    char *p_prefixes[] = { "", "- ", "- [ ] ", "1. " };
    for (int i = 0; i < 4; i++) {
        GtkWidget *btn = gtk_button_new_with_label(p_labels[i]);
        gtk_widget_add_css_class(btn, "pop-btn");
        gtk_widget_set_halign(btn, GTK_ALIGN_FILL);
        g_object_set_data(G_OBJECT(btn), "prefix", p_prefixes[i]);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_para_clicked), pd);
        gtk_box_append(GTK_BOX(para_box), btn);
    }
    
    gtk_box_append(GTK_BOX(main_box), format_box);
    gtk_box_append(GTK_BOX(main_box), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(main_box), para_box);
    
    gtk_popover_set_child(GTK_POPOVER(popover), main_box);
    gtk_popover_popup(GTK_POPOVER(popover));
    
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void apply_wiki_link_tags(GtkTextBuffer *buf) {
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

static void on_editor_left_click(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data) {
    (void)n_press;
    AppGui *gui = (AppGui *)user_data;
    
    if (gui->sidebar && gtk_widget_get_visible(gui->sidebar)) {
        gtk_widget_set_visible(gui->sidebar, FALSE);
    }
    
    int bx, by;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(gui->source_view),
                                         GTK_TEXT_WINDOW_TEXT,
                                         (int)x, (int)y, &bx, &by);
                                         
    GtkTextIter iter;
    if (gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(gui->source_view), &iter, bx, by)) {
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
            
            char *full_link = gtk_text_buffer_get_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view)), &tag_start, &tag_end, FALSE);
            size_t len = strlen(full_link);
            if (len > 4 && full_link[0] == '[' && full_link[1] == '[') {
                char *note_name = g_strndup(full_link + 2, len - 4);
                zig_open_wiki_link(note_name);
                g_free(note_name);
            }
            g_free(full_link);
        }
    }
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
    GRegex *regex = g_regex_new(pattern, G_REGEX_DEFAULT, 0, &error);
    if (!regex) {
        if (error) {
            g_printerr("Failed to compile regex %s: %s\n", pattern, error->message);
            g_clear_error(&error);
        }
        return;
    }

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
    g_regex_unref(regex);
}

static void update_conceal_markdown(GtkTextBuffer *buf) {
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buf);
    GtkTextTag *conceal_tag = gtk_text_tag_table_lookup(table, "conceal");
    if (!conceal_tag) {
        conceal_tag = gtk_text_buffer_create_tag(buf, "conceal", "invisible", TRUE, NULL);
    }

    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    gtk_text_buffer_remove_tag(buf, conceal_tag, &start, &end);

    gchar *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    if (!text || strlen(text) == 0) {
        g_free(text);
        return;
    }

    GtkTextMark *insert_mark = gtk_text_buffer_get_insert(buf);
    GtkTextIter cursor_iter;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor_iter, insert_mark);
    gint cursor_char = gtk_text_iter_get_offset(&cursor_iter);

    // Apply conceal to bold
    apply_regex_conceal(buf, text, "\\*\\*([^\\n]+?)\\*\\*", cursor_char, 2, conceal_tag);

    // Apply conceal to highlight
    apply_regex_conceal(buf, text, "==([^\\n]+?)==", cursor_char, 2, conceal_tag);

    // Apply conceal to italic
    apply_regex_conceal(buf, text, "(?<!\\*)\\*([^\\n\\*]+?)\\*(?!\\*)", cursor_char, 1, conceal_tag);

    g_free(text);
}

static void on_buffer_changed(GtkTextBuffer *buf, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    gchar *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    
    glong char_count = g_utf8_strlen(text, -1);
    
    glong word_count = 0;
    gboolean in_word = FALSE;
    char *p = text;
    while (*p) {
        gunichar c = g_utf8_get_char(p);
        if (g_unichar_isspace(c) || g_unichar_ispunct(c)) {
            in_word = FALSE;
        } else {
            if (!in_word) {
                word_count++;
                in_word = TRUE;
            }
        }
        p = g_utf8_next_char(p);
    }
    
    char w_buf[32], c_buf[32];
    snprintf(w_buf, sizeof(w_buf), "%ld words", word_count);
    snprintf(c_buf, sizeof(c_buf), "%ld chars", char_count);
    
    if (gui->lbl_words) gtk_label_set_text(GTK_LABEL(gui->lbl_words), w_buf);
    if (gui->lbl_chars) gtk_label_set_text(GTK_LABEL(gui->lbl_chars), c_buf);
    
    g_free(text);
    update_conceal_markdown(buf);
}

static void on_mark_set(GtkTextBuffer *buf, GtkTextIter *location, GtkTextMark *mark, gpointer user_data) {
    (void)location;
    AppGui *gui = (AppGui *)user_data;
    if (gui->in_scroll_update) return;
    if (!gui->vadjustment || !gui->source_view || !gui->virtual_layout_box) return;

    GtkTextMark *insert_mark = gtk_text_buffer_get_insert(buf);
    if (mark == insert_mark) {
        update_conceal_markdown(buf);
        if (!gtk_widget_get_realized(gui->source_view)) return;

        GtkTextIter iter;
        gtk_text_buffer_get_iter_at_mark(buf, &iter, insert_mark);

        GdkRectangle rect;
        gtk_text_view_get_iter_location(GTK_TEXT_VIEW(gui->source_view), &iter, &rect);

        int win_x = 0, win_y = 0;
        gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(gui->source_view),
                                              GTK_TEXT_WINDOW_WIDGET,
                                              rect.x, rect.y,
                                              &win_x, &win_y);

        graphene_point_t p, out_p;
        p.x = (float)win_x;
        p.y = (float)win_y;
        if (gtk_widget_compute_point(gui->source_view,
                                     gui->virtual_layout_box,
                                     &p, &out_p)) {
            double dest_x = out_p.x;
            double dest_y = out_p.y;
            double value = gtk_adjustment_get_value(gui->vadjustment);
            double page_size = gtk_adjustment_get_page_size(gui->vadjustment);
            double cursor_y = dest_y;
            double cursor_h = (double)rect.height;

            int line_h = get_line_height(gui->source_view);
            if (line_h <= 0) line_h = 24;

            // Define a comfortable cushion (5 lines cushion at bottom, 3 lines at top)
            double top_cushion = 3.0 * line_h;
            double bottom_cushion = 5.0 * line_h;

            if (top_cushion + bottom_cushion > page_size) {
                top_cushion = page_size / 4.0;
                bottom_cushion = page_size / 4.0;
            }

            double target_value = value;

            if (cursor_y < value + top_cushion) {
                target_value = cursor_y - top_cushion;
            } else if (cursor_y + cursor_h > value + page_size - bottom_cushion) {
                target_value = cursor_y + cursor_h - page_size + bottom_cushion;
            }

            double lower = gtk_adjustment_get_lower(gui->vadjustment);
            double upper = gtk_adjustment_get_upper(gui->vadjustment);
            if (target_value < lower) target_value = lower;
            if (target_value > upper - page_size) target_value = upper - page_size;

            if (target_value != value) {
                gtk_adjustment_set_value(gui->vadjustment, target_value);
            }
        }
    }
}

static void on_cursor_position_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    (void)user_data;
    update_conceal_markdown(GTK_TEXT_BUFFER(object));
}

static void on_insert_text_before(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data) {
    (void)user_data;
    if (len != 1) return;
    
    char c = text[0];
    char closing = 0;
    if (c == '"') closing = '"';
    else if (c == '[') closing = ']';
    else if (c == '(') closing = ')';
    else if (c == '{') closing = '}';
    else if (c == '*') closing = '*';
    else if (c == '_') closing = '_';
    else if (c == '`') closing = '`';
    
    if (closing != 0) {
        GtkTextIter next_iter = *location;
        gunichar next_char = gtk_text_iter_get_char(&next_iter);
        if ((c == '"' || c == '*' || c == '_' || c == '`') && (gunichar)c == next_char) {
            gtk_text_iter_forward_char(&next_iter);
            gtk_text_buffer_place_cursor(buf, &next_iter);
            g_signal_stop_emission_by_name(buf, "insert-text");
            return;
        }

        gint offset = gtk_text_iter_get_offset(location);
        g_signal_handlers_block_by_func(buf, on_insert_text_before, user_data);
        
        gchar both_chars[3] = { c, closing, 0 };
        GtkTextIter target_iter;
        gtk_text_buffer_get_iter_at_offset(buf, &target_iter, offset);
        gtk_text_buffer_insert(buf, &target_iter, both_chars, 2);
        
        GtkTextIter cursor_iter;
        gtk_text_buffer_get_iter_at_offset(buf, &cursor_iter, offset + 1);
        gtk_text_buffer_place_cursor(buf, &cursor_iter);
        
        g_signal_handlers_unblock_by_func(buf, on_insert_text_before, user_data);
        g_signal_stop_emission_by_name(buf, "insert-text");
    } else if (c == '"' || c == ']' || c == ')' || c == '}' || c == '*' || c == '_' || c == '`') {
        GtkTextIter next_iter = *location;
        gunichar next_char = gtk_text_iter_get_char(&next_iter);
        if ((gunichar)c == next_char) {
            gtk_text_iter_forward_char(&next_iter);
            gtk_text_buffer_place_cursor(buf, &next_iter);
            g_signal_stop_emission_by_name(buf, "insert-text");
            return;
        }
    }
}

static void show_keybindings_window(AppGui *gui) {
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Keyboard Shortcuts");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 520, 560);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(gui->window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    gtk_widget_add_css_class(dialog, "kb-dialog");

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_window_set_child(GTK_WINDOW(dialog), scroll);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(vbox, 24);
    gtk_widget_set_margin_end(vbox, 24);
    gtk_widget_set_margin_top(vbox, 20);
    gtk_widget_set_margin_bottom(vbox, 20);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), vbox);

    /* Helper: add section header */
    #define KB_SECTION(label_text) { \
        GtkWidget *_s = gtk_label_new(label_text); \
        gtk_widget_add_css_class(_s, "kb-section-label"); \
        gtk_widget_set_halign(_s, GTK_ALIGN_START); \
        gtk_box_append(GTK_BOX(vbox), _s); \
    }
    /* Helper: add keybinding row */
    #define KB_ROW(shortcut, description) { \
        GtkWidget *_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); \
        GtkWidget *_k = gtk_label_new(shortcut); \
        gtk_widget_add_css_class(_k, "kb-key"); \
        gtk_widget_set_halign(_k, GTK_ALIGN_START); \
        GtkWidget *_d = gtk_label_new(description); \
        gtk_widget_add_css_class(_d, "kb-desc"); \
        gtk_widget_set_halign(_d, GTK_ALIGN_START); \
        gtk_widget_set_hexpand(_d, TRUE); \
        gtk_box_append(GTK_BOX(_row), _k); \
        gtk_box_append(GTK_BOX(_row), _d); \
        gtk_box_append(GTK_BOX(vbox), _row); \
    }

    KB_SECTION("FORMATTING")
    KB_ROW("Ctrl + B",       "Bold selected text");
    KB_ROW("Ctrl + I",       "Italic selected text");
    KB_ROW("Ctrl + K",       "Inline code");
    KB_ROW("Ctrl + H",       "Highlight selected text");
    KB_ROW("Ctrl + Shift+S", "Strikethrough selected text");
    KB_ROW("Ctrl + Q",       "Blockquote selected text");
    KB_ROW("Ctrl + M",       "Inline math ($…$)");

    KB_SECTION("PARAGRAPH")
    KB_ROW("Ctrl + 1",       "Heading 1 (# )");
    KB_ROW("Ctrl + 2",       "Heading 2 (## )");
    KB_ROW("Ctrl + 3",       "Heading 3 (### )");
    KB_ROW("Ctrl + 4",       "Heading 4 (#### )");
    KB_ROW("Ctrl + 5",       "Heading 5 (##### )");
    KB_ROW("Ctrl + 6",       "Heading 6 (###### )");
    KB_ROW("Ctrl + 0",       "Body (remove prefix)");
    KB_ROW("Ctrl + Shift+L", "Bullet list item");
    KB_ROW("Ctrl + Shift+O", "Numbered list item");
    KB_ROW("Ctrl + Shift+T", "Task / checkbox item");

    KB_SECTION("NAVIGATION & UI")
    KB_ROW("Ctrl + F",       "Toggle search bar");
    KB_ROW("Ctrl + S",       "Force save now");
    KB_ROW("Ctrl + ?",       "Show this shortcuts reference");
    KB_ROW("Escape",         "Close search bar");
    KB_ROW("Return (list)",  "Continue list on next line");
    KB_ROW("Return (empty)", "Exit list / clear prefix");
    KB_ROW("Click [[link]]", "Open / create wiki-link note");

    #undef KB_SECTION
    #undef KB_ROW

    gtk_window_present(GTK_WINDOW(dialog));
}

static gboolean keycode_matches_latin_keyval(guint keycode, guint target_keyval) {
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return FALSE;

    GdkKeymapKey *keys = NULL;
    guint *keyvals = NULL;
    int n_entries = 0;
    gboolean found = FALSE;

    if (gdk_display_map_keycode(display, keycode, &keys, &keyvals, &n_entries)) {
        for (int i = 0; i < n_entries; i++) {
            guint kv = keyvals[i];
            if (kv == target_keyval ||
                (target_keyval >= GDK_KEY_a && target_keyval <= GDK_KEY_z && kv == target_keyval - 32) ||
                (target_keyval >= GDK_KEY_A && target_keyval <= GDK_KEY_Z && kv == target_keyval + 32)) {
                found = TRUE;
                break;
            }
        }
        g_free(keys);
        g_free(keyvals);
    }
    return found;
}

static gboolean on_editor_key_pressed(GtkEventControllerKey *ctrl,
                                      guint keyval, guint keycode,
                                      GdkModifierType state, gpointer user_data) {
    (void)ctrl;
    AppGui *gui = (AppGui *)user_data;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));

    gboolean ctrl_held  = (state & GDK_CONTROL_MASK) != 0;
    gboolean shift_held = (state & GDK_SHIFT_MASK)   != 0;

    /* ── Ctrl+B  Bold ── */
    if (ctrl_held && !shift_held && (keyval == GDK_KEY_b || keyval == GDK_KEY_B || keycode_matches_latin_keyval(keycode, GDK_KEY_b))) {
        apply_format(buf, "**", "**");
        return TRUE;
    }
    /* ── Ctrl+I  Italic ── */
    if (ctrl_held && !shift_held && (keyval == GDK_KEY_i || keyval == GDK_KEY_I || keycode_matches_latin_keyval(keycode, GDK_KEY_i))) {
        apply_format(buf, "*", "*");
        return TRUE;
    }
    /* ── Ctrl+K  Inline code ── */
    if (ctrl_held && !shift_held && (keyval == GDK_KEY_k || keyval == GDK_KEY_K || keycode_matches_latin_keyval(keycode, GDK_KEY_k))) {
        apply_format(buf, "`", "`");
        return TRUE;
    }
    /* ── Ctrl+H  Highlight ── */
    if (ctrl_held && !shift_held && (keyval == GDK_KEY_h || keyval == GDK_KEY_H || keycode_matches_latin_keyval(keycode, GDK_KEY_h))) {
        apply_format(buf, "==", "==");
        return TRUE;
    }
    /* ── Ctrl+Shift+S  Strikethrough ── */
    if (ctrl_held && shift_held && (keyval == GDK_KEY_s || keyval == GDK_KEY_S || keycode_matches_latin_keyval(keycode, GDK_KEY_s))) {
        apply_format(buf, "~~", "~~");
        return TRUE;
    }
    /* ── Ctrl+Q  Blockquote ── */
    if (ctrl_held && !shift_held && (keyval == GDK_KEY_q || keyval == GDK_KEY_Q || keycode_matches_latin_keyval(keycode, GDK_KEY_q))) {
        apply_paragraph_format(buf, "> ");
        return TRUE;
    }
    /* ── Ctrl+M  Math ── */
    if (ctrl_held && !shift_held && (keyval == GDK_KEY_m || keyval == GDK_KEY_M || keycode_matches_latin_keyval(keycode, GDK_KEY_m))) {
        apply_format(buf, "$", "$");
        return TRUE;
    }
    /* ── Ctrl+1..6  Headings ── */
    if (ctrl_held && !shift_held) {
        const char *h_prefix = NULL;
        if      (keyval == GDK_KEY_1) h_prefix = "# ";
        else if (keyval == GDK_KEY_2) h_prefix = "## ";
        else if (keyval == GDK_KEY_3) h_prefix = "### ";
        else if (keyval == GDK_KEY_4) h_prefix = "#### ";
        else if (keyval == GDK_KEY_5) h_prefix = "##### ";
        else if (keyval == GDK_KEY_6) h_prefix = "###### ";
        else if (keyval == GDK_KEY_0) h_prefix = ""; /* remove prefix */
        if (h_prefix != NULL) {
            apply_paragraph_format(buf, h_prefix);
            return TRUE;
        }
    }
    /* ── Ctrl+Shift+L  Bullet list ── */
    if (ctrl_held && shift_held && (keyval == GDK_KEY_l || keyval == GDK_KEY_L || keycode_matches_latin_keyval(keycode, GDK_KEY_l))) {
        apply_paragraph_format(buf, "- ");
        return TRUE;
    }
    /* ── Ctrl+Shift+O  Ordered list ── */
    if (ctrl_held && shift_held && (keyval == GDK_KEY_o || keyval == GDK_KEY_O || keycode_matches_latin_keyval(keycode, GDK_KEY_o))) {
        apply_paragraph_format(buf, "1. ");
        return TRUE;
    }
    /* ── Ctrl+Shift+T  Task list ── */
    if (ctrl_held && shift_held && (keyval == GDK_KEY_t || keyval == GDK_KEY_T || keycode_matches_latin_keyval(keycode, GDK_KEY_t))) {
        apply_paragraph_format(buf, "- [ ] ");
        return TRUE;
    }
    /* ── Ctrl+S  Force save ── */
    if (ctrl_held && !shift_held && (keyval == GDK_KEY_s || keyval == GDK_KEY_S || keycode_matches_latin_keyval(keycode, GDK_KEY_s))) {
        zig_force_save();
        return TRUE;
    }

    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        GtkTextIter insert_iter;
        gtk_text_buffer_get_iter_at_mark(buf, &insert_iter, gtk_text_buffer_get_insert(buf));
        
        GtkTextIter sel_start, sel_end;
        if (gtk_text_buffer_get_selection_bounds(buf, &sel_start, &sel_end)) {
            return FALSE;
        }

        gint line_num = gtk_text_iter_get_line(&insert_iter);
        GtkTextIter line_start;
        gtk_text_buffer_get_iter_at_line(buf, &line_start, line_num);
        
        gchar *line_text = gtk_text_buffer_get_text(buf, &line_start, &insert_iter, FALSE);
        
        int ws_len = 0;
        while (line_text[ws_len] == ' ' || line_text[ws_len] == '\t') {
            ws_len++;
        }
        
        char *content = line_text + ws_len;
        char *bullet_prefix = NULL;
        char prefix_buf[32] = "";
        
        if (strncmp(content, "- [ ] ", 6) == 0 || strncmp(content, "- [x] ", 6) == 0) {
            bullet_prefix = "- [ ] ";
        } else if (strncmp(content, "* [ ] ", 6) == 0 || strncmp(content, "* [x] ", 6) == 0) {
            bullet_prefix = "* [ ] ";
        } else if (strncmp(content, "- ", 2) == 0) {
            bullet_prefix = "- ";
        } else if (strncmp(content, "* ", 2) == 0) {
            bullet_prefix = "* ";
        } else if (strncmp(content, "+ ", 2) == 0) {
            bullet_prefix = "+ ";
        } else {
            int num_len = 0;
            while (content[num_len] >= '0' && content[num_len] <= '9') {
                num_len++;
            }
            if (num_len > 0 && content[num_len] == '.' && content[num_len+1] == ' ') {
                char num_str[16];
                if (num_len < 15) {
                    strncpy(num_str, content, num_len);
                    num_str[num_len] = '\0';
                    int val = atoi(num_str);
                    snprintf(prefix_buf, sizeof(prefix_buf), "%d. ", val + 1);
                    bullet_prefix = prefix_buf;
                }
            }
        }
        
        if (bullet_prefix != NULL) {
            gboolean is_empty_bullet = TRUE;
            char *p = content;
            if (strncmp(p, "- [ ] ", 6) == 0 || strncmp(p, "- [x] ", 6) == 0 || strncmp(p, "* [ ] ", 6) == 0 || strncmp(p, "* [x] ", 6) == 0) {
                is_empty_bullet = (p[6] == '\0' || p[6] == '\n' || p[6] == '\r');
            } else if (strncmp(p, "- ", 2) == 0 || strncmp(p, "* ", 2) == 0 || strncmp(p, "+ ", 2) == 0) {
                is_empty_bullet = (p[2] == '\0' || p[2] == '\n' || p[2] == '\r');
            } else {
                int num_len = 0;
                while (p[num_len] >= '0' && p[num_len] <= '9') num_len++;
                is_empty_bullet = (p[num_len+2] == '\0' || p[num_len+2] == '\n' || p[num_len+2] == '\r');
            }
            
            if (is_empty_bullet) {
                GtkTextIter line_end = insert_iter;
                gtk_text_buffer_delete(buf, &line_start, &line_end);
                gtk_text_buffer_insert(buf, &line_start, "\n", 1);
                g_free(line_text);
                return TRUE;
            }
            
            gchar *indent = g_strndup(line_text, ws_len);
            gchar *newline_and_bullet = g_strconcat("\n", indent, bullet_prefix, NULL);
            
            gtk_text_buffer_insert(buf, &insert_iter, newline_and_bullet, -1);
            
            g_free(indent);
            g_free(newline_and_bullet);
            g_free(line_text);
            return TRUE;
        }
        
        g_free(line_text);
    }
    return FALSE;
}

static void on_insert_text_after(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data) {
    (void)user_data;
    
    GtkTextIter end_iter = *location;
    GtkTextIter start_iter = end_iter;
    
    glong char_len = g_utf8_strlen(text, len);
    gtk_text_iter_backward_chars(&start_iter, char_len);
    
    gint start_line = gtk_text_iter_get_line(&start_iter);
    gint end_line = gtk_text_iter_get_line(&end_iter);
    
    for (gint l = start_line; l <= end_line; l++) {
        GtkTextIter line_iter;
        gtk_text_buffer_get_iter_at_line(buf, &line_iter, l);
        update_paragraph_direction(buf, &line_iter);
    }
    
    gui_set_sync_status("Not Synced");
    apply_wiki_link_tags(buf);
}

static void on_delete_range_after(GtkTextBuffer *buf, GtkTextIter *start, GtkTextIter *end, gpointer user_data) {
    (void)end; (void)user_data;
    
    update_paragraph_direction(buf, start);
    
    gui_set_sync_status("Not Synced");
    apply_wiki_link_tags(buf);
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

static void on_search_text_changed(GtkSearchEntry *entry, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (!gui->search_settings) return;
    gtk_source_search_settings_set_search_text(
        gui->search_settings,
        gtk_editable_get_text(GTK_EDITABLE(entry)));
    update_search_match_count(gui);
}

static void on_search_next_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    do_search_forward((AppGui *)user_data);
}

static void on_search_prev_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    do_search_backward((AppGui *)user_data);
}

static gboolean on_search_entry_key(GtkEventControllerKey *ctrl,
                                    guint keyval, guint keycode,
                                    GdkModifierType state, gpointer user_data) {
    (void)ctrl; (void)keycode; (void)state;
    AppGui *gui = (AppGui *)user_data;
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        do_search_forward(gui);
        return TRUE;
    }
    if (keyval == GDK_KEY_Escape) {
        toggle_search(gui);
        return TRUE;
    }
    return FALSE;
}

static void toggle_search(AppGui *gui) {
    gui->search_visible = !gui->search_visible;
    gtk_revealer_set_reveal_child(GTK_REVEALER(gui->search_revealer),
                                  gui->search_visible);
    if (gui->search_visible) {
        if (gui->btn_search) gtk_widget_add_css_class(gui->btn_search, "active");
        set_active_tab(gui, gui->btn_editor, "editor");
        if (gui->btn_editor) gtk_widget_add_css_class(gui->btn_editor, "active");
        gtk_widget_grab_focus(gui->search_entry);
    } else {
        if (gui->btn_search) gtk_widget_remove_css_class(gui->btn_search, "active");
        gtk_widget_grab_focus(gui->source_view);
    }
}

static void on_search_icon_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    toggle_search((AppGui *)user_data);
}

static void on_close_search_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppGui *gui = (AppGui *)user_data;
    if (gui->search_visible) toggle_search(gui);
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
    gtk_text_view_set_wrap_mode(
        GTK_TEXT_VIEW(user_data),
        gtk_check_button_get_active(btn) ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
}

static char current_en_font[64] = "JetBrains Mono";
static char current_ar_font[64] = "Amiri";
static double current_font_size = 16.0;

static void update_editor_font(AppGui *gui) {
    if (!gui->font_provider) {
        gui->font_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(),
            GTK_STYLE_PROVIDER(gui->font_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
    char css[1024];
    snprintf(css, sizeof(css),
        "textview text, textview.sourceview text, textview {\n"
        "    font-family: \"%s\", \"%s\", \"serif\";\n"
        "    font-size: %.0fpx;\n"
        "    line-height: 1.45;\n"
        "    caret-color: #00E5FF;\n"
        "}\n"
        "h1, h2, h3 {\n"
        "    font-family: \"Cairo\", \"Inter\", \"system-ui\", sans-serif;\n"
        "}",
        current_en_font, current_ar_font, current_font_size);
    gtk_css_provider_load_from_string(gui->font_provider, css);
}

static void on_font_size_changed(GtkSpinButton *spin, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    current_font_size = gtk_spin_button_get_value(spin);
    update_editor_font(gui);
}

static void on_en_font_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    GtkDropDown *dropdown = GTK_DROP_DOWN(gobject);
    guint selected = gtk_drop_down_get_selected(dropdown);
    const char *en_fonts[] = {
        "JetBrains Mono",
        "Inter",
        "Roboto",
        "Fira Code",
        "Source Code Pro"
    };
    if (selected < 5) {
        strncpy(current_en_font, en_fonts[selected], sizeof(current_en_font) - 1);
        current_en_font[sizeof(current_en_font) - 1] = '\0';
        update_editor_font((AppGui *)user_data);
    }
}

static void on_ar_font_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    GtkDropDown *dropdown = GTK_DROP_DOWN(gobject);
    guint selected = gtk_drop_down_get_selected(dropdown);
    const char *ar_fonts[] = {
        "Amiri",
        "Cairo",
        "IBM Plex Sans Arabic",
        "KFGQPC Uthman Taha Naskh",
        "Noto Naskh Arabic"
    };
    if (selected < 5) {
        strncpy(current_ar_font, ar_fonts[selected], sizeof(current_ar_font) - 1);
        current_ar_font[sizeof(current_ar_font) - 1] = '\0';
        update_editor_font((AppGui *)user_data);
    }
}


/* ============================================================
 * FILE EXPLORER — helpers
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

static void on_file_card_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    zig_open_file((const char *)user_data);
}

static int explorer_sort_func(GtkListBoxRow *row1, GtkListBoxRow *row2, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    const char *search_text = gtk_editable_get_text(GTK_EDITABLE(gui->exp_search_entry));
    if (search_text && strlen(search_text) > 0) {
        gpointer idx1 = g_object_get_data(G_OBJECT(row1), "search_index");
        gpointer idx2 = g_object_get_data(G_OBJECT(row2), "search_index");
        int i1 = GPOINTER_TO_INT(idx1);
        int i2 = GPOINTER_TO_INT(idx2);
        if (i1 >= 0 && i2 >= 0) {
            return (i1 < i2) ? -1 : ((i1 > i2) ? 1 : 0);
        }
        if (i1 >= 0) return -1;
        if (i2 >= 0) return 1;
        return 0;
    } else {
        gpointer m1 = g_object_get_data(G_OBJECT(row1), "mtime");
        gpointer m2 = g_object_get_data(G_OBJECT(row2), "mtime");
        int mtime1 = GPOINTER_TO_INT(m1);
        int mtime2 = GPOINTER_TO_INT(m2);
        if (mtime1 > mtime2) return -1;
        if (mtime1 < mtime2) return 1;
        return 0;
    }
}

static guint explorer_search_timeout_id = 0;

static gboolean do_debounced_explorer_search(gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    explorer_search_timeout_id = 0;

    const char *query = gtk_editable_get_text(GTK_EDITABLE(gui->exp_search_entry));
    if (query && strlen(query) > 0) {
        // Run FTS5 search (Zig backend)
        zig_search_workspace(query);

        // Filter and update rows
        GtkWidget *row = gtk_widget_get_first_child(gui->explorer_listbox);
        int match_count = 0;
        while (row) {
            GtkWidget *card = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
            if (card) {
                const char *filepath = g_object_get_data(G_OBJECT(card), "filepath");
                GtkWidget *snippet_lbl = g_object_get_data(G_OBJECT(card), "snippet_lbl");
                if (filepath) {
                    const char *snippet = zig_get_search_snippet(filepath);
                    if (snippet) {
                        match_count++;
                        gtk_widget_set_visible(row, TRUE);
                        if (snippet_lbl) {
                            gtk_label_set_markup(GTK_LABEL(snippet_lbl), snippet);
                        }
                        int rank = zig_get_search_rank(filepath);
                        g_object_set_data(G_OBJECT(row), "search_index", GINT_TO_POINTER(rank));
                    } else {
                        gtk_widget_set_visible(row, FALSE);
                        g_object_set_data(G_OBJECT(row), "search_index", GINT_TO_POINTER(-1));
                    }
                }
            }
            row = gtk_widget_get_next_sibling(row);
        }
        
        if (gui->exp_count_label) {
            char badge[64];
            snprintf(badge, sizeof(badge), "Found %d matches", match_count);
            gtk_label_set_text(GTK_LABEL(gui->exp_count_label), badge);
        }
    } else {
        // Clear search index / show all / restore default meta
        GtkWidget *row = gtk_widget_get_first_child(gui->explorer_listbox);
        int total_count = 0;
        while (row) {
            gtk_widget_set_visible(row, TRUE);
            g_object_set_data(G_OBJECT(row), "search_index", GINT_TO_POINTER(-1));
            
            GtkWidget *card = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
            if (card) {
                const char *default_meta = g_object_get_data(G_OBJECT(card), "default_meta");
                GtkWidget *snippet_lbl = g_object_get_data(G_OBJECT(card), "snippet_lbl");
                if (snippet_lbl && default_meta) {
                    gtk_label_set_text(GTK_LABEL(snippet_lbl), default_meta);
                }
                total_count++;
            }
            row = gtk_widget_get_next_sibling(row);
        }

        if (gui->exp_count_label) {
            char badge[64];
            snprintf(badge, sizeof(badge), "%d items", total_count);
            gtk_label_set_text(GTK_LABEL(gui->exp_count_label), badge);
        }
    }

    // Trigger ListBox resort
    gtk_list_box_invalidate_sort(GTK_LIST_BOX(gui->explorer_listbox));

    return G_SOURCE_REMOVE;
}

static void on_explorer_search_changed(GtkSearchEntry *entry, gpointer user_data) {
    (void)entry;
    if (explorer_search_timeout_id > 0) {
        g_source_remove(explorer_search_timeout_id);
    }
    explorer_search_timeout_id = g_timeout_add(150, do_debounced_explorer_search, user_data);
}

static void populate_explorer(AppGui *gui) {
    if (!gui || !gui->explorer_listbox || !GTK_IS_LIST_BOX(gui->explorer_listbox)) return;

    /* Clear */
    GtkWidget *c = gtk_widget_get_first_child(gui->explorer_listbox);
    while (c) {
        GtkWidget *nxt = gtk_widget_get_next_sibling(c);
        gtk_list_box_remove(GTK_LIST_BOX(gui->explorer_listbox), c);
        c = nxt;
    }

    // Default: list all directory files
    GError *err = NULL;
    GDir   *dir = g_dir_open(".", 0, &err);
    if (!dir) return;

    /* Count for badge */
    int count = 0;
    const char *nm;
    while ((nm = g_dir_read_name(dir)) != NULL)
        if (nm[0] != '.') count++;
    g_dir_rewind(dir);

    if (gui->exp_count_label) {
        char badge[32];
        snprintf(badge, sizeof(badge), "%d items", count);
        gtk_label_set_text(GTK_LABEL(gui->exp_count_label), badge);
    }

    while ((nm = g_dir_read_name(dir)) != NULL) {
        if (nm[0] == '.') continue;

        gboolean is_dir = g_file_test(nm, G_FILE_TEST_IS_DIR);
        gboolean is_md  = g_str_has_suffix(nm, ".md") || g_str_has_suffix(nm, ".txt") ||
                          g_str_has_suffix(nm, ".zig") || g_str_has_suffix(nm, ".zon") ||
                          g_str_has_suffix(nm, ".c") || g_str_has_suffix(nm, ".h");
        char    *cb     = g_strdup(nm);

        struct stat st;
        gboolean has_st = (stat(nm, &st) == 0);

        /* Card button */
        GtkWidget *card = gtk_button_new();
        gtk_widget_add_css_class(card, "explorer-card");

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_halign(vbox, GTK_ALIGN_START);

        /* Icon row */
        GtkWidget *icon_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

        const char *icon_name = is_dir ? "folder-symbolic" : "text-x-generic-symbolic";
        GtkWidget  *icon      = gtk_image_new_from_icon_name(icon_name);
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 18);
        if      (is_dir) gtk_widget_add_css_class(icon, "icon-folder");
        else if (is_md)  gtk_widget_add_css_class(icon, "icon-file-md");
        else             gtk_widget_add_css_class(icon, "icon-file");

        GtkWidget *type_badge = gtk_label_new(is_dir ? "Dir" : (is_md ? "md" : "file"));
        gtk_widget_add_css_class(type_badge, "explorer-badge");
        gtk_widget_set_hexpand(type_badge, TRUE);
        gtk_widget_set_halign(type_badge, GTK_ALIGN_END);

        gtk_box_append(GTK_BOX(icon_row), icon);
        gtk_box_append(GTK_BOX(icon_row), type_badge);

        /* Name */
        GtkWidget *name_lbl = gtk_label_new(nm);
        gtk_widget_add_css_class(name_lbl, "card-name");
        gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(GTK_LABEL(name_lbl), 18);
        gtk_widget_set_halign(name_lbl, GTK_ALIGN_START);

        /* Meta: size • time */
        char meta[64] = "";
        long long mtime = 0;
        if (has_st) {
            mtime = st.st_mtime;
            char sz[24] = "", ts[24] = "";
            if (!is_dir) format_size(st.st_size, sz, sizeof(sz));
            format_mtime(st.st_mtime, ts, sizeof(ts));
            if (!is_dir) snprintf(meta, sizeof(meta), "%s • %s", sz, ts);
            else         snprintf(meta, sizeof(meta), "%s", ts);
        }
        GtkWidget *meta_lbl = gtk_label_new(meta);
        gtk_widget_add_css_class(meta_lbl, "card-meta");
        gtk_widget_set_halign(meta_lbl, GTK_ALIGN_START);

        gtk_box_append(GTK_BOX(vbox), icon_row);
        gtk_box_append(GTK_BOX(vbox), name_lbl);
        gtk_box_append(GTK_BOX(vbox), meta_lbl);
        gtk_button_set_child(GTK_BUTTON(card), vbox);

        g_signal_connect_data(card, "clicked",
                              G_CALLBACK(on_file_card_clicked),
                              cb, (GClosureNotify)g_free, 0);

        /* Row wrapper */
        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), card);
        
        // Store metadata on the card/row
        g_object_set_data_full(G_OBJECT(card), "filepath", g_strdup(nm), g_free);
        g_object_set_data_full(G_OBJECT(card), "default_meta", g_strdup(meta), g_free);
        g_object_set_data(G_OBJECT(card), "snippet_lbl", meta_lbl);
        
        g_object_set_data(G_OBJECT(row), "mtime", GINT_TO_POINTER((int)mtime));
        g_object_set_data(G_OBJECT(row), "search_index", GINT_TO_POINTER(-1));
        
        gtk_list_box_append(GTK_LIST_BOX(gui->explorer_listbox), row);
    }
    g_dir_close(dir);

    // Sort initially by mtime
    gtk_list_box_invalidate_sort(GTK_LIST_BOX(gui->explorer_listbox));

    // If search is currently active, re-filter immediately
    const char *search_text = gtk_editable_get_text(GTK_EDITABLE(gui->exp_search_entry));
    if (search_text && strlen(search_text) > 0) {
        do_debounced_explorer_search(gui);
    }
}

/* ============================================================
 * BUILD UI
 * ============================================================ */

static gboolean on_window_key_pressed(GtkEventControllerKey *ctrl,
                                      guint keyval, guint keycode,
                                      GdkModifierType state, gpointer user_data) {
    (void)ctrl;
    AppGui *gui = (AppGui *)user_data;

    if ((state & GDK_CONTROL_MASK) && (keyval == GDK_KEY_f || keyval == GDK_KEY_F || keycode_matches_latin_keyval(keycode, GDK_KEY_f))) {
        toggle_search(gui);
        return TRUE;
    }

    if (keyval == GDK_KEY_Escape && gui->search_visible) {
        toggle_search(gui);
        return TRUE;
    }

    /* Ctrl+? — Keybindings reference window */
    if ((state & GDK_CONTROL_MASK) && keyval == GDK_KEY_question) {
        show_keybindings_window(gui);
        return TRUE;
    }

    /* Ctrl+Q — Quit application */
    if ((state & GDK_CONTROL_MASK) && (keyval == GDK_KEY_q || keyval == GDK_KEY_Q || keycode_matches_latin_keyval(keycode, GDK_KEY_q))) {
        g_application_quit(g_application_get_default());
        return TRUE;
    }

    /* Ctrl+comma — Settings */
    if ((state & GDK_CONTROL_MASK) && keyval == GDK_KEY_comma) {
        on_settings_btn_clicked(NULL, gui);
        return TRUE;
    }

    /* Ctrl+backslash — Toggle Sidebar */
    if ((state & GDK_CONTROL_MASK) && keyval == GDK_KEY_backslash) {
        on_logo_clicked(NULL, gui);
        return TRUE;
    }

    return FALSE;
}

static void on_save_credentials_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppGui *gui = (AppGui *)user_data;
    const char *client_id = gtk_editable_get_text(GTK_EDITABLE(gui->client_id_entry));
    const char *client_secret = gtk_editable_get_text(GTK_EDITABLE(gui->client_secret_entry));

    if ((!client_id || strlen(client_id) == 0) && (!client_secret || strlen(client_secret) == 0)) {
        /* Both fields empty — warn the user */
        if (gui->sync_status_lbl)
            gtk_label_set_text(GTK_LABEL(gui->sync_status_lbl), "Enter credentials first");
        return;
    }

    zig_save_sync_credentials(client_id, client_secret);

    /* Give visual confirmation that the save succeeded */
    if (gui->sync_status_lbl)
        gtk_label_set_text(GTK_LABEL(gui->sync_status_lbl), "Credentials saved ✓");
}

static void on_sync_connect_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppGui *gui = (AppGui *)user_data;
    int connected = zig_sync_check_status();
    if (connected) {
        zig_sync_disconnect();
    } else {
        zig_sync_connect();
    }
}

static void on_sync_submit_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppGui *gui = (AppGui *)user_data;
    const char *code = gtk_editable_get_text(GTK_EDITABLE(gui->sync_code_entry));
    if (code && strlen(code) > 0) {
        zig_sync_submit_code(code);
    }
}

static void on_sync_now_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    zig_sync_now();
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
    gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    return TRUE;
}

static void load_sync_credentials(AppGui *gui) {
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }
    const char *sql = "SELECT client_id, client_secret FROM sync_tokens WHERE id = 1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *c_id = (const char *)sqlite3_column_text(stmt, 0);
            const char *c_sec = (const char *)sqlite3_column_text(stmt, 1);
            if (c_id && gui->client_id_entry) {
                gtk_editable_set_text(GTK_EDITABLE(gui->client_id_entry), c_id);
            }
            if (c_sec && gui->client_secret_entry) {
                gtk_editable_set_text(GTK_EDITABLE(gui->client_secret_entry), c_sec);
            }
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
}

static void on_theme_dark_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    apply_theme((AppGui *)user_data, "dark");
}

static void on_theme_sepia_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    apply_theme((AppGui *)user_data, "sepia");
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

static void on_scroll_changed(GtkAdjustment *adj, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (gui->in_scroll_update) return;

    double value = gtk_adjustment_get_value(adj);
    double page_size = gtk_adjustment_get_page_size(adj);
    
    int line_h = get_line_height(gui->source_view);
    if (line_h <= 0) line_h = 24;
    
    int view_start_line = (int)(value / line_h);
    int view_lines = (int)(page_size / line_h);
    int view_end_line = view_start_line + view_lines;
    
    gboolean need_update = FALSE;
    if (view_start_line < gui->active_page_start_line + 10 && gui->active_page_start_line > 0) {
        need_update = TRUE;
    } else if (view_end_line > gui->active_page_end_line - 10 && gui->active_page_end_line < gui->total_virtual_lines) {
        need_update = TRUE;
    }
    
    if (need_update) {
        gui->in_scroll_update = TRUE;
        
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
        GtkTextIter start_iter, end_iter;
        gtk_text_buffer_get_bounds(buf, &start_iter, &end_iter);
        char *page_text = gtk_text_buffer_get_text(buf, &start_iter, &end_iter, FALSE);
        
        extern int zig_save_active_page(int start_line, int end_line, const char *text);
        int status = zig_save_active_page(gui->active_page_start_line, gui->active_page_end_line, page_text);
        g_free(page_text);
        (void)status;
        
        int center_line = (view_start_line + view_end_line) / 2;
        int new_start = center_line - 50;
        if (new_start < 0) new_start = 0;
        int new_end = new_start + 100;
        if (new_end > gui->total_virtual_lines) {
            new_end = gui->total_virtual_lines;
            new_start = new_end - 100;
            if (new_start < 0) new_start = 0;
        }
        
        extern const char *zig_get_text_for_line_range(int start_line, int end_line, int *out_len);
        int new_len = 0;
        const char *new_text = zig_get_text_for_line_range(new_start, new_end, &new_len);
        
        extern void on_insert_text_before(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data);
        extern void on_insert_text_after(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data);
        extern void on_delete_range_after(GtkTextBuffer *buf, GtkTextIter *start, GtkTextIter *end, gpointer user_data);
        
        g_signal_handlers_block_by_func(buf, on_insert_text_before, gui);
        g_signal_handlers_block_by_func(buf, on_insert_text_after, gui);
        g_signal_handlers_block_by_func(buf, on_delete_range_after, gui);
        
        gtk_text_buffer_set_text(buf, new_text ? new_text : "", new_len);
        
        g_signal_handlers_unblock_by_func(buf, on_insert_text_before, gui);
        g_signal_handlers_unblock_by_func(buf, on_insert_text_after, gui);
        g_signal_handlers_unblock_by_func(buf, on_delete_range_after, gui);
        
        update_all_paragraphs_direction(buf);
        extern void apply_wiki_link_tags(GtkTextBuffer *buf);
        apply_wiki_link_tags(buf);
        
        gui->active_page_start_line = new_start;
        gui->active_page_end_line = new_end;
        
        int top_h = new_start * line_h;
        int bottom_h = (gui->total_virtual_lines - new_end) * line_h;
        if (top_h < 0) top_h = 0;
        if (bottom_h < 0) bottom_h = 0;
        gtk_widget_set_size_request(gui->top_spacer, -1, top_h);
        gtk_widget_set_size_request(gui->bottom_spacer, -1, bottom_h);
        
        gtk_widget_queue_resize(gui->virtual_layout_box);
        
        gui->in_scroll_update = FALSE;
    }
}

static void on_theme_midnight_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    apply_theme((AppGui *)user_data, "midnight");
}

static void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    g_object_set(gtk_settings_get_default(), 
                 "gtk-cursor-blink", FALSE, 
                 "gtk-cursor-aspect-ratio", 0.02, 
                 NULL);

    /* ── Window ── */
    GtkWidget *window = adw_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Qirtas");
    gtk_window_set_default_size(GTK_WINDOW(window), 1100, 720);
    gtk_widget_set_size_request(window, 350, 250);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    AppGui *gui = g_new0(AppGui, 1);
    gui->window       = window;
    gui->search_visible = FALSE;
    gui->font_provider  = NULL;
    gui->css_provider   = NULL;

    init_css(gui);

    GtkEventController *win_key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(win_key_ctrl, "key-pressed",
                     G_CALLBACK(on_window_key_pressed), gui);
    gtk_widget_add_controller(window, win_key_ctrl);
    g_signal_connect(window, "close-request", G_CALLBACK(on_window_close_request), gui);

    /*
     * AdwApplicationWindow does NOT support gtk_window_set_titlebar().
     * The correct pattern is AdwToolbarView:
     *
     *   AdwApplicationWindow
     *     └─ AdwToolbarView
     *          ├─ [top] AdwHeaderBar   (CSD close/min/max)
     *          └─ [content] root_box   (sidebar + stack)
     */
    GtkWidget *toolbar_view = adw_toolbar_view_new();
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), toolbar_view);

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
    gtk_icon_theme_add_search_path(icon_theme, "/home/sinkeat/projects/qirtas/src/ui/icons");
    if (strlen(custom_icon_path) > 0) {
        char resolved_path[PATH_MAX];
        if (realpath(custom_icon_path, resolved_path) != NULL) {
            gtk_icon_theme_add_search_path(icon_theme, resolved_path);
        } else {
            gtk_icon_theme_add_search_path(icon_theme, custom_icon_path);
        }
    }

    AddPopoverWidgets *w = g_new0(AddPopoverWidgets, 1);
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

    GtkWidget *btn_pdf = gtk_button_new_with_label("تصدير كـ PDF");
    gtk_widget_add_css_class(btn_pdf, "pop-btn");
    g_signal_connect(btn_pdf, "clicked", G_CALLBACK(on_export_pdf_clicked), gui);
    gtk_box_append(GTK_BOX(w->box_actions), btn_pdf);

    w->box_input = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_visible(w->box_input, FALSE);
    gtk_widget_set_margin_start(w->box_input, 8);
    gtk_widget_set_margin_end(w->box_input, 8);
    gtk_widget_set_margin_top(w->box_input, 8);
    gtk_widget_set_margin_bottom(w->box_input, 8);

    GtkWidget *lbl_prompt = gtk_label_new("Enter file name:");
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

    /* ── Root box: sidebar | stack ── */
    GtkWidget *overlay = gtk_overlay_new();
    gtk_widget_set_vexpand(overlay, TRUE);
    gtk_widget_set_hexpand(overlay, TRUE);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), overlay);
    gui->root_box = overlay;

    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(sidebar, "sidebar");
    gtk_widget_set_size_request(sidebar, 280, -1); /* fixed 280px */
    gtk_widget_set_visible(sidebar, FALSE); /* not visible on app open */
    gtk_widget_set_halign(sidebar, GTK_ALIGN_START);
    gtk_widget_set_valign(sidebar, GTK_ALIGN_FILL);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), sidebar);
    gui->sidebar = sidebar;

    /* 1. Global Workspace Search Entry */
    GtkWidget *exp_search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(exp_search), "Search in workspace...");
    gtk_box_append(GTK_BOX(sidebar), exp_search);
    gui->exp_search_entry = exp_search;
    g_signal_connect(exp_search, "search-changed", G_CALLBACK(on_explorer_search_changed), gui);

    /* 2. Unified Add Note Button (Menu button with popover) */
    GtkWidget *add_action_button = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(add_action_button), "New Note");
    gtk_widget_add_css_class(add_action_button, "add-action-btn");
    gtk_widget_set_hexpand(add_action_button, TRUE);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(add_action_button), w->popover);
    gtk_box_append(GTK_BOX(sidebar), add_action_button);

    /* Notes section header (Title + Count Badge) */
    GtkWidget *exp_title_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(exp_title_row, 12);
    gtk_widget_set_margin_bottom(exp_title_row, 4);

    GtkWidget *exp_title = gtk_label_new("Notes");
    gtk_widget_add_css_class(exp_title, "explorer-title");
    gtk_widget_set_halign(exp_title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(exp_title_row), exp_title);

    gui->exp_count_label = gtk_label_new("0 items");
    gtk_widget_add_css_class(gui->exp_count_label, "explorer-badge");
    gtk_widget_set_halign(gui->exp_count_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(exp_title_row), gui->exp_count_label);

    gtk_box_append(GTK_BOX(sidebar), exp_title_row);

    /* 3. Scrolled Window & Explorer ListBox for Notes List */
    GtkWidget *exp_scroll = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(exp_scroll, TRUE);
    gtk_widget_set_vexpand(exp_scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(exp_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(sidebar), exp_scroll);

    GtkWidget *listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_NONE);
    gtk_list_box_set_sort_func(GTK_LIST_BOX(listbox), explorer_sort_func, gui, NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(exp_scroll), listbox);
    gui->explorer_listbox = listbox;

    /* 4. Bottom nav group (Settings) */
    GtkWidget *nav_bot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign(nav_bot, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(nav_bot, 4);
    gtk_box_append(GTK_BOX(sidebar), nav_bot);

    gui->btn_stats = NULL;

    gui->btn_settings = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(gui->btn_settings),
        gtk_image_new_from_icon_name("preferences-system-symbolic"));
    gtk_widget_add_css_class(gui->btn_settings, "nav-btn");
    gtk_widget_set_tooltip_text(gui->btn_settings, "Settings (Ctrl+,)");
    gtk_box_append(GTK_BOX(nav_bot), gui->btn_settings);

    /* ============================================================
     * STACK
     * ============================================================ */
    GtkWidget *stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(stack), 180);
    gtk_widget_set_hexpand(stack, TRUE);
    gtk_widget_set_vexpand(stack, TRUE);
    gtk_widget_add_css_class(stack, "workspace");
    gtk_overlay_set_child(GTK_OVERLAY(overlay), stack);
    gui->stack = stack;

    GtkGesture *workspace_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(workspace_click), GDK_BUTTON_PRIMARY);
    g_signal_connect(workspace_click, "pressed", G_CALLBACK(on_workspace_click), gui);
    gtk_widget_add_controller(stack, GTK_EVENT_CONTROLLER(workspace_click));

    /* ============================================================
     * PAGE 1 — EDITOR
     * ============================================================ */
    GtkWidget *editor_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_stack_add_named(GTK_STACK(stack), editor_page, "editor");
    gui->editor_page = editor_page;

    /* Search revealer (configured to show at bottom, slide up) */
    GtkWidget *search_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(search_revealer),
                                      GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_transition_duration(GTK_REVEALER(search_revealer), 200);
    gtk_revealer_set_reveal_child(GTK_REVEALER(search_revealer), FALSE);
    gui->search_revealer = search_revealer;

    GtkWidget *sbar_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(sbar_box, "search-bar-box");
    gtk_revealer_set_child(GTK_REVEALER(search_revealer), sbar_box);

    GtkWidget *sbar_icon = gtk_image_new_from_icon_name("system-search-symbolic");
    gtk_box_append(GTK_BOX(sbar_box), sbar_icon);

    GtkWidget *search_entry = gtk_search_entry_new();
    gtk_widget_add_css_class(search_entry, "search-entry");
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search_entry),
                                           "Search in file…");
    gtk_box_append(GTK_BOX(sbar_box), search_entry);
    gui->search_entry = search_entry;

    GtkWidget *match_lbl = gtk_label_new("");
    gtk_widget_add_css_class(match_lbl, "search-match-label");
    gtk_box_append(GTK_BOX(sbar_box), match_lbl);
    gui->search_match_label = match_lbl;

    GtkWidget *btn_prev = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_add_css_class(btn_prev, "search-nav-btn");
    gtk_widget_set_tooltip_text(btn_prev, "Previous match");
    gtk_box_append(GTK_BOX(sbar_box), btn_prev);

    GtkWidget *btn_next = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_add_css_class(btn_next, "search-nav-btn");
    gtk_widget_set_tooltip_text(btn_next, "Next match");
    gtk_box_append(GTK_BOX(sbar_box), btn_next);

    GtkWidget *btn_close_s = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(btn_close_s, "search-nav-btn");
    gtk_widget_set_tooltip_text(btn_close_s, "Close (Esc)");
    gtk_widget_set_hexpand(btn_close_s, TRUE);
    gtk_widget_set_halign(btn_close_s, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(sbar_box), btn_close_s);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(scrolled, TRUE);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_widget_set_halign(scrolled, GTK_ALIGN_FILL);
    gtk_widget_set_valign(scrolled, GTK_ALIGN_FILL);
    gtk_widget_set_margin_start(scrolled, 12);
    gtk_widget_set_margin_end(scrolled, 12);
    gtk_widget_set_margin_top(scrolled, 12);
    gtk_widget_set_margin_bottom(scrolled, 12);
    gtk_widget_add_css_class(scrolled, "editor-scroll");
    gtk_box_append(GTK_BOX(editor_page), scrolled);

    /* Append search bar after content so it sits at the bottom */
    gtk_box_append(GTK_BOX(editor_page), search_revealer);

    GtkWidget *virtual_layout_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gui->virtual_layout_box = virtual_layout_box;

    gui->top_spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(gui->top_spacer, FALSE);
    gtk_widget_set_valign(gui->top_spacer, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(virtual_layout_box), gui->top_spacer);

    GtkWidget *source_view = gtk_source_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(source_view), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_hexpand(source_view, TRUE);
    gtk_widget_set_vexpand(source_view, TRUE);
    gtk_widget_set_halign(source_view, GTK_ALIGN_FILL);
    gtk_widget_set_valign(source_view, GTK_ALIGN_FILL);
    gtk_widget_set_margin_start(source_view, 16);
    gtk_widget_set_margin_end(source_view, 16);
    gtk_widget_set_margin_top(source_view, 16);
    gtk_widget_set_margin_bottom(source_view, 320);
    gtk_box_append(GTK_BOX(virtual_layout_box), source_view);
    gui->source_view = source_view;

    gui->bottom_spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(gui->bottom_spacer, FALSE);
    gtk_widget_set_valign(gui->bottom_spacer, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(virtual_layout_box), gui->bottom_spacer);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), virtual_layout_box);

    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
    gui->vadjustment = vadj;
    g_signal_connect(vadj, "value-changed", G_CALLBACK(on_scroll_changed), gui);

    /* Typography & Render spacing (line height equivalent) */
    gtk_text_view_set_pixels_above_lines(GTK_TEXT_VIEW(source_view), 6);
    gtk_text_view_set_pixels_below_lines(GTK_TEXT_VIEW(source_view), 6);
    gtk_text_view_set_pixels_inside_wrap(GTK_TEXT_VIEW(source_view), 6);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(source_view), 150);


    /* Highlight Current Line & Show Line Numbers */
    gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(source_view), TRUE);
    gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(source_view), TRUE);

    /* Markdown syntax highlighting */
    GtkSourceBuffer *src_buf =
        GTK_SOURCE_BUFFER(gtk_text_view_get_buffer(GTK_TEXT_VIEW(source_view)));
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
    gui->search_settings = ss;
    gui->search_ctx = gtk_source_search_context_new(src_buf, ss);

    /* Wire search and text buffer direction signals */
    g_signal_connect(src_buf, "insert-text", G_CALLBACK(on_insert_text_before), gui);
    g_signal_connect_after(src_buf, "insert-text", G_CALLBACK(on_insert_text_after), gui);
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



    /* ============================================================
     * SETTINGS WINDOW
     * ============================================================ */
    GtkWidget *pop_box  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(pop_box, 14);
    gtk_widget_set_margin_end(pop_box, 14);
    gtk_widget_set_margin_top(pop_box, 14);
    gtk_widget_set_margin_bottom(pop_box, 14);

    GtkWidget *th_lbl = gtk_label_new("THEME");
    gtk_widget_add_css_class(th_lbl, "pop-section-label");
    gtk_widget_set_halign(th_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pop_box), th_lbl);

    GtkWidget *th_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(th_row), TRUE);
    GtkWidget *btn_dark  = gtk_button_new_with_label("Deep Slate");
    GtkWidget *btn_sepia = gtk_button_new_with_label("Classic Sepia");
    GtkWidget *btn_mid   = gtk_button_new_with_label("Midnight");
    gtk_widget_add_css_class(btn_dark,  "pop-btn");
    gtk_widget_add_css_class(btn_sepia, "pop-btn");
    gtk_widget_add_css_class(btn_mid,   "pop-btn");
    gtk_box_append(GTK_BOX(th_row), btn_dark);
    gtk_box_append(GTK_BOX(th_row), btn_sepia);
    gtk_box_append(GTK_BOX(th_row), btn_mid);
    gtk_box_append(GTK_BOX(pop_box), th_row);
    g_signal_connect(btn_dark,  "clicked", G_CALLBACK(on_theme_dark_clicked),  gui);
    g_signal_connect(btn_sepia, "clicked", G_CALLBACK(on_theme_sepia_clicked), gui);
    g_signal_connect(btn_mid,   "clicked", G_CALLBACK(on_theme_midnight_clicked), gui);

    gtk_box_append(GTK_BOX(pop_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *pr_lbl = gtk_label_new("PREFERENCES");
    gtk_widget_add_css_class(pr_lbl, "pop-section-label");
    gtk_widget_set_halign(pr_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pop_box), pr_lbl);

    GtkWidget *wrap_chk = gtk_check_button_new_with_label("Word Wrap");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(wrap_chk), TRUE);
    g_signal_connect(wrap_chk, "toggled", G_CALLBACK(on_wrap_toggled), source_view);
    gtk_box_append(GTK_BOX(pop_box), wrap_chk);

    GtkWidget *font_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *font_lbl = gtk_label_new("Font Size");
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
    GtkWidget *en_font_lbl = gtk_label_new("English Font");
    gtk_widget_set_hexpand(en_font_lbl, TRUE);
    gtk_widget_set_halign(en_font_lbl, GTK_ALIGN_START);
    const char *en_fonts[] = {
        "JetBrains Mono",
        "Inter",
        "Roboto",
        "Fira Code",
        "Source Code Pro",
        NULL
    };
    GtkWidget *en_font_dropdown = gtk_drop_down_new_from_strings(en_fonts);
    int en_idx = 0;
    for (int i = 0; i < 5; i++) {
        if (strcmp(current_en_font, en_fonts[i]) == 0) {
            en_idx = i;
            break;
        }
    }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(en_font_dropdown), en_idx);
    g_signal_connect(en_font_dropdown, "notify::selected", G_CALLBACK(on_en_font_changed), gui);
    gtk_box_append(GTK_BOX(en_font_row), en_font_lbl);
    gtk_box_append(GTK_BOX(en_font_row), en_font_dropdown);
    gtk_box_append(GTK_BOX(pop_box), en_font_row);

    // Arabic Font Selection
    GtkWidget *ar_font_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *ar_font_lbl = gtk_label_new("Arabic Font");
    gtk_widget_set_hexpand(ar_font_lbl, TRUE);
    gtk_widget_set_halign(ar_font_lbl, GTK_ALIGN_START);
    const char *ar_fonts[] = {
        "Amiri",
        "Cairo",
        "IBM Plex Sans Arabic",
        "KFGQPC Uthman Taha Naskh",
        "Noto Naskh Arabic",
        NULL
    };
    GtkWidget *ar_font_dropdown = gtk_drop_down_new_from_strings(ar_fonts);
    int ar_idx = 0;
    for (int i = 0; i < 5; i++) {
        if (strcmp(current_ar_font, ar_fonts[i]) == 0) {
            ar_idx = i;
            break;
        }
    }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(ar_font_dropdown), ar_idx);
    g_signal_connect(ar_font_dropdown, "notify::selected", G_CALLBACK(on_ar_font_changed), gui);
    gtk_box_append(GTK_BOX(ar_font_row), ar_font_lbl);
    gtk_box_append(GTK_BOX(ar_font_row), ar_font_dropdown);
    gtk_box_append(GTK_BOX(pop_box), ar_font_row);


    gtk_box_append(GTK_BOX(pop_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *st_lbl = gtk_label_new("SESSION STATS");
    gtk_widget_add_css_class(st_lbl, "pop-section-label");
    gtk_widget_set_halign(st_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pop_box), st_lbl);

    GtkWidget *stats_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(stats_grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(stats_grid), 24);
    gtk_box_append(GTK_BOX(pop_box), stats_grid);

    struct { const char *label; GtkWidget **val; const char *def; } rows[] = {
        { "Session Duration", &gui->stats_time_val, "0 seconds"          },
        { "Active File",      &gui->stats_file_val, "Economics_Notes.md" },
        { "Engine",           NULL,                 "Zig 0.16 + GTK4"    },
        { "Syntax Engine",    NULL,                 "GtkSourceView 5"    },
    };
    for (int i = 0; i < 4; i++) {
        GtkWidget *lbl = gtk_label_new(rows[i].label);
        gtk_widget_add_css_class(lbl, "stats-label");
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        GtkWidget *val = gtk_label_new(rows[i].def);
        gtk_widget_add_css_class(val, "stats-value");
        gtk_widget_set_halign(val, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(stats_grid), lbl, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(stats_grid), val, 1, i, 1, 1);
        if (rows[i].val) *rows[i].val = val;
    }

    gtk_box_append(GTK_BOX(pop_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    /* ── KEYBOARD SHORTCUTS (inline table) ── */
    GtkWidget *kb_lbl = gtk_label_new("KEYBOARD SHORTCUTS");
    gtk_widget_add_css_class(kb_lbl, "pop-section-label");
    gtk_widget_set_halign(kb_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pop_box), kb_lbl);

    /* Open full reference button */
    GtkWidget *kb_open_btn = gtk_button_new_with_label("Open Full Reference  (Ctrl+?)");
    gtk_widget_add_css_class(kb_open_btn, "pop-btn");
    gtk_widget_set_halign(kb_open_btn, GTK_ALIGN_START);
    g_signal_connect_swapped(kb_open_btn, "clicked", G_CALLBACK(show_keybindings_window), gui);
    gtk_box_append(GTK_BOX(pop_box), kb_open_btn);

    /* Quick summary grid */
    GtkWidget *kb_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(kb_grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(kb_grid), 12);
    gtk_box_append(GTK_BOX(pop_box), kb_grid);

    struct { const char *key; const char *desc; } kb_rows[] = {
        { "Ctrl+B",       "Bold"              },
        { "Ctrl+I",       "Italic"            },
        { "Ctrl+K",       "Inline Code"       },
        { "Ctrl+H",       "Highlight"         },
        { "Ctrl+⇧S",      "Strikethrough"     },
        { "Ctrl+Q",       "Blockquote"        },
        { "Ctrl+M",       "Math"              },
        { "Ctrl+1…6",     "Heading H1–H6"     },
        { "Ctrl+0",       "Body (no prefix)"  },
        { "Ctrl+⇧L",      "Bullet List"       },
        { "Ctrl+⇧O",      "Numbered List"     },
        { "Ctrl+⇧T",      "Task Checkbox"     },
        { "Ctrl+F",       "Search"            },
        { "Ctrl+S",       "Force Save"        },
    };
    for (int i = 0; i < 14; i++) {
        GtkWidget *k = gtk_label_new(kb_rows[i].key);
        gtk_widget_add_css_class(k, "kb-key");
        gtk_widget_set_halign(k, GTK_ALIGN_START);
        GtkWidget *d = gtk_label_new(kb_rows[i].desc);
        gtk_widget_add_css_class(d, "kb-desc");
        gtk_widget_set_halign(d, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(kb_grid), k, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(kb_grid), d, 1, i, 1, 1);
    }

    // Sync & Cloud Layout
    gtk_box_append(GTK_BOX(pop_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *sync_lbl = gtk_label_new("SYNC & CLOUD");
    gtk_widget_add_css_class(sync_lbl, "pop-section-label");
    gtk_widget_set_halign(sync_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pop_box), sync_lbl);

    // Grid for credentials
    GtkWidget *creds_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(creds_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(creds_grid), 10);
    gtk_box_append(GTK_BOX(pop_box), creds_grid);

    GtkWidget *cid_lbl = gtk_label_new("Client ID");
    gtk_widget_add_css_class(cid_lbl, "stats-label");
    gtk_widget_set_halign(cid_lbl, GTK_ALIGN_START);
    GtkWidget *cid_entry = gtk_entry_new();
    gtk_widget_add_css_class(cid_entry, "pop-entry");
    gtk_entry_set_placeholder_text(GTK_ENTRY(cid_entry), "Google Client ID");
    gtk_widget_set_hexpand(cid_entry, TRUE);
    gtk_grid_attach(GTK_GRID(creds_grid), cid_lbl, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(creds_grid), cid_entry, 1, 0, 1, 1);
    gui->client_id_entry = cid_entry;

    GtkWidget *sec_lbl = gtk_label_new("Client Secret");
    gtk_widget_add_css_class(sec_lbl, "stats-label");
    gtk_widget_set_halign(sec_lbl, GTK_ALIGN_START);
    GtkWidget *sec_entry = gtk_entry_new();
    gtk_widget_add_css_class(sec_entry, "pop-entry");
    gtk_entry_set_placeholder_text(GTK_ENTRY(sec_entry), "Google Client Secret");
    gtk_widget_set_hexpand(sec_entry, TRUE);
    gtk_grid_attach(GTK_GRID(creds_grid), sec_lbl, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(creds_grid), sec_entry, 1, 1, 1, 1);
    gui->client_secret_entry = sec_entry;

    GtkWidget *save_creds_btn = gtk_button_new_with_label("Save Credentials");
    gtk_widget_add_css_class(save_creds_btn, "pop-btn");
    gtk_grid_attach(GTK_GRID(creds_grid), save_creds_btn, 1, 2, 1, 1);
    g_signal_connect(save_creds_btn, "clicked", G_CALLBACK(on_save_credentials_clicked), gui);

    // Sync Actions Box / Card
    GtkWidget *sync_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(sync_card, "sync-card");
    gtk_box_append(GTK_BOX(pop_box), sync_card);

    // Row 1: status & sync button
    GtkWidget *status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *sync_status_label_text = gtk_label_new("Google Drive:");
    gtk_widget_add_css_class(sync_status_label_text, "stats-label");
    gui->sync_status_lbl = gtk_label_new("Disconnected");
    gtk_widget_add_css_class(gui->sync_status_lbl, "stats-value");
    gtk_widget_set_hexpand(gui->sync_status_lbl, TRUE);
    gtk_widget_set_halign(gui->sync_status_lbl, GTK_ALIGN_START);

    gui->sync_connect_btn = gtk_button_new_with_label("Connect Account");
    gtk_widget_add_css_class(gui->sync_connect_btn, "pop-btn");
    g_signal_connect(gui->sync_connect_btn, "clicked", G_CALLBACK(on_sync_connect_clicked), gui);

    gtk_box_append(GTK_BOX(status_row), sync_status_label_text);
    gtk_box_append(GTK_BOX(status_row), gui->sync_status_lbl);
    gtk_box_append(GTK_BOX(status_row), gui->sync_connect_btn);
    gtk_box_append(GTK_BOX(sync_card), status_row);

    // Row 2: Code Submission (visible only during connection handshake)
    gui->sync_code_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_visible(gui->sync_code_box, FALSE);
    gui->sync_code_entry = gtk_entry_new();
    gtk_widget_add_css_class(gui->sync_code_entry, "pop-entry");
    gtk_entry_set_placeholder_text(GTK_ENTRY(gui->sync_code_entry), "Paste Authorization Code Here");
    gtk_widget_set_hexpand(gui->sync_code_entry, TRUE);
    gui->sync_submit_btn = gtk_button_new_with_label("Submit Code");
    gtk_widget_add_css_class(gui->sync_submit_btn, "pop-btn");
    g_signal_connect(gui->sync_submit_btn, "clicked", G_CALLBACK(on_sync_submit_clicked), gui);

    gtk_box_append(GTK_BOX(gui->sync_code_box), gui->sync_code_entry);
    gtk_box_append(GTK_BOX(gui->sync_code_box), gui->sync_submit_btn);
    gtk_box_append(GTK_BOX(sync_card), gui->sync_code_box);

    // Row 3: Sync now button
    gui->sync_now_btn = gtk_button_new_with_label("Sync Now");
    gtk_widget_add_css_class(gui->sync_now_btn, "sync-now-btn");
    gtk_widget_set_sensitive(gui->sync_now_btn, FALSE); // default disabled until connected
    g_signal_connect(gui->sync_now_btn, "clicked", G_CALLBACK(on_sync_now_clicked), gui);
    gtk_box_append(GTK_BOX(sync_card), gui->sync_now_btn);

    GtkWidget *pop_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(pop_scroll), 340);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(pop_scroll), 450);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(pop_scroll), TRUE);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(pop_scroll), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pop_scroll), pop_box);

    GtkWidget *settings_win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(settings_win), "Settings");
    gtk_window_set_default_size(GTK_WINDOW(settings_win), 400, 550);
    gtk_window_set_transient_for(GTK_WINDOW(settings_win), GTK_WINDOW(window));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(settings_win), TRUE);
    gtk_window_set_child(GTK_WINDOW(settings_win), pop_scroll);
    g_signal_connect(settings_win, "close-request", G_CALLBACK(on_settings_window_close_request), NULL);

    gui->settings_window = settings_win;

    /* ============================================================
     * WIRE ALL SIGNALS
     * ============================================================ */
    if (gui->btn_editor) g_signal_connect(gui->btn_editor,   "clicked", G_CALLBACK(on_editor_clicked), gui);
    if (gui->btn_files)    g_signal_connect(gui->btn_files,    "clicked", G_CALLBACK(on_files_clicked),  gui);
    if (gui->btn_settings) g_signal_connect(gui->btn_settings, "clicked", G_CALLBACK(on_settings_btn_clicked), gui);

    // Track text changes to update word/character counts
    g_signal_connect(src_buf, "changed", G_CALLBACK(on_buffer_changed), gui);

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

    /* ============================================================
     * BOTTOM BAR
     * ============================================================ */
    GtkWidget *bottom_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(bottom_bar, "bottom-bar");

    /* ── Far bottom-left: sidebar toggle icon button ── */
    gui->btn_sidebar_toggle = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(gui->btn_sidebar_toggle),
        gtk_image_new_from_icon_name("sidebar-show-symbolic"));
    gtk_widget_add_css_class(gui->btn_sidebar_toggle, "bottom-icon-btn");
    gtk_widget_set_tooltip_text(gui->btn_sidebar_toggle, "Toggle Sidebar (Ctrl+\\)");
    g_signal_connect(gui->btn_sidebar_toggle, "clicked",
                     G_CALLBACK(on_logo_clicked), gui);

    gui->path_label = gtk_label_new("Economics_Notes.md");
    gtk_widget_add_css_class(gui->path_label, "bottom-path");

    GtkWidget *bottom_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(bottom_spacer, TRUE);

    /* ── Bottom-right: Search icon button ── */
    gui->btn_search = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(gui->btn_search),
        gtk_image_new_from_icon_name("system-search-symbolic"));
    gtk_widget_add_css_class(gui->btn_search, "bottom-icon-btn");
    gtk_widget_set_tooltip_text(gui->btn_search, "Search in file (Ctrl+F)");
    g_signal_connect(gui->btn_search, "clicked", G_CALLBACK(on_search_icon_clicked), gui);

    gui->lbl_words = gtk_label_new("0 words");
    gtk_widget_add_css_class(gui->lbl_words, "bottom-bar-lbl");

    gui->lbl_chars = gtk_label_new("0 chars");
    gtk_widget_add_css_class(gui->lbl_chars, "bottom-bar-lbl");

    /* ── Far bottom-right: sync status dot button ── */
    gui->btn_sync_icon_bottom = gtk_button_new();
    gui->sync_dot = gtk_label_new("●");
    gtk_widget_add_css_class(gui->sync_dot, "status-dot");
    gtk_widget_add_css_class(gui->sync_dot, "status-saved");
    gtk_button_set_child(GTK_BUTTON(gui->btn_sync_icon_bottom), gui->sync_dot);
    gtk_widget_add_css_class(gui->btn_sync_icon_bottom, "bottom-icon-btn");
    gtk_widget_set_tooltip_text(gui->btn_sync_icon_bottom, "Sync Now");
    g_signal_connect(gui->btn_sync_icon_bottom, "clicked",
                     G_CALLBACK(on_sync_now_clicked), gui);

    gtk_box_append(GTK_BOX(bottom_bar), gui->btn_sidebar_toggle);
    gtk_box_append(GTK_BOX(bottom_bar), gui->path_label);
    gtk_box_append(GTK_BOX(bottom_bar), bottom_spacer);
    gtk_box_append(GTK_BOX(bottom_bar), gui->btn_search);
    gtk_box_append(GTK_BOX(bottom_bar), gui->lbl_words);
    gtk_box_append(GTK_BOX(bottom_bar), gui->lbl_chars);
    gtk_box_append(GTK_BOX(bottom_bar), gui->btn_sync_icon_bottom);

    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar_view), bottom_bar);

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
    gtk_window_present(GTK_WINDOW(window));
    zig_on_gui_ready();

    load_sync_credentials(gui);
    int connected = zig_sync_check_status();
    gui_update_sync_status(connected, connected ? "Connected" : "Disconnected");
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
    sqlite3_exec(db, "INSERT OR IGNORE INTO sync_tokens (id, client_id, client_secret) VALUES (1, '100982736451-example.apps.googleusercontent.com', 'GOCSPX-examplesecret');", NULL, NULL, NULL);

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
    return gtk_text_buffer_get_text(buf, &start, &end, FALSE);
}

void gui_free_text(char *text) { g_free(text); }

void gui_set_text(const char *text, int len) {
    if (!global_source_view || !global_gui) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));
    
    g_signal_handlers_block_by_func(buf, on_insert_text_before, global_gui);
    g_signal_handlers_block_by_func(buf, on_insert_text_after, global_gui);
    g_signal_handlers_block_by_func(buf, on_delete_range_after, global_gui);
    
    gtk_text_buffer_set_text(buf, text, len);
    
    g_signal_handlers_unblock_by_func(buf, on_insert_text_before, global_gui);
    g_signal_handlers_unblock_by_func(buf, on_insert_text_after, global_gui);
    g_signal_handlers_unblock_by_func(buf, on_delete_range_after, global_gui);
    
    update_all_paragraphs_direction(buf);
    apply_wiki_link_tags(buf);

    // Recompute line height and update spacers
    int line_h = get_line_height(global_source_view);
    if (line_h <= 0) line_h = 24;
    global_gui->estimated_line_height = line_h;

    int top_h = global_gui->active_page_start_line * line_h;
    int bottom_h = (global_gui->total_virtual_lines - global_gui->active_page_end_line) * line_h;
    if (top_h < 0) top_h = 0;
    if (bottom_h < 0) bottom_h = 0;
    gtk_widget_set_size_request(global_gui->top_spacer, -1, top_h);
    gtk_widget_set_size_request(global_gui->bottom_spacer, -1, bottom_h);
    
    gtk_widget_queue_resize(global_gui->virtual_layout_box);
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
    *line = (global_gui ? global_gui->active_page_start_line : 0) + rel_line + 1;
    *col = gtk_text_iter_get_line_offset(&iter);
}

void gui_set_cursor_position(int line, int col) {
    if (!global_source_view || !global_gui) return;
    
    int target_abs_line = line - 1; // 0-indexed
    if (target_abs_line < 0) target_abs_line = 0;
    
    // Check if within active page
    if (target_abs_line < global_gui->active_page_start_line || target_abs_line >= global_gui->active_page_end_line) {
        // Trigger page reload centered around target_abs_line
        global_gui->in_scroll_update = TRUE;
        
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));
        GtkTextIter start_iter, end_iter;
        gtk_text_buffer_get_bounds(buf, &start_iter, &end_iter);
        char *page_text = gtk_text_buffer_get_text(buf, &start_iter, &end_iter, FALSE);
        extern int zig_save_active_page(int start_line, int end_line, const char *text);
        int status = zig_save_active_page(global_gui->active_page_start_line, global_gui->active_page_end_line, page_text);
        g_free(page_text);
        (void)status;
        
        int new_start = target_abs_line - 50;
        if (new_start < 0) new_start = 0;
        int new_end = new_start + 100;
        if (new_end > global_gui->total_virtual_lines) {
            new_end = global_gui->total_virtual_lines;
            new_start = new_end - 100;
            if (new_start < 0) new_start = 0;
        }
        
        extern const char *zig_get_text_for_line_range(int start_line, int end_line, int *out_len);
        int new_len = 0;
        const char *new_text = zig_get_text_for_line_range(new_start, new_end, &new_len);
        
        extern void on_insert_text_before(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data);
        extern void on_insert_text_after(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data);
        extern void on_delete_range_after(GtkTextBuffer *buf, GtkTextIter *start, GtkTextIter *end, gpointer user_data);
        
        g_signal_handlers_block_by_func(buf, on_insert_text_before, global_gui);
        g_signal_handlers_block_by_func(buf, on_insert_text_after, global_gui);
        g_signal_handlers_block_by_func(buf, on_delete_range_after, global_gui);
        
        gtk_text_buffer_set_text(buf, new_text ? new_text : "", new_len);
        
        g_signal_handlers_unblock_by_func(buf, on_insert_text_before, global_gui);
        g_signal_handlers_unblock_by_func(buf, on_insert_text_after, global_gui);
        g_signal_handlers_unblock_by_func(buf, on_delete_range_after, global_gui);
        
        update_all_paragraphs_direction(buf);
        extern void apply_wiki_link_tags(GtkTextBuffer *buf);
        apply_wiki_link_tags(buf);
        
        global_gui->active_page_start_line = new_start;
        global_gui->active_page_end_line = new_end;
        
        int line_h = get_line_height(global_source_view);
        if (line_h <= 0) line_h = 24;
        int top_h = new_start * line_h;
        int bottom_h = (global_gui->total_virtual_lines - new_end) * line_h;
        if (top_h < 0) top_h = 0;
        if (bottom_h < 0) bottom_h = 0;
        gtk_widget_set_size_request(global_gui->top_spacer, -1, top_h);
        gtk_widget_set_size_request(global_gui->bottom_spacer, -1, bottom_h);
        
        if (global_gui->vadjustment) {
            double target_y = (double)(target_abs_line * line_h);
            gtk_adjustment_set_value(global_gui->vadjustment, target_y);
        }
        
        gtk_widget_queue_resize(global_gui->virtual_layout_box);
        global_gui->in_scroll_update = FALSE;
    }
    
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));
    GtkTextIter iter;
    int rel_line = target_abs_line - global_gui->active_page_start_line;
    if (rel_line < 0) rel_line = 0;
    
    gtk_text_buffer_get_iter_at_line_offset(buf, &iter, rel_line, col);
    gtk_text_buffer_select_range(buf, &iter, &iter);
}

void gui_init_virtual_document(int total_lines, int start_line, int end_line) {
    if (!global_gui) return;
    global_gui->total_virtual_lines = total_lines;
    global_gui->active_page_start_line = start_line;
    global_gui->active_page_end_line = end_line;
    
    global_gui->estimated_line_height = get_line_height(global_gui->source_view);
    if (global_gui->estimated_line_height <= 0) {
        global_gui->estimated_line_height = 24;
    }
    
    int top_h = start_line * global_gui->estimated_line_height;
    int bottom_h = (total_lines - end_line) * global_gui->estimated_line_height;
    if (top_h < 0) top_h = 0;
    if (bottom_h < 0) bottom_h = 0;
    gtk_widget_set_size_request(global_gui->top_spacer, -1, top_h);
    gtk_widget_set_size_request(global_gui->bottom_spacer, -1, bottom_h);
    
    if (global_gui->vadjustment) {
        global_gui->in_scroll_update = TRUE;
        gtk_adjustment_set_value(global_gui->vadjustment, (double)top_h);
        global_gui->in_scroll_update = FALSE;
    }
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
    
    extern int zig_save_active_page(int start_line, int end_line, const char *text);
    int status = zig_save_active_page(global_gui->active_page_start_line, global_gui->active_page_end_line, page_text);
    g_free(page_text);
    
    if (status == 0) {
        gui_set_sync_status("Saved");
    } else {
        gui_set_sync_status("Save Failed");
    }
}

static void refresh_explorer_idle_cb(void *user_data) {
    (void)user_data;
    if (global_gui && global_gui->explorer_listbox && GTK_IS_LIST_BOX(global_gui->explorer_listbox)) {
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

typedef struct {
    int connected;
    char *status_text;
} SyncStatusUpdate;

static void update_sync_status_callback(void *user_data) {
    SyncStatusUpdate *up = (SyncStatusUpdate *)user_data;
    if (global_gui) {
        if (global_gui->sync_status_lbl) {
            gtk_label_set_text(GTK_LABEL(global_gui->sync_status_lbl), up->status_text);
        }
        
        if (up->connected == 2) {
            if (global_gui->sync_code_box) {
                gtk_widget_set_visible(global_gui->sync_code_box, TRUE);
            }
            if (global_gui->sync_now_btn) {
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
                    gtk_widget_set_sensitive(global_gui->sync_now_btn, TRUE);
                }
            } else {
                if (global_gui->sync_connect_btn) {
                    gtk_button_set_label(GTK_BUTTON(global_gui->sync_connect_btn), "Connect Account");
                }
                if (global_gui->sync_now_btn) {
                    gtk_widget_set_sensitive(global_gui->sync_now_btn, FALSE);
                }
            }
        }

        if (up->connected == 1) {
            gui_set_sync_status("Synced");
        } else if (up->connected == 2 && strcmp(up->status_text, "Syncing...") == 0) {
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

void gui_get_active_page_bounds(int *start_line, int *end_line, int *total_lines) {
    if (!global_gui) {
        *start_line = 0;
        *end_line = 0;
        *total_lines = 0;
        return;
    }
    *start_line = global_gui->active_page_start_line;
    *end_line = global_gui->active_page_end_line;
    *total_lines = global_gui->total_virtual_lines;
}

void gui_update_total_virtual_lines(int total_lines) {
    if (!global_gui) return;
    global_gui->total_virtual_lines = total_lines;
    
    // Update bottom spacer request
    int line_h = global_gui->estimated_line_height;
    if (line_h <= 0) line_h = 24;
    int bottom_h = (total_lines - global_gui->active_page_end_line) * line_h;
    if (bottom_h < 0) bottom_h = 0;
    gtk_widget_set_size_request(global_gui->bottom_spacer, -1, bottom_h);
    gtk_widget_queue_resize(global_gui->virtual_layout_box);
}
