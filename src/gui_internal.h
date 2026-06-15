#pragma once

#include "gui_shared.h"

/* Vault DB lives at $XDG_CONFIG_HOME/qirtas/vault.db — resolved once on
 * the Zig side (single source of truth, includes legacy migration). */
const char *zig_db_path(void);
#define DB_PATH zig_db_path()
#define GHOST_COUNT 40
#define GHOST_MIN_DIST 3.0

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
    GtkWidget *status_pill;
    GtkWidget *lbl_words;
    GtkWidget *lbl_chars;
    GtkWidget *lbl_lines;
    GtkWidget *sync_dot;

    /* Search */
    GtkWidget               *search_revealer;
    GtkWidget               *search_entry;
    GtkWidget               *replace_entry;
    GtkWidget               *search_match_label;
    GtkSourceSearchContext  *search_ctx;
    GtkSourceSearchSettings *search_settings;
    gboolean                 search_visible;

    /* Source view */
    GtkWidget *scrolled;
    GtkWidget *source_view;
    GtkWidget *active_popover;

    /* Explorer */
    GtkWidget *explorer_listbox;
    GtkWidget *exp_count_label;
    GtkWidget *exp_search_entry;

    /* Stats */
    GtkWidget *stats_time_val;
    GtkWidget *stats_file_val;

    /* Theme / font */
    char   current_theme[32];
    char   current_en_font[64];
    char   current_ar_font[64];
    double current_font_size;

    /* Font / CSS providers */
    GtkCssProvider *font_provider;
    GtkCssProvider *css_provider;

    /* Sync Engine UI */
    GtkWidget *sync_status_lbl;
    GtkWidget *sync_connect_btn;
    GtkWidget *sync_now_btn;
    GtkWidget *sync_code_entry;
    GtkWidget *sync_submit_btn;
    GtkWidget *sync_code_box;
    GtkWidget *client_id_entry;
    GtkWidget *client_secret_entry;

    /* Dropbox Sync UI */
    GtkWidget *dropbox_status_lbl;
    GtkWidget *dropbox_connect_btn;
    GtkWidget *dropbox_now_btn;
    GtkWidget *dropbox_code_entry;
    GtkWidget *dropbox_submit_btn;
    GtkWidget *dropbox_code_box;
    GtkWidget *dropbox_client_id_entry;
    GtkWidget *dropbox_client_secret_entry;

    /* GitHub Sync UI */
    GtkWidget *github_status_lbl;
    GtkWidget *github_connect_btn;
    GtkWidget *github_now_btn;
    GtkWidget *github_token_entry;
    GtkWidget *github_repo_entry;

    /* Local folder sync */
    GtkWidget *local_sync_status_lbl;
    GtkWidget *local_sync_btn;

    GtkWidget *settings_window;
    GtkWidget *recovery_window;
    GtkWidget *recovery_words_view;
    GtkWidget *recovery_passphrase_entry;
    GtkWidget *recovery_status_lbl;

    /* Bottom-bar buttons */
    GtkWidget *btn_sidebar_toggle;
    GtkWidget *btn_sync_icon_bottom;
    GtkWidget *btn_status_actions;

    /* Virtual document layout */
    GtkWidget     *virtual_layout_box;
    GtkWidget     *top_spacer;
    GtkWidget     *bottom_spacer;
    GtkAdjustment *vadjustment;
    gboolean       in_scroll_update;
    gboolean       scroll_queued;
    gboolean       loading_viewport;
    gboolean       mouse_dragging;
    gboolean       primary_button_down;
    double         mouse_press_x;
    double         mouse_press_y;
    double         last_v_offset;
    int            last_scroll_requested_line;
    guint          buffer_generation;

    /* Cursor trail animation */
    GtkWidget *cursor_trail_area;
    gboolean   cursor_initialized;
    double     cursor_target_x;
    double     cursor_target_y;
    double     cursor_current_x;
    double     cursor_current_y;
    double     cursor_height;
    double     cursor_width;
    struct {
        double x;
        double y;
        double alpha;
    } trail[GHOST_COUNT];
    int      trail_len;
    gboolean trail_needs_clear;
    guint    cursor_tick_id;

    /* Layout preferences */
    GtkWidget *main_vertical_box;
    GtkWidget *sidebar_editor_box;
    GtkWidget *bottom_bar_widget;
    GtkWidget *sb_pos_dropdown;
    GtkWidget *sb_side_dropdown;
    GtkWidget *text_width_dropdown;
    GtkWidget *divider_chk;
    GtkWidget *focus_chk;
    gboolean   enable_focus_mode;
    GtkWidget *wrap_chk;
    int        document_total_lines;
    int        line_height;
    /* Vault system */
    GtkWidget *vault_path_lbl_val;

    /* Cursor trail */
    gboolean enable_cursor_trail;
    gboolean enable_bottom_margin;
    gboolean enable_editor_border;
    gboolean use_custom_trail_color;
    GdkRGBA  custom_trail_color;
    GtkWidget *trail_color_btn;
    GtkWidget *trail_color_chk;
    gboolean use_custom_pointer_color;
    GdkRGBA  custom_pointer_color;
    GtkWidget *pointer_color_btn;
    GtkWidget *pointer_color_chk;

    /* Tabs system */
    char *open_tabs[20];
    char *tab_contents[20];
    gboolean tab_modified[20];
    int num_tabs;
    int active_tab_index;
    GtkWidget *tab_bar_box;
    GtkWidget *tab_bar_scroll;
    GtkWidget *btn_tab_scroll_left;
    GtkWidget *btn_tab_scroll_right;

    /* Re-entrancy guard */
    gboolean in_conceal_update;
    gboolean show_layout_dividers;

    /* Dirty-line range accumulated across edits within one stats-debounce
     * window. buffer_stats_timeout_cb reconceals only this range instead of
     * the whole document. -1 = nothing dirty. */
    int conceal_dirty_start;
    int conceal_dirty_end;

    /* TRUE when an edit since the last debounce may have changed the heading
     * outline (line added/removed, or a heading line touched). The stats
     * debounce rebuilds gui_outline_refresh only when set — a plain keystroke
     * inside a paragraph leaves it FALSE and skips the O(document) scan. */
    gboolean outline_dirty;

    /* Editor preferences */
    gboolean  wrap_lines;
    gboolean  show_line_numbers;
    gboolean  highlight_current_line;
    gboolean  show_right_margin;
    int       right_margin_pos;
    gboolean  text_width_full_page;
    gboolean  show_overview_map;
    gboolean  restore_session;
    gboolean  compact_mode;
    GtkWidget *source_map;
    GtkWidget *outline_box;

    /* Redesign: top tab strip, card header band, desk outline panel */
    GtkWidget *tab_strip;            /* moved to top of main_vertical_box */
    GtkWidget *editor_card;          /* paper card wrapper: thread+header+text */
    GtkWidget *editor_header;        /* 46px card header band */
    GtkWidget *editor_thread;        /* 2px gradient thread on card top */
    GtkWidget *breadcrumb_label;     /* folder/sub/file path in header */
    GtkWidget *outline_panel;        /* GtkRevealer on the desk */
    GtkWidget *outline_panel_inner;  /* content box, auto-hidden when empty */
    gboolean   outline_panel_visible;

    /* User-resizable gap between the paper card and the desk edge (drag the
     * card's outer edge); text_column_width is derived from the card width
     * each tick, not persisted directly. */
    int        desk_gap;
    int        text_column_width;    /* derived cache, recomputed by paper_column_tick */
    int        centered_text_width;  /* user slider: max column width in centered mode (px) */
    GtkWidget *width_slider;
    gboolean   resizing_text_column;
    int        resize_drag_start_gap;
    int        resize_drag_edge;     /* -1 = left edge, +1 = right edge, 0 = none */
    guint      resize_column_tick_id; /* 60fps tick, active only mid-drag */

    /* Read mode: view-only rendering state. Non-editable, caret hidden,
     * markdown syntax markers always concealed, narrower reading column.
     * Reusable by future renderers (tables, LaTeX) that hook the same flag. */
    gboolean   read_mode;
    GtkWidget *btn_read_mode;

    /* Sidebar brand logo image, swapped between light/dark PNGs on theme change. */
    GtkWidget *logo_image;
} AppGui;

