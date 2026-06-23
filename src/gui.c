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

/* Transient editor toast ("Copied", etc.) — a revealer pill over the editor
 * overlay. Created in the editor setup; driven by gui_show_toast(). */
static GtkWidget *g_toast_revealer = NULL;
static GtkWidget *g_toast_label = NULL;
static guint g_toast_timeout_id = 0;

static gboolean toast_hide_cb(gpointer data) {
    (void)data;
    g_toast_timeout_id = 0;
    if (g_toast_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(g_toast_revealer), FALSE);
    return G_SOURCE_REMOVE;
}

void gui_show_toast(const char *msg) {
    if (!g_toast_revealer || !g_toast_label || !msg) return;
    gtk_label_set_text(GTK_LABEL(g_toast_label), msg);
    gtk_revealer_set_reveal_child(GTK_REVEALER(g_toast_revealer), TRUE);
    if (g_toast_timeout_id) g_source_remove(g_toast_timeout_id);
    g_toast_timeout_id = g_timeout_add(1300, toast_hide_cb, NULL);
}

/* Perf observability gate (QIRTAS_PERF=1). Read by QIRTAS_PERF_* macros in
 * gui_internal.h; set from env in run_gui(). */
int qirtas_perf_enabled = 0;

/* Diagnostic toggles (env, read in run_gui). QIRTAS_NO_CONCEAL=1 disables the
 * markdown conceal passes so scroll/typing cost can be measured with the
 * conceal-tag overhead removed — tells us whether conceal drives scroll CPU
 * before committing to viewport-scoped conceal. */
int qirtas_no_conceal = 0;

/* Icon style (0 = Classic, 1 = Modern); drives qirtas_icon() lookup. */
int qirtas_icon_style = 0;

/* Offline measurement of the per-pause buffer_stats_timeout_cb work, minus the
 * GUI plumbing. Builds a headless GtkTextBuffer (no window) from `text` and
 * times the three O(document) passes that run on every typing pause:
 *   word-count  : gtk_text_buffer_get_text() + full UTF-8 walk (capped >500k)
 *   outline scan: the gui_outline_refresh per-line loop (get_iter_at_line +
 *                 get_text per line), without creating widgets
 *   counts      : get_char_count / get_line_count (expected ~free)
 * Prints ms to stderr. Called from the QIRTAS_BENCH harness. */
void qirtas_bench_stats(const char *text, int len) {
    if (!gtk_is_initialized() && !gtk_init_check()) {
        g_printerr("[bench] gtk_init_check failed (no display?) — skipping stats bench\n");
        return;
    }
    GtkTextBuffer *buf = gtk_text_buffer_new(NULL);
    gtk_text_buffer_set_text(buf, text, len);

    gint64 t0 = g_get_monotonic_time();
    glong char_count = (glong)gtk_text_buffer_get_char_count(buf);
    glong line_count = (glong)gtk_text_buffer_get_line_count(buf);
    gint64 t1 = g_get_monotonic_time();

    /* word count (same cap + walk as the real callback) */
    glong word_count = -1;
    if (char_count <= 500000) {
        GtkTextIter s, e;
        gtk_text_buffer_get_bounds(buf, &s, &e);
        gchar *t = gtk_text_buffer_get_text(buf, &s, &e, TRUE);
        word_count = 0;
        gboolean in_word = FALSE;
        for (char *p = t; *p; p = g_utf8_next_char(p)) {
            gunichar ch = g_utf8_get_char(p);
            if (g_unichar_isspace(ch) || g_unichar_ispunct(ch)) in_word = FALSE;
            else if (!in_word) { word_count++; in_word = TRUE; }
        }
        g_free(t);
    }
    gint64 t2 = g_get_monotonic_time();

    /* outline scan: the gui_outline_refresh loop, no widget creation */
    int headings = 0, lc = (int)line_count;
    for (int i = 0; i < lc && headings < 200; i++) {
        GtkTextIter ls, le;
        gtk_text_buffer_get_iter_at_line(buf, &ls, i);
        le = ls;
        if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
        gchar *lt = gtk_text_buffer_get_text(buf, &ls, &le, TRUE);
        int level = 0;
        while (lt[level] == '#') level++;
        if (level >= 1 && level <= 6 && lt[level] == ' ' && lt[level + 1] != '\0') headings++;
        g_free(lt);
    }
    gint64 t3 = g_get_monotonic_time();

    g_printerr("[bench] stats-pass (per typing pause) on %ld chars / %ld lines:\n", char_count, line_count);
    g_printerr("[bench]   counts (char+line)        %.3f ms\n", (t1 - t0) / 1000.0);
    g_printerr("[bench]   word-count walk           %.3f ms%s\n", (t2 - t1) / 1000.0, word_count < 0 ? " (SKIPPED: >500k cap)" : "");
    g_printerr("[bench]   outline scan (%d headings) %.3f ms\n", headings, (t3 - t2) / 1000.0);

    g_object_unref(buf);
}


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

static void on_trail_toggled(GObject *gobject, GParamSpec *pspec, gpointer user_data);

void populate_explorer(AppGui *gui);
static void set_active_tab(AppGui *gui, GtkWidget *active_btn, const char *page);
void toggle_search(AppGui *gui);
void on_insert_text_before(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data);
void on_insert_text_after(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data);
void on_delete_range_after(GtkTextBuffer *buf, GtkTextIter *start, GtkTextIter *end, gpointer user_data);
gboolean on_editor_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
void on_editor_right_click(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data);
void update_conceal_markdown(GtkTextBuffer *buf);
void update_conceal_markdown_all(GtkTextBuffer *buf);
void on_buffer_changed(GtkTextBuffer *buf, gpointer user_data);
void on_mark_set(GtkTextBuffer *buf, GtkTextIter *location, GtkTextMark *mark, gpointer user_data);
static void on_cursor_position_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
void apply_wiki_link_tags(GtkTextBuffer *buf);
void on_editor_left_click(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data);
void on_editor_motion(GtkEventControllerMotion *controller, gdouble x, gdouble y, gpointer user_data);
static gboolean on_editor_mouse_event(GtkEventControllerLegacy *controller, GdkEvent *event, gpointer user_data);
gboolean keycode_matches_latin_keyval(guint keycode, guint target_keyval);
void show_keybindings_window(AppGui *gui);
static gboolean on_settings_window_close_request(GtkWindow *window, gpointer user_data);
static void on_status_bar_pos_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);
static void on_sidebar_side_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);
void qirtas_export_to_pdf(AppGui *gui);
void update_editor_font(AppGui *gui);
void check_and_insert_hr(GtkTextBuffer *buf, AppGui *gui);
void parse_and_render_hrs(GtkTextBuffer *buf, AppGui *gui);
void parse_and_render_code_pills(GtkTextBuffer *buf, AppGui *gui);
void parse_and_render_tables(GtkTextBuffer *buf, AppGui *gui);
void gui_table_reset_reveal(GtkTextBuffer *buf);
static char *replace_anchors_with_hrs(const char *src);
void apply_paragraph_alignment(GtkTextBuffer *buf, GtkJustification justification);
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

void init_app_shortcuts(void);
gboolean match_app_shortcut(const char *action_id, guint keyval, guint keycode, GdkModifierType state);


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

static char current_theme[32] = "qirtas";
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









static void on_trail_toggled(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    AppGui *gui = (AppGui *)user_data;
    gboolean active = gtk_switch_get_active(GTK_SWITCH(gobject));
    gui->cursor.enable_trail = active;
    zig_set_cursor_trail(active ? 1 : 0);
    if (!active) {
        gui->cursor.trail_len = 0;
        if (gui->cursor.trail_area) {
            gtk_widget_queue_draw(gui->cursor.trail_area);
        }
    }
}

/* Font-size stepper (− / value / +). Clamped to the same 10-26 range the old
 * GtkSpinButton enforced. */
static void on_font_size_step_clicked(GtkButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    int delta = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "qirtas-delta"));
    double new_size = gui->current_font_size + delta;
    if (new_size < 10.0) new_size = 10.0;
    if (new_size > 26.0) new_size = 26.0;
    if (new_size == gui->current_font_size) return;
    gui->current_font_size = new_size;
    qirtas_pref_set_int("font_size", (int)new_size);  /* persist as the new default */
    update_editor_font(gui);
    if (gui->font_size_value_lbl) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", (int)gui->current_font_size);
        gtk_label_set_text(GTK_LABEL(gui->font_size_value_lbl), buf);
    }
}

