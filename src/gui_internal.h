#pragma once

#include "gui_shared.h"

#define DB_PATH "/home/.config/lawh/vault.db"
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
    GtkWidget *sync_dot;

    /* Search */
    GtkWidget               *search_revealer;
    GtkWidget               *search_entry;
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
    gulong         scroll_signal_id;
    gboolean       in_scroll_update;
    gboolean       loading_viewport;
    gboolean       mouse_dragging;
    gboolean       primary_button_down;
    double         mouse_press_x;
    double         mouse_press_y;
    double         last_v_offset;
    int            total_virtual_lines;
    int            last_scroll_requested_line;
    guint          buffer_generation;
    int            pending_line;
    int            pending_col;

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

    /* Layout preferences */
    GtkWidget *main_vertical_box;
    GtkWidget *sidebar_editor_box;
    GtkWidget *bottom_bar_widget;
    GtkWidget *sb_pos_dropdown;
    GtkWidget *sb_side_dropdown;
    GtkWidget *divider_chk;
    GtkWidget *focus_chk;
    gboolean   enable_focus_mode;
    gboolean   virtual_scroll_enabled;
    GtkWidget *wrap_chk;
    /* Viewport state */
    int viewport_start_line;
    int viewport_end_line;
    int document_total_lines;
    int line_height;
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

    /* Re-entrancy guard */
    gboolean in_conceal_update;
    gboolean show_layout_dividers;
} AppGui;

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
void gui_reset_scroll_direction_state(void);
void gui_remeasure_line_height(void);

gboolean debug_get_iter_at(
    GtkTextBuffer *buf,
    GtkTextIter *iter,
    int rel_line,
    int col,
    const char *caller
);

void debug_get_iter_at_offset(
    GtkTextBuffer *buf,
    GtkTextIter *iter,
    int char_offset,
    const char *caller
);

void debug_set_line_offset(
    GtkTextIter *iter,
    int offset,
    const char *caller
);

extern AppGui *global_gui;
extern GtkWidget *main_window;
extern GtkWidget *global_window;
extern GtkWidget *global_source_view;
extern GtkWidget *global_sync_label;
extern GtkWidget *global_path_label;
extern GtkWidget *global_time_label;

void init_css(AppGui *gui);
void init_cursor_trail(AppGui *gui);
void draw_cursor_trail(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);
gboolean on_cursor_tick(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data);
void gui_tabs_refresh(AppGui *gui);
void qirtas_export_to_pdf(AppGui *gui);
int gui_get_absolute_cursor_line(void);
void on_font_size_changed(GtkSpinButton *spin, gpointer user_data);
void on_en_font_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);
void on_ar_font_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);
void on_theme_dropdown_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);
void check_and_insert_hr(GtkTextBuffer *buf, AppGui *gui);
void parse_and_render_hrs(GtkTextBuffer *buf, AppGui *gui);
void apply_theme(AppGui *gui, const char *theme_name);
void update_editor_font(AppGui *gui);
void reset_cursor_trail(AppGui *gui);
void gui_push_undo_snapshot(void);
void request_viewport_position(AppGui *gui, int abs_line);
void gui_reload_viewport(void);
void gui_set_buffer_modified(gboolean modified);
void on_search_text_changed(GtkSearchEntry *entry, gpointer user_data);
void on_search_next_clicked(GtkButton *btn, gpointer user_data);
void on_search_prev_clicked(GtkButton *btn, gpointer user_data);
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
void on_editor_left_click(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data);
void on_editor_motion(GtkEventControllerMotion *controller, gdouble x, gdouble y, gpointer user_data);
void update_conceal_markdown(GtkTextBuffer *buf);
void update_conceal_markdown_all(GtkTextBuffer *buf);
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
void insert_text_pair(GtkTextBuffer *buf, const char *open, const char *close);
gboolean maybe_skip_closing_pair(GtkTextBuffer *buf, const char *close);
gboolean maybe_delete_empty_pair(GtkTextBuffer *buf);
void duplicate_current_line(GtkTextBuffer *buf);
void delete_current_line(GtkTextBuffer *buf);
void move_current_line(GtkTextBuffer *buf, gboolean up);
void load_viewport_page(AppGui *gui, int new_start);
gboolean on_editor_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
void trigger_save_as(AppGui *gui);
void gui_manual_save(AppGui *gui);
void toggle_comment_current_line(GtkTextBuffer *buf);
void clear_selection_formatting(GtkTextBuffer *buf);

void debug_place_cursor(GtkTextBuffer *buf, const GtkTextIter *iter, const char *caller);
void debug_select_range(GtkTextBuffer *buf, const GtkTextIter *ins, const GtkTextIter *bound, const char *caller);
void debug_scroll_to_mark(GtkTextView *text_view, GtkTextMark *mark, double within_margin, gboolean use_align, double xalign, double yalign, const char *caller);
void debug_scroll_mark_onscreen(GtkTextView *text_view, GtkTextMark *mark, const char *caller);

#define gtk_text_buffer_place_cursor(buf, iter) \
    debug_place_cursor(buf, iter, __func__)

#define gtk_text_buffer_select_range(buf, ins, bound) \
    debug_select_range(buf, ins, bound, __func__)

#define gtk_text_view_scroll_to_mark(tv, mark, margin, use_align, xalign, yalign) \
    debug_scroll_to_mark(tv, mark, margin, use_align, xalign, yalign, __func__)

#define gtk_text_view_scroll_mark_onscreen(tv, mark) \
    debug_scroll_mark_onscreen(tv, mark, __func__)