/* Language / icon style (loaded from app_prefs at startup) */
extern int qirtas_app_language; /* 0 = English, 1 = Arabic */
extern int qirtas_icon_style;   /* 0 = Classic, 1 = Modern */
const char *qirtas_tr(const char *en);
const char *qirtas_icon(const char *key);

void show_quick_switcher(AppGui *gui);
void gui_outline_refresh(AppGui *gui);

/* Perf observability: QIRTAS_PERF=1 logs main-loop callbacks > 8 ms. */
extern int qirtas_perf_enabled;
extern int qirtas_no_conceal;
#define QIRTAS_PERF_BEGIN gint64 _qp_t0 = qirtas_perf_enabled ? g_get_monotonic_time() : 0
#define QIRTAS_PERF_END(name) do { \
    if (qirtas_perf_enabled) { \
        gint64 _qp_d = g_get_monotonic_time() - _qp_t0; \
        if (_qp_d > 8000) g_printerr("[perf] %s took %.1f ms\n", (name), _qp_d / 1000.0); \
    } } while (0)

/* Arabic search normalization (implemented in Zig, single source) */
char *zig_normalize_arabic(const char *text);
void zig_free_normalized(char *ptr);

/* Generic key/value preference store (app_prefs table in vault.db) */
int  qirtas_pref_get_int(const char *key, int fallback);
void qirtas_pref_set_int(const char *key, int value);
char *qirtas_pref_get_string(const char *key);
void qirtas_pref_set_string(const char *key, const char *value);