/* Persist the main window's current size + maximized state so it reopens the
 * same way. Called from every user-initiated exit path (window close, Ctrl+Q,
 * the unsaved-changes prompt). Reads the live allocation, so the window must
 * still be realized — true for all those paths. */
void gui_save_window_geometry(AppGui *gui) {
    if (!gui || !gui->window) return;
    GtkWindow *win = GTK_WINDOW(gui->window);
    gboolean maximized = gtk_window_is_maximized(win);
    qirtas_pref_set_int("window_maximized", maximized ? 1 : 0);
    if (!maximized) {
        int w = gtk_widget_get_width(GTK_WIDGET(win));
        int h = gtk_widget_get_height(GTK_WIDGET(win));
        if (w > 100 && h > 100) {
            qirtas_pref_set_int("window_width", w);
            qirtas_pref_set_int("window_height", h);
        }
    }
}

/* Settings row: title (+ optional subtitle) on the left, GtkSwitch on the
 * right. Returned box is appended directly to pop_box and inherits the
 * existing .settings-sheet-body > box card styling. */
static GtkWidget *settings_switch_row(const char *title, const char *subtitle,
                                       gboolean active, GCallback cb, gpointer user_data,
                                       GtkWidget **out_switch) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

    GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(text_box, TRUE);
    gtk_widget_set_valign(text_box, GTK_ALIGN_CENTER);

    GtkWidget *title_lbl = gtk_label_new(title);
    gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);
    gtk_widget_add_css_class(title_lbl, "settings-switch-title");
    gtk_box_append(GTK_BOX(text_box), title_lbl);

    if (subtitle) {
        GtkWidget *sub_lbl = gtk_label_new(subtitle);
        gtk_widget_set_halign(sub_lbl, GTK_ALIGN_START);
        gtk_widget_add_css_class(sub_lbl, "settings-switch-subtitle");
        gtk_box_append(GTK_BOX(text_box), sub_lbl);
    }

    gtk_box_append(GTK_BOX(row), text_box);

    GtkWidget *sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), active);
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    g_signal_connect(sw, "notify::active", cb, user_data);
    gtk_box_append(GTK_BOX(row), sw);

    if (out_switch) *out_switch = sw;
    return row;
}






/* Final cleanup: fires exactly once when the GApplication terminates, no
 * matter how (window close, quit action, last-window-removed). The right
 * place for the DB flush — close-request only covers the window path. */


/* New-folder prompt (Ctrl+Shift+N, or the explorer right-click menu). Creates
 * a real directory in the vault, optionally under `parent_dir`. */

static void on_exp_new_file_clicked(GtkButton *btn, gpointer user_data);
static void on_exp_new_folder_clicked(GtkButton *btn, gpointer user_data);
static void on_exp_open_vault_fm_clicked(GtkButton *btn, gpointer user_data);








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
        /* Library bar stays open until its toggle icon is pressed — clicking in
         * the workspace no longer auto-closes it. */
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
    { "fullscreen", "Fullscreen", "F11", 0, 0 },
    { "focus_mode", "Focus mode", "<Control><Shift>f", 0, 0 },
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
    { "new_folder", "Create new folder", "<Control><Shift>n", 0, 0 },
    { "open_file", "Open existing file", "<Control>o", 0, 0 },
    { "save_file", "Save file", "<Control>s", 0, 0 },
    { "save_file_as", "Save file as...", "<Control><Shift>s", 0, 0 },
    { "export_pdf", "Print / Export PDF", "<Control>p", 0, 0 },
    { "toggle_search", "Toggle search bar", "<Control>f", 0, 0 },
    { "replace_text", "Replace text", "<Control>h", 0, 0 },
    { "close_tab", "Close file / tab", "<Control>w", 0, 0 },
    { "open_settings", "Open settings", "<Control>comma", 0, 0 },
    { "shortcuts_ref", "Shortcuts reference", "<Control><Shift>slash", 0, 0 },
    { "toggle_sidebar", "Toggle Sidebar", "F9", 0, 0 },
    { "inline_code", "Inline code", "<Control>k", 0, 0 },
    { "highlight", "Highlight", "<Control><Shift>h", 0, 0 },
    { "blockquote", "Blockquote", "<Control>q", 0, 0 },
    { "math", "Math", "<Control>m", 0, 0 },
    { "ordered_list", "Ordered list", "<Control><Shift>o", 0, 0 },
    { "task_list", "Task list", "<Control><Shift>t", 0, 0 }
};
#define NUM_APP_SHORTCUTS (sizeof(app_shortcuts)/sizeof(app_shortcuts[0]))

static int shortcut_listening_index = -1;
static GtkWidget *shortcut_edit_buttons[NUM_APP_SHORTCUTS] = { NULL };









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


static void on_wrap_toggled(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    GtkTextView *view = GTK_TEXT_VIEW(user_data);
    /* Full-buffer model: soft wrap is a real user choice (the old virtual-
     * paging restriction that forced it off is gone). Honor the switch. */
    gboolean active = gtk_switch_get_active(GTK_SWITCH(gobject));
    if (global_gui) {
        global_gui->wrap_lines = active;
        qirtas_pref_set_int("wrap_lines", active ? 1 : 0);
    }
    gtk_text_view_set_wrap_mode(view, active ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    /* Re-layout so disabling wrap doesn't leave dead space on the right. */
    if (global_gui) apply_editor_prefs(global_gui);
}

/* Toggle markdown conceal (hiding **, #, etc). A persistent escape hatch:
 * conceal applies scale-tagged ranges that GTK's text layout can choke on with
 * some pathological documents — turning it off renders raw markers but is
 * crash-safe. */
static void on_conceal_toggled(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    AppGui *gui = (AppGui *)user_data;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(gobject));
    qirtas_no_conceal = on ? 0 : 1;
    qirtas_pref_set_int("conceal_enabled", on ? 1 : 0);
    if (!gui || !gui->source_view) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
    if (!on) {
        /* Strip the conceal/heading tags so the raw markers show. */
        GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buf);
        GtkTextIter s, e;
        gtk_text_buffer_get_bounds(buf, &s, &e);
        const char *names[] = { "conceal", "heading1", "heading2", "heading3", "heading4" };
        for (int i = 0; i < 5; i++) {
            GtkTextTag *t = gtk_text_tag_table_lookup(table, names[i]);
            if (t) gtk_text_buffer_remove_tag(buf, t, &s, &e);
        }
    } else {
        gui_refresh_buffer_stats();
    }
}

static char current_en_font[64] = "JetBrains Mono";
static char current_ar_font[64] = "Amiri";


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

/* Data passed to toggle callback for directory rows */
typedef struct {
    GtkWidget *children_box; /* The collapsible children container */
    GtkWidget *arrow_label;  /* "▶" / "▼" label */
    gboolean   expanded;
} TreeDirData;



/* Forward declaration */

/*
 * Build a single file row widget.
 * full_path  – absolute or relative path to the file
 * name       – display name (basename)
 */

/*
 * Build a directory row with a collapsible children box.
 * Returns the outer wrapper box (row + children).
 */

/*
 * String comparison function for qsort — directories first, then alphabetical.
 */
typedef struct { char name[NAME_MAX+1]; gboolean is_dir; } DirEntry;


/*
 * Recursively fill parent_box with tree rows for all entries in dir_path.
 * depth is unused currently but kept for future indent logic.
 */

/* ============================================================
 * populate_explorer  — replaces the old flat-list version
 * ============================================================ */



/* ============================================================
 * Search helpers — simplified for tree (just re-populate on clear,
 * filter visible rows by name on search)
 * ============================================================ */


/* Recursively walk the tree GtkBox and show/hide file rows matching query */



/* Stub sort func — not used with tree box but kept to avoid linker issues */

/* ============================================================
 * BUILD UI
 * ============================================================ */

/* Zoom helpers — exported so the editor key handler (which reliably receives
 * editor shortcuts) can drive font size too; the window-level handler was being
 * pre-empted before Ctrl+=/-/0 reached it. */