typedef struct {
    const char      *action_id;
    const char      *display_name;
    char             shortcut_str[64];
    guint            keyval;
    GdkModifierType  state;
} AppShortcut;

typedef struct {
    AppGui *gui;
    gint    offset;
    guint   generation;
} ScrollToCursorData;

typedef struct {
    AppGui *gui;
    guint   generation;
} HrRenderData;

gboolean idle_render_hrs_cb(gpointer user_data);

void gui_remeasure_line_height(void);

void gui_set_sync_status(const char *status);
void gui_run_on_main_thread(GuiIdleCallback callback, void *user_data);

/* Add/open popover widget bundle (gui_dialogs.c, built in activate). */
typedef struct {
    GtkWidget *popover;
    GtkWidget *box_actions;
    GtkWidget *box_input;
    GtkWidget *entry_name;
    AppGui *gui;
} AddPopoverWidgets;

/* Dialogs, add/open popover, folder prompt, shutdown (gui_dialogs.c). */
void on_open_dialog_response(GObject *source_object, GAsyncResult *res, gpointer user_data);
void on_vault_dialog_response(GObject *source_object, GAsyncResult *res, gpointer user_data);
void on_open_vault_clicked(GtkButton *btn, gpointer user_data);
void on_open_existing_clicked(GtkButton *btn, gpointer user_data);
void on_create_submit(GtkEntry *entry, gpointer user_data);
void on_create_new_clicked(GtkButton *btn, gpointer user_data);
void on_popover_closed(GtkPopover *popover, gpointer user_data);
void on_app_shutdown(GApplication *app, gpointer user_data);
gboolean on_window_close_request(GtkWindow *window, gpointer user_data);
void on_save_as_dialog_response(GObject *source_object, GAsyncResult *res, gpointer user_data);
void prompt_new_folder(AppGui *gui, const char *parent_dir);

/* Settings window opener (gui.c). */
void on_settings_btn_clicked(GtkButton *btn, gpointer user_data);

/* Status bar actions + overflow menu (gui_statusbar.c). */
void popdown_ancestor_popover(GtkWidget *w);
GtkWidget *status_menu_item(const char *icon, const char *label, const char *hint, GCallback cb, gpointer user_data);
void on_status_menu_shortcuts(GtkButton *btn, gpointer user_data);
void on_status_menu_settings(GtkButton *btn, gpointer user_data);
void on_status_menu_quit(GtkButton *btn, gpointer user_data);
void on_status_menu_find_replace(GtkButton *btn, gpointer user_data);
void on_status_menu_fullscreen(GtkButton *btn, gpointer user_data);
void on_status_menu_copy_file(GtkButton *btn, gpointer user_data);
void on_status_menu_save_as(GtkButton *btn, gpointer user_data);
void on_status_bar_open_file_clicked(GtkButton *btn, gpointer user_data);
void on_status_bar_new_file_clicked(GtkButton *btn, gpointer user_data);
void on_status_bar_save_file_clicked(GtkButton *btn, gpointer user_data);
void on_status_bar_export_pdf_clicked(GtkButton *btn, gpointer user_data);
void on_read_mode_toggle_clicked(GtkButton *btn, gpointer user_data);
void on_restart_clicked(GtkButton *btn, gpointer user_data);