/* Single source of truth for font size: gui->current_font_size, the field
 * update_editor_font() actually renders from and that the settings +/- buttons
 * and the saved "font_size" pref use. (An old file-static `current_font_size`
 * existed in parallel; the zoom shortcuts mutated *that* while the renderer read
 * the field, so Ctrl+-/+ silently did nothing. Removed — everything funnels
 * through here now and persists, so keyboard zoom and the settings control
 * stay in sync and survive restart.) */
static void apply_font_size(AppGui *gui, double new_size) {
    /* Clamp to the same 10–26 range the startup loader and the settings stepper
     * use, so keyboard zoom can't push the value past what survives a restart. */
    if (new_size < 10.0) new_size = 10.0;
    if (new_size > 26.0) new_size = 26.0;
    gui->current_font_size = new_size;
    qirtas_pref_set_int("font_size", (int)new_size);
    update_editor_font(gui);
    if (gui->font_size_value_lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)new_size);
        gtk_label_set_text(GTK_LABEL(gui->font_size_value_lbl), buf);
    }
}

void gui_zoom_in(AppGui *gui)    { apply_font_size(gui, gui->current_font_size + 1.0); }
void gui_zoom_out(AppGui *gui)   { apply_font_size(gui, gui->current_font_size - 1.0); }
void gui_zoom_reset(AppGui *gui) { apply_font_size(gui, 16.0); }

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

    /* Create New File — open a blank Untitled buffer (redesign new-file flow;
     * the file is written to the vault on first save). */
    if (match_app_shortcut("new_file", keyval, keycode, state)) {
        extern void zig_open_file(const char *filename);
        zig_open_file("Untitled");
        return TRUE;
    }

    /* Create New Folder (Ctrl+Shift+N) */
    if (match_app_shortcut("new_folder", keyval, keycode, state)) {
        explorer_begin_new_folder(gui, NULL);
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

    /* Fullscreen (window only) */
    if (match_app_shortcut("fullscreen", keyval, keycode, state)) {
        toggle_fullscreen(gui);
        return TRUE;
    }

    /* Focus mode (hide chrome / center text) */
    if (match_app_shortcut("focus_mode", keyval, keycode, state)) {
        toggle_focus_mode(gui);
        return TRUE;
    }

    /* Zoom In */
    if (match_app_shortcut("zoom_in", keyval, keycode, state)) {
        gui_zoom_in(gui);
        return TRUE;
    }

    /* Zoom Out */
    if (match_app_shortcut("zoom_out", keyval, keycode, state)) {
        gui_zoom_out(gui);
        return TRUE;
    }

    /* Reset Zoom */
    if (match_app_shortcut("reset_zoom", keyval, keycode, state)) {
        gui_zoom_reset(gui);
        return TRUE;
    }

    return FALSE;
}


void on_settings_btn_clicked(GtkButton *btn, gpointer user_data) {
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
            gtk_paned_set_shrink_end_child(GTK_PANED(gui->sidebar_editor_box), TRUE); /* shrinkable: fit half-tile */

            gtk_paned_set_position(GTK_PANED(gui->sidebar_editor_box), 220);
        } else { // Right
            gtk_paned_set_start_child(GTK_PANED(gui->sidebar_editor_box), gui->stack);
            gtk_paned_set_resize_start_child(GTK_PANED(gui->sidebar_editor_box), TRUE);
            gtk_paned_set_shrink_start_child(GTK_PANED(gui->sidebar_editor_box), TRUE); /* shrinkable: fit half-tile */

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
void zig_set_layout_dividers(int);
int zig_get_layout_dividers(void);
void zig_set_bottom_margin(int);
int zig_get_bottom_margin(void);
int zig_get_focus_mode(void);
void zig_set_editor_border(int);

/* Paper-card layout subsystem moved to gui_layout.c; geometry macros +
 * ReadModeScrollData now live in gui_internal.h alongside it. */





/* ===== Redesign UI shell (activate + handlers), ported from gui_conflict.c ===== */
typedef struct { const char *key; const char *classic; const char *modern; } IconPair;
/* forward decls (shell region) */
static void on_icon_style_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);
static void on_language_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);
static void on_trail_color_custom_toggled(GtkCheckButton *chk, gpointer user_data);
static void on_trail_color_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
static void on_pointer_color_custom_toggled(GtkCheckButton *chk, gpointer user_data);
static void on_pointer_color_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
const char *qirtas_icon(const char *key);
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





/* auto-pulled deps round 1 */






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








/* auto-pulled deps round 0 */

static void on_trail_color_custom_toggled(GtkCheckButton *chk, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gboolean active = gtk_check_button_get_active(chk);
    gui->cursor.use_custom_trail_color = active;
    if (gui->cursor.trail_color_btn) {
        gtk_widget_set_sensitive(gui->cursor.trail_color_btn, active);
    }
    save_trail_color_settings(gui);
    reset_cursor_trail(gui);
}



static void on_trail_color_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    AppGui *gui = (AppGui *)user_data;
    if (!gui || !gui->cursor.use_custom_trail_color) return;
    GtkColorDialogButton *btn = GTK_COLOR_DIALOG_BUTTON(object);
    const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba(btn);
    if (rgba) {
        gui->cursor.custom_trail_color = *rgba;
        save_trail_color_settings(gui);
        reset_cursor_trail(gui);
    }
}







/* Card gap to the screen edge (how narrow the paper card sits from the desk
 * edge when the layout border is on). Larger gap = narrower card. */


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
    { "history",     "document-open-recent-symbolic", "document-open-recent-symbolic" },
};

static void on_pointer_color_custom_toggled(GtkCheckButton *chk, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gboolean active = gtk_check_button_get_active(chk);
    gui->cursor.use_custom_pointer_color = active;
    if (gui->cursor.pointer_color_btn) {
        gtk_widget_set_sensitive(gui->cursor.pointer_color_btn, active);
    }
    save_pointer_color_settings(gui);
    apply_theme(gui, current_theme);
    update_editor_font(gui);
}

static void on_pointer_color_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    AppGui *gui = (AppGui *)user_data;
    if (!gui || !gui->cursor.use_custom_pointer_color) return;
    GtkColorDialogButton *btn = GTK_COLOR_DIALOG_BUTTON(object);
    const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba(btn);
    if (rgba) {
        gui->cursor.custom_pointer_color = *rgba;
        save_pointer_color_settings(gui);
        apply_theme(gui, current_theme);
        update_editor_font(gui);
    }
}


/* forward decls */
const char *qirtas_icon(const char *key);
static void on_header_outline_clicked(GtkButton *btn, gpointer user_data);
static GtkWidget *editor_header_icon_btn(const char *icon_key, const char *tip, GCallback cb, gpointer data);

const char *qirtas_icon(const char *key) {
    for (size_t i = 0; i < G_N_ELEMENTS(icon_table); i++) {
        if (strcmp(icon_table[i].key, key) == 0)
            return qirtas_icon_style == 1 ? icon_table[i].modern : icon_table[i].classic;
    }
    return key;
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
        /* QIRTAS_PERF=1: log main-loop callbacks over 8 ms.
         * QIRTAS_PERF=2: also log a full per-pass breakdown every stats pass
         *               and any per-keystroke edit cost over 1 ms. */
        qirtas_perf_enabled = perf_env ? atoi(perf_env) : 0;
        const char *nc_env = g_getenv("QIRTAS_NO_CONCEAL");
        if (nc_env) qirtas_no_conceal = (nc_env[0] == '1');
        else qirtas_no_conceal = qirtas_pref_get_int("conceal_enabled", 1) ? 0 : 1;
    }
    qirtas_app_language = qirtas_pref_get_int("app_language", 0);
    qirtas_icon_style   = qirtas_pref_get_int("icon_style", 0);
    if (qirtas_app_language == 1) {
        gtk_widget_set_default_direction(GTK_TEXT_DIR_RTL);
    }

    /* ── Window ── */
    GtkWidget *window = adw_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Qirtas");
    /* Restore the last window size (falls back to the original default). */
    {
        int win_w = qirtas_pref_get_int("window_width", 1180);
        int win_h = qirtas_pref_get_int("window_height", 760);
        if (win_w < 350) win_w = 1180;
        if (win_h < 250) win_h = 760;
        gtk_window_set_default_size(GTK_WINDOW(window), win_w, win_h);
    }
    gtk_widget_set_size_request(window, 350, 250);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    AppGui *gui = g_new0(AppGui, 1);
    gui->conceal_dirty_start = -1;
    gui->conceal_dirty_end = -1;
    gui->outline_dirty = TRUE;
    gui->window       = window;
    g_strlcpy(gui->current_en_font, "Inter", sizeof(gui->current_en_font));
    g_strlcpy(gui->current_ar_font, "Amiri", sizeof(gui->current_ar_font));
    /* Restore the saved editor font size (clamped to the stepper's range). */
    {
        int saved_fs = qirtas_pref_get_int("font_size", 16);
        if (saved_fs < 10) saved_fs = 10;
        if (saved_fs > 26) saved_fs = 26;
        gui->current_font_size = (double)saved_fs;
    }
    gui->search_visible = FALSE;
    gui->font_provider  = NULL;
    gui->css_provider   = NULL;
    gui->cursor.enable_trail = zig_get_cursor_trail();
    gui->show_layout_dividers = zig_get_layout_dividers();
    gui->enable_bottom_margin = zig_get_bottom_margin();
    gui->enable_editor_border = zig_get_editor_border();
    gui->enable_focus_mode = zig_get_focus_mode();
    load_trail_color_settings(gui);
    load_pointer_color_settings(gui);
    gui->tabs.active = -1;

    /* Editor preferences from the app_prefs store */
    gui->wrap_lines             = qirtas_pref_get_int("wrap_lines", 1) != 0;
    gui->show_line_numbers      = qirtas_pref_get_int("show_line_numbers", 0) != 0;
    gui->highlight_current_line = qirtas_pref_get_int("highlight_current_line", 1) != 0;
    gui->show_right_margin      = qirtas_pref_get_int("show_right_margin", 0) != 0;
    gui->right_margin_pos       = qirtas_pref_get_int("right_margin_pos", 80);
    gui->text_width_full_page   = qirtas_pref_get_int("text_width_full_page", 0) != 0;
    gui->centered_text_width    = qirtas_pref_get_int("centered_text_width", QIRTAS_TEXT_COLUMN_MAX);
    if (gui->centered_text_width < QIRTAS_TEXT_COLUMN_MIN) gui->centered_text_width = QIRTAS_TEXT_COLUMN_MIN;
    if (gui->centered_text_width > 1400) gui->centered_text_width = 1400;
    gui->show_overview_map      = qirtas_pref_get_int("show_overview_map", 0) != 0;
    gui->restore_session        = qirtas_pref_get_int("restore_session", 1) != 0;
    gui->autosave_enabled       = qirtas_pref_get_int("autosave_enabled", 1) != 0;
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

     /* 1. Resolve the bundled icons directory. Search order mirrors
      *    resolve_resource_path(): $QIRTAS_DATA_DIR, the build tree
      *    (<exe>/../../), then the system install (/usr/share/qirtas). */
    char custom_icon_path[PATH_MAX] = "";
    {
        char exe_path[PATH_MAX];
        char exe_dir[PATH_MAX] = "";
        ssize_t link_len = readlink("/proc/self/exe", exe_path, sizeof(exe_path)-1);
        if (link_len != -1) {
            exe_path[link_len] = '\0';
            g_strlcpy(exe_dir, dirname(exe_path), sizeof(exe_dir));
        }
        char cand[3][PATH_MAX];
        const char *candidates[3];
        int nc = 0;
        const char *data_dir = g_getenv("QIRTAS_DATA_DIR");
        if (data_dir && data_dir[0]) {
            snprintf(cand[nc], PATH_MAX, "%s/src/ui/icons", data_dir);
            candidates[nc] = cand[nc]; nc++;
        }
        if (exe_dir[0]) {
            snprintf(cand[nc], PATH_MAX, "%s/../../src/ui/icons", exe_dir);
            candidates[nc] = cand[nc]; nc++;
        }
        snprintf(cand[nc], PATH_MAX, "/usr/share/qirtas/src/ui/icons");
        candidates[nc] = cand[nc]; nc++;
        for (int i = 0; i < nc; i++) {
            if (access(candidates[i], F_OK) == 0) {
                g_strlcpy(custom_icon_path, candidates[i], sizeof(custom_icon_path));
                break;
            }
        }
    }

    /* 2. Retrieve active icon theme name and dynamically prepare its layout */
    gchar *theme_name = NULL;
    g_object_get(gtk_settings_get_default(), "gtk-icon-theme-name", &theme_name, NULL);

    /* This step writes a per-theme alias dir (symlinks + index.theme) into the
     * icons directory. On a read-only system install (/usr/share) that isn't
     * writable, so only attempt it when the dir is writable. Icons still
     * resolve via the bundled hicolor/ fallback in the search path below. */
    if (theme_name && strlen(theme_name) > 0 && strlen(custom_icon_path) > 0 &&
        access(custom_icon_path, W_OK) == 0) {
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

    /* 3. Register the bundled icons directory with the display icon theme.
     *    GTK looks under <path>/hicolor/... automatically, so the custom
     *    qirtas-* icons resolve from there even without the per-theme aliases. */
    GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
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

    /* Obsidian-style header actions, right-aligned: new file, new folder,
     * open the vault folder in the system file manager. */
    {
        GtkWidget *spacer = gtk_label_new(NULL);
        gtk_widget_set_hexpand(spacer, TRUE);
        gtk_box_append(GTK_BOX(exp_title_row), spacer);

        struct { const char *icon; const char *tip; GCallback cb; } acts[] = {
            { "document-new-symbolic",      qirtas_tr("New File"),                 G_CALLBACK(on_exp_new_file_clicked) },
            { "folder-new-symbolic",        qirtas_tr("New Folder"),               G_CALLBACK(on_exp_new_folder_clicked) },
            { "system-file-manager-symbolic", qirtas_tr("Open Vault in File Manager"), G_CALLBACK(on_exp_open_vault_fm_clicked) },
        };
        for (size_t i = 0; i < G_N_ELEMENTS(acts); i++) {
            GtkWidget *b = gtk_button_new_from_icon_name(acts[i].icon);
            gtk_widget_add_css_class(b, "flat");
            gtk_widget_add_css_class(b, "explorer-action-btn");
            gtk_widget_set_tooltip_text(b, acts[i].tip);
            g_signal_connect(b, "clicked", acts[i].cb, gui);
            gtk_box_append(GTK_BOX(exp_title_row), b);
        }
    }

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
    /* Editor side must be shrinkable, or the window's minimum width becomes
     * sidebar(271) + editor(543) = 820px and it can't fit a half-screen tile
     * on a tiling WM (half of 1366 = 683). Shrinking only kicks in when space
     * is constrained; normal sizes are unaffected. */
    gtk_paned_set_shrink_end_child(GTK_PANED(sidebar_editor_box), TRUE);
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
    gui->cursor.trail_area = trail_area;

    /* Transient "Copied" pill — a GtkRevealer at the bottom-centre of the
     * editor, hidden until gui_show_toast() reveals it for a moment. */
    g_toast_label = gtk_label_new("");
    gtk_widget_add_css_class(g_toast_label, "copy-toast");
    g_toast_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(g_toast_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_revealer_set_transition_duration(GTK_REVEALER(g_toast_revealer), 150);
    gtk_revealer_set_child(GTK_REVEALER(g_toast_revealer), g_toast_label);
    gtk_revealer_set_reveal_child(GTK_REVEALER(g_toast_revealer), FALSE);
    gtk_widget_set_halign(g_toast_revealer, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(g_toast_revealer, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(g_toast_revealer, 24);
    gtk_widget_set_can_target(g_toast_revealer, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(editor_overlay), g_toast_revealer);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(editor_overlay), g_toast_revealer, FALSE);

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
    /* Outline must yield when the editor area is squeezed (narrow window /
     * half-tile), otherwise the desk paned's min = card(283)+outline(254)
     * re-imposes a wide floor on the editor side. */
    gtk_paned_set_shrink_end_child(GTK_PANED(desk_paned), TRUE);

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
    /* Installed locations (makepkg, AppImage, system install): the .lang and
     * .style-scheme.xml files live at <data>/src/ui. Mirror the search order in
     * resolve_resource_path() so highlighting survives a real install, not just
     * a run from the build tree. */
    {
        const char *data_dir = g_getenv("QIRTAS_DATA_DIR");
        if (data_dir && data_dir[0]) {
            char p[1024];
            snprintf(p, sizeof(p), "%s/src/ui", data_dir);
            gtk_source_language_manager_append_search_path(lm, p);
        }
    }
    gtk_source_language_manager_append_search_path(lm, "/usr/share/qirtas/src/ui");
    gtk_source_language_manager_append_search_path(lm, "/usr/local/share/qirtas/src/ui");
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
    /* Installed locations (makepkg, AppImage, system install) — see the language
     * manager above. Without these the custom schemes are missing after install:
     * light mode falls back to a stock scheme, dark mode finds nothing. */
    {
        const char *data_dir = g_getenv("QIRTAS_DATA_DIR");
        if (data_dir && data_dir[0]) {
            char p[1024];
            snprintf(p, sizeof(p), "%s/src/ui", data_dir);
            gtk_source_style_scheme_manager_append_search_path(sm, p);
        }
    }
    gtk_source_style_scheme_manager_append_search_path(sm, "/usr/share/qirtas/src/ui");
    gtk_source_style_scheme_manager_append_search_path(sm, "/usr/local/share/qirtas/src/ui");
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
    /* CAPTURE phase so our editor shortcuts (Home/End logical-line, numbered-
     * list Enter, Alt+Up/Down, etc.) run BEFORE GtkTextView's built-in key
     * bindings, which otherwise win in the default bubble phase and shadow
     * them. on_editor_key_pressed returns FALSE for anything it doesn't claim,
     * so plain typing and the input method still reach GtkTextView normally. */
    gtk_event_controller_set_propagation_phase(editor_key_ctrl, GTK_PHASE_CAPTURE);
    gtk_widget_add_controller(source_view, editor_key_ctrl);

    /* ── Cursor-trail: wire draw function + frame-clock tick ── */
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(gui->cursor.trail_area),
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
        "Qirtas Light",
        "Qirtas Dark",
        "Paper & Ink Navy",
        "Add Custom Theme...",
        NULL
    };
    GtkWidget *theme_dropdown = gtk_drop_down_new_from_strings(themes);

    /* Reflect the saved theme (pref is the source of truth, written by
     * apply_theme on every change). */
    char *cur_theme = qirtas_pref_get_string("theme");
    const char *theme_for_idx = (cur_theme && cur_theme[0]) ? cur_theme : current_theme;
    int theme_idx = 0;
    if (strcmp(theme_for_idx, "qirtas") == 0) theme_idx = 0;
    else if (strcmp(theme_for_idx, "qirtas-dark") == 0) theme_idx = 1;
    else if (strcmp(theme_for_idx, "navy") == 0) theme_idx = 2;
    else if (strcmp(theme_for_idx, "custom") == 0) theme_idx = 3;
    g_free(cur_theme);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(theme_dropdown), theme_idx);
    
    g_signal_connect(theme_dropdown, "notify::selected", G_CALLBACK(on_theme_dropdown_changed), gui);
    
    gtk_box_append(GTK_BOX(theme_row), theme_label);
    gtk_box_append(GTK_BOX(theme_row), theme_dropdown);
    gtk_box_append(GTK_BOX(pop_box), theme_row);

    GtkWidget *font_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *font_lbl = gtk_label_new(qirtas_tr("Font Size"));
    gtk_widget_set_hexpand(font_lbl, TRUE);
    gtk_widget_set_halign(font_lbl, GTK_ALIGN_START);

    GtkWidget *stepper = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(stepper, "font-stepper");

    GtkWidget *minus_btn = gtk_button_new_with_label("−");
    gtk_widget_add_css_class(minus_btn, "font-stepper-btn");
    g_object_set_data(G_OBJECT(minus_btn), "qirtas-delta", GINT_TO_POINTER(-1));
    g_signal_connect(minus_btn, "clicked", G_CALLBACK(on_font_size_step_clicked), gui);

    char fs_buf[8];
    snprintf(fs_buf, sizeof(fs_buf), "%d", (int)gui->current_font_size);
    GtkWidget *value_lbl = gtk_label_new(fs_buf);
    gtk_widget_add_css_class(value_lbl, "font-stepper-value");
    gui->font_size_value_lbl = value_lbl;

    GtkWidget *plus_btn = gtk_button_new_with_label("+");
    gtk_widget_add_css_class(plus_btn, "font-stepper-btn");
    g_object_set_data(G_OBJECT(plus_btn), "qirtas-delta", GINT_TO_POINTER(1));
    g_signal_connect(plus_btn, "clicked", G_CALLBACK(on_font_size_step_clicked), gui);

    gtk_box_append(GTK_BOX(stepper), minus_btn);
    gtk_box_append(GTK_BOX(stepper), value_lbl);
    gtk_box_append(GTK_BOX(stepper), plus_btn);

    gtk_box_append(GTK_BOX(font_row), font_lbl);
    gtk_box_append(GTK_BOX(font_row), stepper);
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

    gtk_box_append(GTK_BOX(pop_box), settings_switch_row(
        qirtas_tr("Compact Layout"), qirtas_tr("Tighter rows in the sidebar"),
        gui->compact_mode, G_CALLBACK(on_compact_mode_toggled), gui, NULL));

    gtk_box_append(GTK_BOX(pop_box), settings_switch_row(
        qirtas_tr("Show editor border"), qirtas_tr("The floating paper card outline"),
        gui->enable_editor_border, G_CALLBACK(on_editor_border_toggled), gui, NULL));

    gtk_box_append(GTK_BOX(pop_box), settings_switch_row(
        qirtas_tr("Focus Mode"), qirtas_tr("Dim everything but the active line"),
        gui->enable_focus_mode, G_CALLBACK(on_focus_mode_toggled), gui, &gui->focus_chk));

    gtk_box_append(GTK_BOX(pop_box), settings_switch_row(
        qirtas_tr("Pointer Trail Animation"), qirtas_tr("Ink-smear caret effect"),
        gui->cursor.enable_trail, G_CALLBACK(on_trail_toggled), gui, NULL));

    /* Trail-color customization removed — the cursor trail uses the default
     * (theme caret) color. gui->cursor.use_custom_trail_color stays 0. */

    /* Pointer-color customization removed (the caret uses the theme color). */

    gtk_box_append(GTK_BOX(pop_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    /* ── EDITOR ── */
    GtkWidget *ed_lbl = gtk_label_new(qirtas_tr("EDITOR"));
    gtk_widget_add_css_class(ed_lbl, "pop-section-label");
    gtk_widget_set_halign(ed_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pop_box), ed_lbl);

    gtk_box_append(GTK_BOX(pop_box), settings_switch_row(
        qirtas_tr("Wrap Lines Automatically"), NULL,
        gui->wrap_lines, G_CALLBACK(on_wrap_toggled), gui->source_view, &gui->wrap_chk));

    gtk_box_append(GTK_BOX(pop_box), settings_switch_row(
        qirtas_tr("Conceal Markdown Markers"), NULL,
        !qirtas_no_conceal, G_CALLBACK(on_conceal_toggled), gui, NULL));

    gtk_box_append(GTK_BOX(pop_box), settings_switch_row(
        qirtas_tr("Display Line Numbers"), NULL,
        gui->show_line_numbers, G_CALLBACK(on_line_numbers_toggled), gui, NULL));

    gtk_box_append(GTK_BOX(pop_box), settings_switch_row(
        qirtas_tr("Highlight Current Line"), NULL,
        gui->highlight_current_line, G_CALLBACK(on_highlight_line_toggled), gui, NULL));

    gtk_box_append(GTK_BOX(pop_box), settings_switch_row(
        qirtas_tr("Restore Session on Startup"), NULL,
        gui->restore_session, G_CALLBACK(on_restore_session_toggled), gui, NULL));

    gtk_box_append(GTK_BOX(pop_box), settings_switch_row(
        qirtas_tr("Auto-save"), NULL,
        gui->autosave_enabled, G_CALLBACK(on_autosave_toggled), gui, NULL));

    /* Card Gap slider — the width control: 0 = full page width (edge to edge),
     * any value > 0 = centred fixed-width column. The text column is capped at
     * QIRTAS_TEXT_COLUMN_MAX and centred by growing the *card* margins
     * (apply_editor_border), so widening the window past the cap only grows the
     * desk gutter, not the wrap width — that's what keeps GtkTextView from
     * re-shaping the whole document on every resize. Auto-clamps on narrow
     * windows. (Replaces the old Text Width dropdown + Column Width slider.) */
    GtkWidget *gap_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *gap_lbl = gtk_label_new(qirtas_tr("Card Gap"));
    gtk_widget_set_halign(gap_lbl, GTK_ALIGN_START);
    GtkWidget *gap_slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                     QIRTAS_DESK_GAP_MIN, QIRTAS_DESK_GAP_MAX, 4);
    gtk_range_set_value(GTK_RANGE(gap_slider), gui->desk_gap);
    gtk_widget_set_hexpand(gap_slider, TRUE);
    gtk_scale_set_draw_value(GTK_SCALE(gap_slider), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(gap_slider), GTK_POS_RIGHT);
    g_signal_connect(gap_slider, "value-changed", G_CALLBACK(on_card_gap_slider_changed), gui);
    gtk_box_append(GTK_BOX(gap_row), gap_lbl);
    gtk_box_append(GTK_BOX(gap_row), gap_slider);
    gtk_box_append(GTK_BOX(pop_box), gap_row);

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

    GtkWidget *gd_help = gtk_label_new(qirtas_tr(
        "Advanced: needs your own Google app key (QIRTAS_GOOGLE_CLIENT_ID). "
        "For easy setup, use GitHub or the Local folder below."));
    gtk_widget_add_css_class(gd_help, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(gd_help), TRUE);
    gtk_label_set_xalign(GTK_LABEL(gd_help), 0.0);
    gtk_box_append(GTK_BOX(gd_card), gd_help);

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
        "(the local one as <name>_conflict_<timestamp>). See docs/SYNC.md.");
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

    GtkWidget *db_help = gtk_label_new(qirtas_tr(
        "Advanced: needs your own Dropbox app key (QIRTAS_DROPBOX_APP_KEY). "
        "For easy setup, use GitHub or the Local folder below."));
    gtk_widget_add_css_class(db_help, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(db_help), TRUE);
    gtk_label_set_xalign(GTK_LABEL(db_help), 0.0);
    gtk_box_append(GTK_BOX(db_card), db_help);

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

    /* Token-based connect is the reliable method (no GitHub-App permission
     * headaches). Help text + a one-click link that opens GitHub's token page
     * with the right scope pre-selected. Leave the token empty to instead sign
     * in through the browser (device flow). */
    GtkWidget *gh_help = gtk_label_new(qirtas_tr(
        "Paste a GitHub token below, or leave it empty to sign in via your browser."));
    gtk_widget_add_css_class(gh_help, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(gh_help), TRUE);
    gtk_label_set_xalign(GTK_LABEL(gh_help), 0.0);
    gtk_box_append(GTK_BOX(gh_card), gh_help);

    GtkWidget *gh_token_link = gtk_link_button_new_with_label(
        "https://github.com/settings/tokens/new?scopes=repo&description=Qirtas%20Sync",
        qirtas_tr("Get a token from GitHub \xe2\x86\x92"));
    gtk_widget_set_halign(gh_token_link, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(gh_card), gh_token_link);

    gui->github_token_entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(gui->github_token_entry), TRUE);
    g_object_set(gui->github_token_entry, "placeholder-text",
                 qirtas_tr("GitHub token (ghp_… or github_pat_…)"), NULL);
    gtk_widget_set_hexpand(gui->github_token_entry, TRUE);
    gtk_box_append(GTK_BOX(gh_card), gui->github_token_entry);

    gui->github_repo_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(gui->github_repo_entry), qirtas_tr("Repo name, owner/repo, or URL (default: qirtas-notes)"));
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
        "(the local one as <name>_conflict_<timestamp>). See docs/SYNC.md.");
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
    gtk_widget_add_css_class(actions_popover, "qirtas-menu");
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

    gtk_box_append(GTK_BOX(actions_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    // 4. Export as PDF button
    GtkWidget *btn_pop_pdf = gtk_button_new();
    gtk_widget_add_css_class(btn_pop_pdf, "pop-btn");
    gtk_widget_add_css_class(btn_pop_pdf, "menu-highlight");
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

    gtk_box_append(GTK_BOX(actions_box),
        status_menu_item(qirtas_icon("history"), qirtas_tr("File History"), NULL,
                         G_CALLBACK(on_status_menu_history), gui));

    gtk_box_append(GTK_BOX(actions_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    /* Find / Replace removed from this menu — it duplicates the in-editor
     * search (Ctrl+F) already reachable from the toolbar. */
    gtk_box_append(GTK_BOX(actions_box),
        status_menu_item(qirtas_icon("fullscreen"), qirtas_tr("Fullscreen"), "F11",
                         G_CALLBACK(on_status_menu_fullscreen), gui));
    gtk_box_append(GTK_BOX(actions_box),
        status_menu_item(qirtas_icon("prefs"), qirtas_tr("Preferences"), "Ctrl+,",
                         G_CALLBACK(on_status_menu_settings), gui));
    gtk_box_append(GTK_BOX(actions_box),
        status_menu_item(qirtas_icon("keyboard"), qirtas_tr("Keyboard Shortcuts"), "Ctrl+?",
                         G_CALLBACK(on_status_menu_shortcuts), gui));

    gtk_box_append(GTK_BOX(actions_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *quit_item = status_menu_item(qirtas_icon("quit"), qirtas_tr("Quit Qirtas"), "Ctrl+Q",
                         G_CALLBACK(on_status_menu_quit), gui);
    gtk_widget_add_css_class(quit_item, "destructive");
    gtk_box_append(GTK_BOX(actions_box), quit_item);

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
    gui->tabs.strip = tab_strip;
    /* Tab strip stays LTR even when the app runs RTL (Arabic). */
    gtk_widget_set_direction(tab_strip, GTK_TEXT_DIR_LTR);

    gui->tabs.scroll_left_btn = gtk_button_new_from_icon_name("pan-start-symbolic");
    gtk_accessible_update_property(GTK_ACCESSIBLE(gui->tabs.scroll_left_btn),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Scroll tabs left", -1);
    gtk_widget_add_css_class(gui->tabs.scroll_left_btn, "tab-scroll-btn");
    gtk_widget_set_tooltip_text(gui->tabs.scroll_left_btn, qirtas_tr("Scroll tabs left"));

    gui->tabs.bar_scroll = gtk_scrolled_window_new();
    gtk_widget_add_css_class(gui->tabs.bar_scroll, "tab-bar-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(gui->tabs.bar_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(gui->tabs.bar_scroll), TRUE);
    gtk_widget_set_hexpand(gui->tabs.bar_scroll, TRUE);
    gtk_widget_set_hexpand_set(gui->tabs.bar_scroll, TRUE);

    GtkWidget *tab_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(tab_bar, "tab-bar");
    gtk_widget_set_valign(tab_bar, GTK_ALIGN_CENTER);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(gui->tabs.bar_scroll), tab_bar);
    gui->tabs.bar_box = tab_bar;

    gui->tabs.scroll_right_btn = gtk_button_new_from_icon_name("pan-end-symbolic");
    gtk_accessible_update_property(GTK_ACCESSIBLE(gui->tabs.scroll_right_btn),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Scroll tabs right", -1);
    gtk_widget_add_css_class(gui->tabs.scroll_right_btn, "tab-scroll-btn");
    gtk_widget_set_tooltip_text(gui->tabs.scroll_right_btn, qirtas_tr("Scroll tabs right"));

    gtk_box_append(GTK_BOX(tab_strip), gui->tabs.scroll_left_btn);
    gtk_box_append(GTK_BOX(tab_strip), gui->tabs.bar_scroll);
    gtk_box_append(GTK_BOX(tab_strip), gui->tabs.scroll_right_btn);
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
    /* Restore the last-used theme (saved by apply_theme on every change). */
    {
        char *saved_theme = qirtas_pref_get_string("theme");
        if (saved_theme && saved_theme[0]) {
            g_strlcpy(current_theme, saved_theme, sizeof(current_theme));
        }
        g_free(saved_theme);
    }
    apply_theme(gui, current_theme);
    apply_focus_mode(gui);
    /* Always start with the sidebar collapsed — it's opened on demand with F9
     * (or the logo) and never persisted open. apply_focus_mode forces it
     * visible in its non-focus branch, so hide it here, after. */
    gtk_widget_set_visible(gui->sidebar, FALSE);
    gtk_window_present(GTK_WINDOW(window));
    if (qirtas_pref_get_int("window_maximized", 0)) {
        gtk_window_maximize(GTK_WINDOW(window));
    }
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

    /* A file passed on the command line (`qirtas /path/note.md`) opens last,
     * so it lands as the focused tab above any restored session tabs. */
    {
        extern const char *zig_cli_open_file(void);
        const char *cli = zig_cli_open_file();
        if (cli && cli[0]) zig_open_file(cli);
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
 * EXPLORER ROW CONTEXT MENU (right-click)
 * ============================================================ */

static void on_tree_open_clicked(GtkButton *btn, gpointer user_data) {
    popdown_ancestor_popover(GTK_WIDGET(btn));
    extern void zig_open_file(const char *filename);
    zig_open_file((const char *)user_data);
}

static void on_tree_open_in_fm_clicked(GtkButton *btn, gpointer user_data) {
    popdown_ancestor_popover(GTK_WIDGET(btn));
    const char *path = (const char *)user_data;
    /* Tree paths are vault-relative ("./Notes/x.md"); a relative GFile won't
     * resolve for the file launcher. Canonicalize against the cwd (vault root)
     * to an absolute path first. */
    char *abs = g_canonicalize_filename(path, NULL);
    GFile *file = g_file_new_for_path(abs ? abs : path);
    GtkFileLauncher *launcher = gtk_file_launcher_new(file);
    GtkWindow *parent = global_gui ? GTK_WINDOW(global_gui->window) : NULL;
    gtk_file_launcher_open_containing_folder(launcher, parent, NULL, NULL, NULL);
    g_object_unref(launcher);
    g_object_unref(file);
    g_free(abs);
}

/* Explorer header toolbar actions. */
static void on_exp_new_file_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    extern void zig_open_file(const char *filename);
    zig_open_file("Untitled");
}
static void on_exp_new_folder_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    explorer_begin_new_folder((AppGui *)user_data, NULL);
}
static void on_exp_open_vault_fm_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    char *cwd = g_get_current_dir();
    GFile *dir = g_file_new_for_path(cwd);
    GtkFileLauncher *launcher = gtk_file_launcher_new(dir);
    GtkWindow *parent = global_gui ? GTK_WINDOW(global_gui->window) : NULL;
    gtk_file_launcher_launch(launcher, parent, NULL, NULL, NULL);
    g_object_unref(launcher);
    g_object_unref(dir);
    g_free(cwd);
}

static void on_tree_history_clicked(GtkButton *btn, gpointer user_data) {
    popdown_ancestor_popover(GTK_WIDGET(btn));
    const char *path = (const char *)user_data;
    show_file_history(global_gui, path);
}

/* New Folder, created as a sibling of the right-clicked file (same directory). */
static void on_tree_new_folder_clicked(GtkButton *btn, gpointer user_data) {
    popdown_ancestor_popover(GTK_WIDGET(btn));
    const char *path = (const char *)user_data;
    char *parent = g_path_get_dirname(path);
    /* g_path_get_dirname returns "." for a bare filename → vault root. */
    explorer_begin_new_folder(global_gui, (parent && strcmp(parent, ".") != 0) ? parent : NULL);
    g_free(parent);
}

/* Rename dialog ── AdwAlertDialog with a single text entry. */
typedef struct { char *old_path; } RenameData;

static void on_rename_response(AdwAlertDialog *dlg, const char *response, gpointer user_data) {
    GtkEntry *entry = GTK_ENTRY(user_data);
    RenameData *rd = g_object_get_data(G_OBJECT(dlg), "rename_data");
    if (strcmp(response, "rename") == 0 && rd) {
        const char *new_name = gtk_editable_get_text(GTK_EDITABLE(entry));
        if (new_name && new_name[0]) {
            char *dir = g_path_get_dirname(rd->old_path);
            char *new_path = g_strdup_printf("%s/%s", strcmp(dir, ".") == 0 ? "." : dir, new_name);
            if (rename(rd->old_path, new_path) == 0) {
                /* Update tab if the renamed file is open. */
                if (global_gui) {
                    for (int i = 0; i < global_gui->tabs.count; i++) {
                        if (strcmp(global_gui->tabs.paths[i], rd->old_path) == 0) {
                            g_free(global_gui->tabs.paths[i]);
                            global_gui->tabs.paths[i] = g_strdup(new_path);
                            gui_tabs_refresh(global_gui);
                            break;
                        }
                    }
                }
                gui_refresh_explorer();
            } else {
                gui_show_toast(qirtas_tr("Rename failed"));
            }
            g_free(new_path);
            g_free(dir);
        }
    }
    if (rd) { g_free(rd->old_path); g_free(rd); }
}

static void on_tree_rename_clicked(GtkButton *btn, gpointer user_data) {
    popdown_ancestor_popover(GTK_WIDGET(btn));
    const char *path = (const char *)user_data;
    if (!global_gui) return;

    AdwAlertDialog *dlg = ADW_ALERT_DIALOG(
        adw_alert_dialog_new(qirtas_tr("Rename File"), NULL));

    char *basename = g_path_get_basename(path);
    GtkWidget *entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), basename);
    g_free(basename);

    adw_alert_dialog_set_extra_child(dlg, entry);
    adw_alert_dialog_add_responses(dlg,
        "cancel", qirtas_tr("Cancel"),
        "rename", qirtas_tr("Rename"), NULL);
    adw_alert_dialog_set_response_appearance(dlg, "rename", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(dlg, "rename");
    adw_alert_dialog_set_close_response(dlg, "cancel");

    RenameData *rd = g_new0(RenameData, 1);
    rd->old_path = g_strdup(path);
    g_object_set_data(G_OBJECT(dlg), "rename_data", rd);

    g_signal_connect(dlg, "response", G_CALLBACK(on_rename_response), entry);
    adw_dialog_present(ADW_DIALOG(dlg), global_gui->window);
}

/* Delete confirmation dialog. */
static void on_delete_confirm_response(AdwAlertDialog *dlg, const char *response, gpointer user_data) {
    (void)dlg;
    char *path = (char *)user_data;
    if (strcmp(response, "delete") == 0) {
        /* Close the tab if this file is currently open. */
        if (global_gui) {
            for (int i = 0; i < global_gui->tabs.count; i++) {
                if (strcmp(global_gui->tabs.paths[i], path) == 0) {
                    gui_tabs_close(global_gui, i);
                    break;
                }
            }
        }
        if (remove(path) != 0)
            gui_show_toast(qirtas_tr("Delete failed"));
        gui_refresh_explorer();
    }
    g_free(path);
}

static void on_tree_delete_clicked(GtkButton *btn, gpointer user_data) {
    popdown_ancestor_popover(GTK_WIDGET(btn));
    const char *path = (const char *)user_data;
    if (!global_gui) return;

    char *basename = g_path_get_basename(path);
    char *msg = g_strdup_printf(qirtas_tr("Delete \"%s\"? This cannot be undone."), basename);
    g_free(basename);

    AdwAlertDialog *dlg = ADW_ALERT_DIALOG(adw_alert_dialog_new(qirtas_tr("Delete File"), msg));
    g_free(msg);
    adw_alert_dialog_add_responses(dlg,
        "cancel", qirtas_tr("Cancel"),
        "delete", qirtas_tr("Delete"), NULL);
    adw_alert_dialog_set_response_appearance(dlg, "delete", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(dlg, "cancel");
    adw_alert_dialog_set_close_response(dlg, "cancel");

    g_signal_connect(dlg, "response", G_CALLBACK(on_delete_confirm_response), g_strdup(path));
    adw_dialog_present(ADW_DIALOG(dlg), global_gui->window);
}

/* Secondary-click handler attached to each explorer file row (gui_explorer.c).
 * user_data is the row's file path (lifetime tied to the gesture controller). */
void on_tree_file_right_click(GtkGestureClick *gesture, gint n_press,
                              gdouble x, gdouble y, gpointer user_data) {
    (void)n_press;
    GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    const char *path = (const char *)user_data;

    GtkWidget *popover = gtk_popover_new();
    gtk_widget_set_parent(popover, row);
    GdkRectangle rect = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    g_signal_connect(popover, "closed", G_CALLBACK(gtk_widget_unparent), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);

    gtk_box_append(GTK_BOX(box),
        status_menu_item(qirtas_icon("open"), qirtas_tr("Open"), NULL,
                         G_CALLBACK(on_tree_open_clicked), (gpointer)path));
    gtk_box_append(GTK_BOX(box),
        status_menu_item(qirtas_icon("filemanager"), qirtas_tr("Open with File Manager"), NULL,
                         G_CALLBACK(on_tree_open_in_fm_clicked), (gpointer)path));
    gtk_box_append(GTK_BOX(box),
        status_menu_item(qirtas_icon("folder"), qirtas_tr("New Folder"), NULL,
                         G_CALLBACK(on_tree_new_folder_clicked), (gpointer)path));
    gtk_box_append(GTK_BOX(box),
        status_menu_item(qirtas_icon("history"), qirtas_tr("File History"), NULL,
                         G_CALLBACK(on_tree_history_clicked), (gpointer)path));

    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gtk_box_append(GTK_BOX(box),
        status_menu_item("document-edit-symbolic", qirtas_tr("Rename"), NULL,
                         G_CALLBACK(on_tree_rename_clicked), (gpointer)path));
    gtk_box_append(GTK_BOX(box),
        status_menu_item("edit-delete-symbolic", qirtas_tr("Delete"), NULL,
                         G_CALLBACK(on_tree_delete_clicked), (gpointer)path));

    gtk_popover_set_child(GTK_POPOVER(popover), box);
    gtk_popover_popup(GTK_POPOVER(popover));
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
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
    g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    gtk_source_finalize();
    return status;
}

/* gui_index_all_files / gui_index_file / gui_remove_file_from_index
 * moved to src/gui/gui_index.c */


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

    /* Drop any table reveal state tied to the outgoing document before the
     * content swap, so its mark can't re-grid a stale range. */
    gui_table_reset_reveal(buf);

    /* Wholesale content swap: invalidate any deferred conceal/wiki/HR passes
     * queued against the OUTGOING document. Their generation guard cancels them
     * instead of letting them run apply_tag/insert with now-stale iterators
     * (the "Invalid text buffer iterator" / cross-buffer assertion storm). The
     * passes scheduled below capture the new generation and run normally. */
    global_gui->buffer_generation++;

    g_signal_handlers_block_by_func(buf, on_insert_text_before, global_gui);
    g_signal_handlers_block_by_func(buf, on_insert_text_after, global_gui);
    g_signal_handlers_block_by_func(buf, on_delete_range_before, global_gui);
    g_signal_handlers_block_by_func(buf, on_delete_range_after, global_gui);
    g_signal_handlers_block_by_func(buf, on_buffer_changed, global_gui);

    gtk_text_buffer_set_text(buf, text ? text : "", text ? len : 0);
    reset_cursor_trail(global_gui);

    parse_and_render_hrs(buf, global_gui);
    parse_and_render_code_pills(buf, global_gui);
    parse_and_render_tables(buf, global_gui);
    /* A freshly loaded file is not a user edit — don't mark it dirty (was
     * lighting the unsaved dot on every tab the moment it opened). */
    gtk_text_buffer_set_modified(buf, FALSE);

    g_signal_handlers_unblock_by_func(buf, on_insert_text_before, global_gui);
    g_signal_handlers_unblock_by_func(buf, on_insert_text_after, global_gui);
    g_signal_handlers_unblock_by_func(buf, on_delete_range_before, global_gui);
    g_signal_handlers_unblock_by_func(buf, on_delete_range_after, global_gui);
    g_signal_handlers_unblock_by_func(buf, on_buffer_changed, global_gui);

    update_all_paragraphs_direction(buf);
    apply_wiki_link_tags(buf);
    /* on_buffer_changed was blocked, so conceal + word/char/line stats never
     * ran for the load. Trigger them explicitly (deferred). */
    gui_refresh_buffer_stats();
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

/* Place the caret WITHOUT scrolling the viewport to it. on_mark_set bails out
 * of its deferred scroll-to-cursor while loading_viewport is set, so toggling
 * it across the select_range suppresses the scroll. Used by undo/redo so the
 * page stays put instead of snapping to the restored caret (the "undo jumps
 * the screen up" bug — the baseline snapshot's caret is at 0,0). The reload's
 * own line-anchored viewport restore then keeps the visible text in place. */
void gui_set_cursor_position_quiet(int line, int col) {
    if (!global_gui) { gui_set_cursor_position(line, col); return; }
    gboolean prev = global_gui->loading_viewport;
    global_gui->loading_viewport = TRUE;
    gui_set_cursor_position(line, col);
    global_gui->loading_viewport = prev;
}

int gui_get_absolute_cursor_line(void) {
    if (!global_gui || !global_source_view) return 1;
    int line = 1, col = 0;
    gui_get_cursor_position(&line, &col);
    return line;
}

/* Whether the periodic/on-edit autosave triggers should fire. Manual save
 * (Ctrl+S, via gui_manual_save) always saves regardless of this setting. */
gboolean gui_autosave_enabled(void) {
    return !global_gui || global_gui->autosave_enabled;
}

void gui_trigger_autosave(void) {
    if (!global_gui || !global_source_view) return;

    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));

    /* Nothing changed since the last save → don't re-serialize, re-encrypt and
     * hit disk. The 30s background autosave thread fires regardless of edits,
     * so without this guard an idle editor saved every 30s for no reason. */
    if (!gtk_text_buffer_get_modified(buf)) return;

    gui_set_sync_status("Saving...");

    /* Serialize the source of truth (Zig's doc_buf), NOT the decorated GTK view.
     * View-only child anchors (HR / table header / code-fence pill) replace the
     * raw markdown on screen, and gtk_text_buffer_get_text() omits those anchors
     * — so serializing the view returned an empty string for every decorated
     * line and silently destroyed it on save. zig_save_document writes doc_buf,
     * which retains the raw markdown for those lines. */
    extern int zig_save_document(void);
    int status = zig_save_document();

    if (status == 0) {
        gui_set_sync_status("Saved");
        gtk_text_buffer_set_modified(buf, FALSE);  /* clears the unsaved dot + skips next idle save */
        gui_tabs_refresh(global_gui);
        gui_show_toast(qirtas_tr("Saved"));        /* visible confirmation */

        if (global_gui->tabs.active >= 0)
            gui_history_record(global_gui->tabs.paths[global_gui->tabs.active]);
    } else {
        gui_set_sync_status("Save Failed");
        gui_show_toast(qirtas_tr("Save failed"));
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
    if (!title) return;
    if (global_window)
        gtk_window_set_title(GTK_WINDOW(global_window), title);
    if (global_path_label) {
        char fname[256];
        strncpy(fname, title, sizeof(fname));
        fname[sizeof(fname) - 1] = '\0';
        char *sfx = strstr(fname, " - Qirtas");
        if (sfx) *sfx = '\0';
        gtk_label_set_text(GTK_LABEL(global_path_label), fname);
        if (global_gui) {
            gui_tabs_add_or_select(global_gui, fname);
            if (global_gui->stats_file_val)
                gtk_label_set_text(GTK_LABEL(global_gui->stats_file_val), fname);
            /* Card-header breadcrumb: render the vault-relative path as
             * folder / sub / file. */
            if (global_gui->breadcrumb_label) {
                gchar **parts = g_strsplit(fname, "/", -1);
                gchar *crumb = g_strjoinv(" / ", parts);
                gtk_label_set_text(GTK_LABEL(global_gui->breadcrumb_label), crumb);
                g_free(crumb);
                g_strfreev(parts);
            }
        }
    }
}

void gui_set_sync_status(const char *status) {
    if (!global_sync_label || !status) return;

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

void gui_show_editor(void) {
    if (global_gui && global_gui->stack && global_gui->btn_editor)
        set_active_tab(global_gui, global_gui->btn_editor, "editor");
    /* Focus the text view so the caret is live immediately. Without this, a
     * freshly opened (esp. empty / Untitled) document left focus on the file
     * tree, and the first click in the editor only moved focus — the caret
     * didn't land until a second interaction (right-click / save). */
    if (global_gui && global_gui->source_view)
        gtk_widget_grab_focus(global_gui->source_view);
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