/* Paper-card geometry (shared between gui.c and gui_layout.c). */
#define QIRTAS_DESK_GAP_MIN 8
#define QIRTAS_DESK_GAP_MAX 360
#define QIRTAS_CARD_CHROME       160
#define QIRTAS_TEXT_COLUMN_MIN   420
#define QIRTAS_TEXT_COLUMN_MAX   840

typedef struct {
    AppGui *gui;
    GtkTextMark *mark;
    guint generation;
} ReadModeScrollData;

void zig_set_focus_mode(int enabled);

/* Editor layout & display preferences (gui_layout.c). */
void reorder_main_layout(AppGui *gui);
void apply_editor_border(AppGui *gui);
gboolean paper_column_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data);
void apply_focus_mode(AppGui *gui);
void toggle_read_mode(AppGui *gui);
void apply_compact_mode(AppGui *gui);
void apply_editor_prefs(AppGui *gui);
void apply_layout_dividers(AppGui *gui);
int  paper_edge_margin(AppGui *gui, GtkWidget *overlay);
gboolean paper_column_timeout_wrapper(gpointer data);
void on_editor_border_toggled(GtkCheckButton *chk, gpointer user_data);
void on_outline_close_clicked(GtkButton *btn, gpointer user_data);
void on_compact_mode_toggled(GtkCheckButton *btn, gpointer user_data);
void on_highlight_line_toggled(GtkCheckButton *btn, gpointer user_data);
void on_line_numbers_toggled(GtkCheckButton *btn, gpointer user_data);
void on_restore_session_toggled(GtkCheckButton *btn, gpointer user_data);
void on_text_width_mode_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);
void on_width_slider_changed(GtkRange *range, gpointer user_data);
void on_card_gap_slider_changed(GtkRange *range, gpointer user_data);
void on_focus_mode_toggled(GtkCheckButton *chk, gpointer user_data);
void on_column_resize_motion(GtkEventControllerMotion *ctrl, gdouble x, gdouble y, gpointer user_data);
void on_column_resize_begin(GtkGestureDrag *gesture, gdouble x, gdouble y, gpointer user_data);
void on_column_resize_update(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y, gpointer user_data);
void on_column_resize_end(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y, gpointer user_data);

extern AppGui *global_gui;
extern GtkWidget *main_window;
extern GtkWidget *global_window;
extern GtkWidget *global_source_view;
extern GtkWidget *global_sync_label;
extern GtkWidget *global_path_label;
extern GtkWidget *global_time_label;

void init_css(AppGui *gui);
void init_cursor_trail(AppGui *gui);
void cursor_trail_wake(AppGui *gui);
void gui_trigger_autosave(void);
void gui_history_record(const char *path);
void draw_cursor_trail(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);
gboolean on_cursor_tick(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data);
void gui_tabs_refresh(AppGui *gui);
void gui_tabs_add_or_select(AppGui *gui, const char *filepath);
void gui_tabs_close_all(AppGui *gui);
void gui_tabs_setup_viewport(AppGui *gui);
void qirtas_export_to_pdf(AppGui *gui);          /* themed chooser (gui_export.c) */
void arabize_digits(const char *in, char *out, size_t out_size);
int gui_get_absolute_cursor_line(void);
void on_font_size_changed(GtkSpinButton *spin, gpointer user_data);
void on_en_font_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);
void on_ar_font_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);
void on_theme_dropdown_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);
void check_and_insert_hr(GtkTextBuffer *buf, AppGui *gui);
void parse_and_render_hrs(GtkTextBuffer *buf, AppGui *gui);
void apply_theme(AppGui *gui, const char *theme_name);
gboolean qirtas_theme_is_dark(const char *theme_name);
void gui_update_brand_logo(AppGui *gui);
void toggle_read_mode(AppGui *gui);
void update_editor_font(AppGui *gui);
void gui_zoom_in(AppGui *gui);
void gui_zoom_out(AppGui *gui);
void gui_zoom_reset(AppGui *gui);
void reset_cursor_trail(AppGui *gui);
void gui_push_undo_snapshot(void);
void gui_reload_full_buffer(void);
void gui_set_buffer_modified(gboolean modified);
void on_search_text_changed(GtkSearchEntry *entry, gpointer user_data);
void on_search_occurrences_changed(GObject *obj, GParamSpec *pspec, gpointer user_data);
void on_search_next_clicked(GtkButton *btn, gpointer user_data);
void on_search_prev_clicked(GtkButton *btn, gpointer user_data);
void on_replace_clicked(GtkButton *btn, gpointer user_data);
void on_replace_all_clicked(GtkButton *btn, gpointer user_data);
gboolean on_search_entry_key(GtkEventControllerKey *ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
void toggle_search(AppGui *gui);
void toggle_fullscreen(AppGui *gui);
void on_search_icon_clicked(GtkButton *btn, gpointer user_data);
void on_close_search_clicked(GtkButton *btn, gpointer user_data);
void apply_wiki_link_tags(GtkTextBuffer *buf);
void apply_wiki_link_tags_local(GtkTextBuffer *buf);
void on_editor_right_click(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data);
void apply_format(GtkTextBuffer *buf, const char *prefix, const char *suffix);
void apply_paragraph_format(GtkTextBuffer *buf, const char *prefix);
void apply_paragraph_alignment(GtkTextBuffer *buf, GtkJustification justification);
void on_editor_left_click(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data);
void on_editor_motion(GtkEventControllerMotion *controller, gdouble x, gdouble y, gpointer user_data);
void update_conceal_markdown(GtkTextBuffer *buf);
void update_conceal_markdown_range(GtkTextBuffer *buf, int first_line, int last_line);
void update_conceal_markdown_all(GtkTextBuffer *buf);
void update_conceal_markdown_all_sync(GtkTextBuffer *buf);
void on_buffer_changed(GtkTextBuffer *buf, gpointer user_data);
void on_insert_text_before(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data);
void on_insert_text_after(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data);
void on_delete_range_after(GtkTextBuffer *buf, GtkTextIter *start, GtkTextIter *end, gpointer user_data);
void on_delete_range_before(GtkTextBuffer *buf, GtkTextIter *start, GtkTextIter *end, gpointer user_data);
void on_mark_set(GtkTextBuffer *buf, GtkTextIter *location, GtkTextMark *mark, gpointer user_data);
void save_trail_color_settings(AppGui *gui);
void save_pointer_color_settings(AppGui *gui);
void load_trail_color_settings(AppGui *gui);
void load_pointer_color_settings(AppGui *gui);
void populate_explorer(AppGui *gui);
void on_explorer_search_changed(GtkSearchEntry *entry, gpointer user_data);
void on_file_card_clicked(GtkButton *btn, gpointer user_data);
int explorer_sort_func(GtkListBoxRow *row1, GtkListBoxRow *row2, gpointer user_data);
gboolean idle_scroll_to_cursor(gpointer user_data);
gboolean match_app_shortcut(const char *action_id, guint keyval, guint keycode, GdkModifierType state);
gboolean keycode_matches_latin_keyval(guint keycode, guint target_keyval);
gboolean keypress_has_text_modifier(GdkModifierType state);
void init_app_shortcuts(void);
void show_keybindings_window(AppGui *gui);
void shortcuts_cancel_listening(void);
void insert_text_pair(GtkTextBuffer *buf, const char *open, const char *close);
gboolean maybe_skip_closing_pair(GtkTextBuffer *buf, const char *close);
gboolean maybe_delete_empty_pair(GtkTextBuffer *buf);
void duplicate_current_line(GtkTextBuffer *buf);
void insert_horizontal_rule(GtkTextBuffer *buf);
void delete_current_line(GtkTextBuffer *buf);
void move_current_line(GtkTextBuffer *buf, gboolean up);
gboolean on_editor_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
void trigger_save_as(AppGui *gui);
void gui_manual_save(AppGui *gui);
void toggle_comment_current_line(GtkTextBuffer *buf);
void clear_selection_formatting(GtkTextBuffer *buf);
int gui_get_buffer_modified(void);
void on_buffer_modified_changed(GtkTextBuffer *buf, gpointer user_data);
void select_position_range(AppGui *gui, Position start, Position end);
Position iter_to_position(GtkTextIter *iter);
Position advance_position(Position pos, const char *text);
void update_paragraph_direction_lines(GtkTextBuffer *buf, gint first_line, gint last_line);
void update_all_paragraphs_direction(GtkTextBuffer *buf);
void gui_refresh_buffer_stats(void);
void gui_word_count_invalidate(void);
Position gui_buffer_replace(GtkTextBuffer *buf, int start_off, int end_off, const char *replacement);
void on_tree_file_right_click(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data);

