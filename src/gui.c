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

/* Forward declarations — static functions defined later in this file */
static void reorder_main_layout(AppGui *gui);
static int get_line_at_y(AppGui *gui, double y);
static void on_settings_btn_clicked(GtkButton *btn, gpointer user_data);
static void on_scroll_changed(GtkAdjustment *adj, gpointer user_data);

/* Forward declarations — gui_sync.c callbacks */
void on_sync_connect_clicked(GtkButton *btn, gpointer user_data);
void on_sync_now_clicked(GtkButton *btn, gpointer user_data);
void on_dropbox_connect_clicked(GtkButton *btn, gpointer user_data);
void on_dropbox_now_clicked(GtkButton *btn, gpointer user_data);
void on_github_connect_clicked(GtkButton *btn, gpointer user_data);
void on_github_now_clicked(GtkButton *btn, gpointer user_data);
void on_local_sync_clicked(GtkButton *btn, gpointer user_data);

/* File-scope variables */
static int seconds_elapsed = 0;
static void *global_add_popover_widgets = NULL;
AppGui    *global_gui         = NULL;
GtkWidget *global_window      = NULL;
GtkWidget *global_source_view = NULL;
GtkWidget *global_sync_label  = NULL;
GtkWidget *global_path_label  = NULL;
GtkWidget *global_time_label  = NULL;

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
static void select_position_range(AppGui *gui, Position start, Position end) {
    if (!gui || !gui->source_view) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
    GtkTextIter start_iter, end_iter;

    int rel_start_line = start.line - gui->viewport_start_line;
    int rel_end_line = end.line - gui->viewport_start_line;
    if (rel_start_line < 0) rel_start_line = 0;
    if (rel_end_line < 0) rel_end_line = 0;

    gtk_text_buffer_get_iter_at_line(buf, &start_iter, rel_start_line);
    gtk_text_buffer_get_iter_at_line(buf, &end_iter, rel_end_line);

    for (int i = 0; i < start.col; i++) {
        if (gtk_text_iter_ends_line(&start_iter) || gtk_text_iter_is_end(&start_iter)) break;
        gtk_text_iter_forward_char(&start_iter);
    }
    for (int i = 0; i < end.col; i++) {
        if (gtk_text_iter_ends_line(&end_iter) || gtk_text_iter_is_end(&end_iter)) break;
        gtk_text_iter_forward_char(&end_iter);
    }

    gtk_text_buffer_select_range(buf, &start_iter, &end_iter);
}

static Position iter_to_position(GtkTextIter *iter) {
    Position pos = { 0, 0 };
    if (!global_gui || !iter) return pos;
    pos.line = global_gui->viewport_start_line + gtk_text_iter_get_line(iter);
    pos.col = gtk_text_iter_get_line_offset(iter);
    return pos;
}

static Position advance_position(Position pos, const char *text) {
    if (!text) return pos;
    const char *p = text;
    while (*p) {
        gunichar c = g_utf8_get_char(p);
        if (c == '\n') {
            pos.line += 1;
            pos.col = 0;
        } else {
            pos.col += 1;
        }
        p = g_utf8_next_char(p);
    }
    return pos;
}



/* FFI — Called from Zig */
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
void gui_tabs_close(AppGui *gui, int index);
void gui_tabs_add_or_select(AppGui *gui, const char *filepath);
void load_sync_credentials(AppGui *gui);

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
void zig_github_connect(void);
void zig_github_now(void);
int  zig_github_check_status(void);
void zig_github_disconnect(void);
void zig_local_sync_now(void);
void zig_set_layout_dividers(int enabled);
int zig_get_layout_dividers(void);
void zig_set_bottom_margin(int enabled);
int zig_get_bottom_margin(void);
void zig_set_focus_mode(int enabled);
int zig_get_focus_mode(void);

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
    "* { font-family: 'Inter', 'Lora', 'Merriweather', 'Cairo', 'system-ui', sans-serif; }\n"
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
#define GHOST_MIN_DIST 3.0
#define SMEAR_STIFFNESS 0.30
#define SMEAR_TRAILING_LENGTH 0.30
#define SMEAR_TRAILING_WIDTH 0.1
#define GHOST_DECAY (1.0 / (SMEAR_TRAILING_LENGTH * 60.0))


static GdkRGBA trail_color_for_theme(void) {
    if (global_gui && global_gui->use_custom_trail_color) {
        return global_gui->custom_trail_color;
    }
    if (strcmp(current_theme, "sepia") == 0)
        return (GdkRGBA){ 164.0/255.0,  46.0/255.0, 121.0/255.0, 1.0 };
    if (strcmp(current_theme, "things") == 0)
        return (GdkRGBA){  46.0/255.0, 128.0/255.0, 242.0/255.0, 1.0 };
    if (strcmp(current_theme, "typewriter-light") == 0)
        return (GdkRGBA){ 184.0/255.0,  46.0/255.0,  46.0/255.0, 1.0 };
    if (strcmp(current_theme, "typewriter-dark") == 0)
        return (GdkRGBA){ 255.0/255.0, 107.0/255.0, 107.0/255.0, 1.0 };
    if (strcmp(current_theme, "qirtas") == 0)
        return (GdkRGBA){  31.0/255.0, 111.0/255.0, 235.0/255.0, 1.0 };
    if (strcmp(current_theme, "midnight") == 0)
        return (GdkRGBA){ 170.0/255.0, 196.0/255.0, 255.0/255.0, 1.0 };
    return (GdkRGBA){ 255.0/255.0, 121.0/255.0, 198.0/255.0, 1.0 };
}




/* ── Draw a single ghost caret (rounded-rect pill) at (gx, gy) with given alpha ── */
static void draw_ghost_caret(cairo_t *cr,
                             double gx, double gy,
                             double caret_w, double caret_h,
                             double r, double g_col, double b,
                             double alpha)
{
    if (alpha <= 0.01) return;

    /* Caret width: keep it narrow but allow tapering down to 0.5px */
    double w      = caret_w < 0.5 ? 0.5 : caret_w;
    double h      = caret_h;
    double radius = w / 2.0;
    if (radius < 0.25) radius = 0.25;
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

static void draw_smear_segment(cairo_t *cr,
                               double xa, double ya, double w_a,
                               double xb, double yb, double w_b,
                               double caret_h,
                               double r, double g, double b,
                               double alpha_a, double alpha_b)
{
    cairo_save(cr);

    // Create linear gradient from center of A to center of B
    cairo_pattern_t *pat = cairo_pattern_create_linear(xa + w_a / 2.0, ya + caret_h / 2.0,
                                                       xb + w_b / 2.0, yb + caret_h / 2.0);
    cairo_pattern_add_color_stop_rgba(pat, 0.0, r, g, b, alpha_a);
    cairo_pattern_add_color_stop_rgba(pat, 1.0, r, g, b, alpha_b);
    cairo_set_source(cr, pat);

    cairo_new_path(cr);
    if (xa < xb) {
        cairo_move_to(cr, xa, ya);
        cairo_line_to(cr, xa + w_a, ya);
        cairo_line_to(cr, xb, yb);
        cairo_line_to(cr, xb + w_b, yb);
        cairo_line_to(cr, xb + w_b, yb + caret_h);
        cairo_line_to(cr, xb, yb + caret_h);
        cairo_line_to(cr, xa + w_a, ya + caret_h);
        cairo_line_to(cr, xa, ya + caret_h);
    } else {
        cairo_move_to(cr, xa + w_a, ya);
        cairo_line_to(cr, xa, ya);
        cairo_line_to(cr, xb + w_b, yb);
        cairo_line_to(cr, xb, yb);
        cairo_line_to(cr, xb, yb + caret_h);
        cairo_line_to(cr, xb + w_b, yb + caret_h);
        cairo_line_to(cr, xa, ya + caret_h);
        cairo_line_to(cr, xa + w_a, ya + caret_h);
    }
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_pattern_destroy(pat);
    cairo_restore(cr);
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

static gboolean content_has_rtl(const char *text, int len) {
    int check = len < 500 ? len : 500;
    for (int i = 0; i < check - 1; i++) {
        unsigned char c = (unsigned char)text[i];
        // Arabic block starts at U+0600 → UTF-8 0xD8 0x80
        // Hebrew block starts at U+0590 → UTF-8 0xD6 0x90
        if (c == 0xD8 || c == 0xD9 || c == 0xD6 || c == 0xD7) return TRUE;
    }
    return FALSE;
}

static void update_all_paragraphs_direction(GtkTextBuffer *buf) {
    // Fast path: skip entirely if no RTL characters in visible content
    const char *sample = zig_get_document_text();
    if (sample) {
        gboolean has_rtl = content_has_rtl(sample, 500);
        zig_free_document_text(sample);
        if (!has_rtl) return;
    }

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

static void on_layout_dividers_toggled(GtkCheckButton *chk, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gboolean active = gtk_check_button_get_active(chk);
    gui->show_layout_dividers = active;
    zig_set_layout_dividers(active ? 1 : 0);
    apply_layout_dividers(gui);
}

static void on_bottom_margin_toggled(GtkCheckButton *chk, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gboolean active = gtk_check_button_get_active(chk);
    gui->enable_bottom_margin = active;
    zig_set_bottom_margin(active ? 1 : 0);
    if (gui->source_view) {
        gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(gui->source_view), active ? 160 : 0);
    }
}

static void apply_editor_border(AppGui *gui) {
    if (!gui || !gui->scrolled) return;
    
    // If focus mode is globally enabled, it forces no borders and no margins
    if (gui->enable_focus_mode) {
        gtk_widget_add_css_class(gui->scrolled, "focus-mode");
        gtk_widget_set_margin_start(gui->scrolled, 0);
        gtk_widget_set_margin_end(gui->scrolled, 0);
        gtk_widget_set_margin_top(gui->scrolled, 0);
        gtk_widget_set_margin_bottom(gui->scrolled, 0);
        return;
    }

    if (gui->enable_editor_border) {
        gtk_widget_remove_css_class(gui->scrolled, "focus-mode");
        gtk_widget_set_margin_start(gui->scrolled, 28);
        gtk_widget_set_margin_end(gui->scrolled, 28);
        gtk_widget_set_margin_top(gui->scrolled, 24);
        gtk_widget_set_margin_bottom(gui->scrolled, 20);
    } else {
        gtk_widget_add_css_class(gui->scrolled, "focus-mode");
        gtk_widget_set_margin_start(gui->scrolled, 0);
        gtk_widget_set_margin_end(gui->scrolled, 0);
        gtk_widget_set_margin_top(gui->scrolled, 0);
        gtk_widget_set_margin_bottom(gui->scrolled, 0);
    }
}

static void on_editor_border_toggled(GtkCheckButton *chk, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gboolean active = gtk_check_button_get_active(chk);
    gui->enable_editor_border = active;
    zig_set_editor_border(active ? 1 : 0);
    apply_editor_border(gui);
}

void apply_focus_mode(AppGui *gui) {
    if (!gui || !gui->scrolled || !gui->sidebar || !gui->main_vertical_box || !gui->bottom_bar_widget || !gui->sidebar_editor_box) return;

    if (gui->enable_focus_mode) {
        // Hide sidebar
        gtk_widget_set_visible(gui->sidebar, FALSE);

        // Reorder layout with status bar on top
        reorder_main_layout(gui);

        // Remove margins around editor scroll
        gtk_widget_set_margin_start(gui->scrolled, 0);
        gtk_widget_set_margin_end(gui->scrolled, 0);
        gtk_widget_set_margin_top(gui->scrolled, 0);
        gtk_widget_set_margin_bottom(gui->scrolled, 0);

        // Add focus-mode CSS class (removes border, border-radius, shadow)
        gtk_widget_add_css_class(gui->scrolled, "focus-mode");

        // Disable layout settings that conflict with focus mode
        if (gui->sb_pos_dropdown) gtk_widget_set_sensitive(gui->sb_pos_dropdown, FALSE);
        if (gui->sb_side_dropdown) gtk_widget_set_sensitive(gui->sb_side_dropdown, FALSE);
        if (gui->divider_chk) gtk_widget_set_sensitive(gui->divider_chk, FALSE);
        if (gui->btn_sidebar_toggle) gtk_widget_set_sensitive(gui->btn_sidebar_toggle, TRUE);

    } else {
        // Show sidebar
        gtk_widget_set_visible(gui->sidebar, TRUE);

        // Restore layout positions
        reorder_main_layout(gui);

        // Remove focus-mode CSS class and restore margins if border is enabled
        apply_editor_border(gui);

        // Re-enable layout settings
        if (gui->sb_pos_dropdown) gtk_widget_set_sensitive(gui->sb_pos_dropdown, TRUE);
        if (gui->sb_side_dropdown) gtk_widget_set_sensitive(gui->sb_side_dropdown, TRUE);
        if (gui->divider_chk) gtk_widget_set_sensitive(gui->divider_chk, TRUE);
        if (gui->btn_sidebar_toggle) gtk_widget_set_sensitive(gui->btn_sidebar_toggle, TRUE);
    }

    // Update the checkbox state in settings if visible/existing
    if (gui->focus_chk) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(gui->focus_chk), gui->enable_focus_mode);
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

static void on_pointer_color_custom_toggled(GtkCheckButton *chk, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    gboolean active = gtk_check_button_get_active(chk);
    gui->use_custom_pointer_color = active;
    if (gui->pointer_color_btn) {
        gtk_widget_set_sensitive(gui->pointer_color_btn, active);
    }
    save_pointer_color_settings(gui);
    apply_theme(gui, current_theme);
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

static void on_status_bar_new_file_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));

    extern void zig_open_file(const char *filename);
    zig_open_file("Untitled");
}

static void on_status_bar_save_file_clicked(GtkButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    gui_manual_save(gui);
}

static void on_status_bar_open_file_clicked(GtkButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Existing File");
    gtk_file_dialog_open(dialog, GTK_WINDOW(gui->window), NULL, on_open_dialog_response, gui);
}

static void on_status_bar_export_pdf_clicked(GtkButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));

    qirtas_export_to_pdf(gui);
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
                                         GTK_TEXT_WINDOW_WIDGET,
                                         (int)x, (int)y, &bx, &by);
    int trailing = 0;
    return gtk_text_view_get_iter_at_position(GTK_TEXT_VIEW(gui->source_view), iter, &trailing, bx, by);
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
                debug_get_iter_at_offset(buf, &start_iter, start_char, "apply_regex_conceal_start");
                debug_get_iter_at_offset(buf, &end_iter, start_char + delim_len, "apply_regex_conceal_end");
                gtk_text_buffer_apply_tag(buf, conceal_tag, &start_iter, &end_iter);
                
                debug_get_iter_at_offset(buf, &start_iter, end_char - delim_len, "apply_regex_conceal_end_start");
                debug_get_iter_at_offset(buf, &end_iter, end_char, "apply_regex_conceal_end_end");
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
                debug_get_iter_at_offset(buf, &start_iter, start_char, "apply_regex_conceal_local_start");
                debug_get_iter_at_offset(buf, &end_iter, start_char + delim_len, "apply_regex_conceal_local_end");
                gtk_text_buffer_apply_tag(buf, conceal_tag, &start_iter, &end_iter);
                
                debug_get_iter_at_offset(buf, &start_iter, end_char - delim_len, "apply_regex_conceal_local_end_start");
                debug_get_iter_at_offset(buf, &end_iter, end_char, "apply_regex_conceal_local_end_end");
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



void gui_push_undo_snapshot(void) {
    if (!global_gui || global_gui->loading_viewport) return;

    int line = 0;
    int col = 0;
    gui_get_cursor_position(&line, &col);
    zig_undo_push(line, col);
}

void gui_set_buffer_modified(gboolean modified) {
    if (!global_source_view) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));
    gtk_text_buffer_set_modified(buf, modified);
}

void gui_reload_viewport(void) {
    if (!global_gui) return;
    if (global_gui->loading_viewport) return;
    request_viewport_position(global_gui, global_gui->viewport_start_line);
}

static int get_page_size(int total_lines) {
    if (total_lines > 4000) return 250;
    if (total_lines > 2000) return 300;
    return 400;
}

void request_viewport_position(AppGui *gui, int abs_line) {
    if (!gui) return;

    int current_local_line = -1;
    int current_local_col = -1;
    int current_abs_line = -1;
    gdouble current_scroll_value = 0.0;
    gdouble current_vadjustment = 0.0;
    if (gui->source_view) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
        GtkTextIter cursor_iter;
        gtk_text_buffer_get_iter_at_mark(buf, &cursor_iter, gtk_text_buffer_get_insert(buf));
        current_local_line = gtk_text_iter_get_line(&cursor_iter);
        current_local_col = gtk_text_iter_get_line_offset(&cursor_iter);
        current_abs_line = gui->viewport_start_line + current_local_line;
    }
    if (gui->vadjustment) {
        current_scroll_value = gtk_adjustment_get_value(gui->vadjustment);
        current_vadjustment = gtk_adjustment_get_page_size(gui->vadjustment);
    }

    int top_h = 0, bottom_h = 0;
    if (gui->top_spacer || gui->bottom_spacer) {
        gtk_widget_get_size_request(gui->top_spacer, NULL, &top_h);
        gtk_widget_get_size_request(gui->bottom_spacer, NULL, &bottom_h);
    }

    g_print(
        "REQUEST_VIEWPORT_POSITION requested_line=%d current_local_line=%d current_local_col=%d current_abs_line=%d current_scroll_value=%g current_vadjustment=%g viewport=%d-%d top_spacer=%d bottom_spacer=%d\n",
        abs_line,
        current_local_line,
        current_local_col,
        current_abs_line,
        current_scroll_value,
        current_vadjustment,
        gui->viewport_start_line,
        gui->viewport_end_line,
        top_h,
        bottom_h
    );

    if (gui->loading_viewport) {
        gui->pending_line = abs_line;
        return;
    }

    int page_size = get_page_size(gui->total_virtual_lines);
    int margin = page_size / 5;
    if (abs_line >= gui->viewport_start_line + margin &&
        abs_line <  gui->viewport_end_line   - margin) {
        return;
    }

    int new_start = MAX(0, abs_line - (page_size / 2));
    int new_end   = MIN(gui->total_virtual_lines, new_start + page_size);
    
    if (new_end >= gui->total_virtual_lines) {
        new_end = gui->total_virtual_lines;
        new_start = MAX(0, new_end - 400);
    }

    if (new_start == gui->viewport_start_line &&
        new_end   == gui->viewport_end_line) {
        return;
    }

    g_print(
        "REQUEST_VIEWPORT_POSITION chosen_viewport_start=%d chosen_viewport_end=%d\n",
        new_start,
        new_end
    );

    load_viewport_page(gui, new_start);
}

void on_buffer_changed(GtkTextBuffer *buf, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (gui && gui->loading_viewport) return;
    
    zig_undo_commit();
    check_and_insert_hr(buf, gui);
    
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    gchar *text = gtk_text_buffer_get_text(buf, &start, &end, TRUE);
    
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
    /* Defer the conceal pass to an idle callback so Pango finishes
     * updating its line-layout cache before we apply/remove any tags.
     * Applying tags synchronously inside the 'changed' signal can leave
     * GTK with a stale byte-index cache, causing the fatal:
     *   "Byte index N is off the end of the line" abort. */
    g_idle_add_once((GSourceOnceFunc)update_conceal_markdown_all, buf);
}

static void on_buffer_modified_changed(GtkTextBuffer *buf, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (gui && gui->loading_viewport) return;
    if (gui && gui->active_tab_index != -1 && gui->active_tab_index < gui->num_tabs) {
        gboolean is_modified = gtk_text_buffer_get_modified(buf);
        gui->tab_modified[gui->active_tab_index] = is_modified;
        gui_tabs_refresh(gui);
    }
}


/* Idle callback: fires after GTK has re-laid-out the text view following
 * conceal-tag changes, so get_iter_location() returns the correct rect. */



static void on_cursor_position_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    (void)object;
    (void)pspec;
    (void)user_data;
    /* Redundant: on_mark_set already updates formatting layout on cursor moves. */
}

void on_insert_text_before(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (gui && gui->loading_viewport) return;
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

        gchar both_chars[3] = { c, closing, 0 };
        Position pos = iter_to_position(location);
        zig_insert_text(pos, both_chars);
        request_viewport_position(global_gui, global_gui->viewport_start_line);
        gui_set_cursor_position(pos.line + 1, pos.col + 1);
        zig_undo_commit();
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

static void apply_paragraph_alignment(GtkTextBuffer *buf, GtkJustification justification) {
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buf);
    GtkTextTag *tag_left = gtk_text_tag_table_lookup(table, "align-left");
    if (!tag_left) {
        tag_left = gtk_text_buffer_create_tag(buf, "align-left", "justification", GTK_JUSTIFY_LEFT, NULL);
    }
    GtkTextTag *tag_center = gtk_text_tag_table_lookup(table, "align-center");
    if (!tag_center) {
        tag_center = gtk_text_buffer_create_tag(buf, "align-center", "justification", GTK_JUSTIFY_CENTER, NULL);
    }
    GtkTextTag *tag_right = gtk_text_tag_table_lookup(table, "align-right");
    if (!tag_right) {
        tag_right = gtk_text_buffer_create_tag(buf, "align-right", "justification", GTK_JUSTIFY_RIGHT, NULL);
    }
    GtkTextTag *tag_justify = gtk_text_tag_table_lookup(table, "align-justify");
    if (!tag_justify) {
        tag_justify = gtk_text_buffer_create_tag(buf, "align-justify", "justification", GTK_JUSTIFY_FILL, NULL);
    }

    GtkTextTag *target_tag = NULL;
    switch (justification) {
        case GTK_JUSTIFY_LEFT:   target_tag = tag_left; break;
        case GTK_JUSTIFY_CENTER: target_tag = tag_center; break;
        case GTK_JUSTIFY_RIGHT:  target_tag = tag_right; break;
        case GTK_JUSTIFY_FILL:   target_tag = tag_justify; break;
        default: return;
    }

    GtkTextIter start, end;
    if (!gtk_text_buffer_get_selection_bounds(buf, &start, &end)) {
        gtk_text_buffer_get_iter_at_mark(buf, &start, gtk_text_buffer_get_insert(buf));
        end = start;
    }

    gint start_line = gtk_text_iter_get_line(&start);
    gint end_line = gtk_text_iter_get_line(&end);

    gtk_text_buffer_begin_user_action(buf);

    for (gint l = start_line; l <= end_line; l++) {
        GtkTextIter line_start, line_end;
        gtk_text_buffer_get_iter_at_line(buf, &line_start, l);
        line_end = line_start;
        gtk_text_iter_forward_to_line_end(&line_end);

        gtk_text_buffer_remove_tag(buf, tag_left, &line_start, &line_end);
        gtk_text_buffer_remove_tag(buf, tag_center, &line_start, &line_end);
        gtk_text_buffer_remove_tag(buf, tag_right, &line_start, &line_end);
        gtk_text_buffer_remove_tag(buf, tag_justify, &line_start, &line_end);

        gtk_text_buffer_apply_tag(buf, target_tag, &line_start, &line_end);
    }

    gtk_text_buffer_end_user_action(buf);
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
        if (!gtk_text_buffer_get_selection_bounds(buf, &start, &end)) {
            gtk_text_buffer_get_iter_at_mark(buf, &start, gtk_text_buffer_get_insert(buf));
            end = start;
        }

        Position start_pos = iter_to_position(&start);
        Position end_pos = iter_to_position(&end);
        zig_replace_range(start_pos, end_pos, text);
        request_viewport_position(gui, gui->viewport_start_line);

        Position cursor_pos = advance_position(start_pos, text);
        gui_set_cursor_position(cursor_pos.line + 1, cursor_pos.col);
        zig_undo_commit();
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
            char *text = gtk_text_buffer_get_text(buf, &start, &end, TRUE);
            if (text) {
                FILE *f = fopen(path, "w");
                if (f) {
                    fputs(text, f);
                    fclose(f);
                    gui_set_sync_status("Saved");
                    
                    gtk_text_buffer_set_modified(buf, FALSE);

                    char cwd[4096];
                    const char *display_name = path;
                    char *allocated_display_name = NULL;
                    if (getcwd(cwd, sizeof(cwd)) != NULL) {
                        size_t cwd_len = strlen(cwd);
                        if (strncmp(path, cwd, cwd_len) == 0) {
                            if (path[cwd_len] == '/') {
                                display_name = path + cwd_len + 1;
                            } else if (path[cwd_len] == '\0') {
                                display_name = path;
                            }
                        } else {
                            allocated_display_name = g_path_get_basename(path);
                            display_name = allocated_display_name;
                        }
                    } else {
                        allocated_display_name = g_path_get_basename(path);
                        display_name = allocated_display_name;
                    }
                    
                    if (gui->active_tab_index != -1 && strcmp(gui->open_tabs[gui->active_tab_index], "Untitled") == 0) {
                        g_free(gui->open_tabs[gui->active_tab_index]);
                        gui->open_tabs[gui->active_tab_index] = g_strdup(display_name);

                        g_free(gui->tab_contents[gui->active_tab_index]);
                        gui->tab_contents[gui->active_tab_index] = g_strdup(text);
                        gui->tab_modified[gui->active_tab_index] = FALSE;
                    }
                    
                    if (allocated_display_name) {
                        g_free(allocated_display_name);
                    }
                    
                    extern void zig_open_file(const char *filename);
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

void clear_selection_formatting(GtkTextBuffer *buf) {
    GtkTextIter start, end;
    if (!gtk_text_buffer_get_selection_bounds(buf, &start, &end)) {
        return;
    }

    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buf);
    GtkTextTag *tag_left = gtk_text_tag_table_lookup(table, "align-left");
    GtkTextTag *tag_center = gtk_text_tag_table_lookup(table, "align-center");
    GtkTextTag *tag_right = gtk_text_tag_table_lookup(table, "align-right");
    GtkTextTag *tag_justify = gtk_text_tag_table_lookup(table, "align-justify");
    if (tag_left) gtk_text_buffer_remove_tag(buf, tag_left, &start, &end);
    if (tag_center) gtk_text_buffer_remove_tag(buf, tag_center, &start, &end);
    if (tag_right) gtk_text_buffer_remove_tag(buf, tag_right, &start, &end);
    if (tag_justify) gtk_text_buffer_remove_tag(buf, tag_justify, &start, &end);

    gchar *text = gtk_text_buffer_get_text(buf, &start, &end, TRUE);
    if (text && strlen(text) > 0) {
        GString *cleaned = g_string_new("");
        gchar *p = text;
        while (*p) {
            if (strncmp(p, "**", 2) == 0) {
                p += 2;
            } else if (strncmp(p, "~~", 2) == 0) {
                p += 2;
            } else if (strncmp(p, "==", 2) == 0) {
                p += 2;
            } else if (strncmp(p, "<u>", 3) == 0) {
                p += 3;
            } else if (strncmp(p, "</u>", 4) == 0) {
                p += 4;
            } else if (*p == '*' || *p == '`' || *p == '$') {
                p++;
            } else {
                g_string_append_c(cleaned, *p);
                p++;
            }
        }

        Position start_pos = iter_to_position(&start);
        Position end_pos = iter_to_position(&end);
        zig_replace_range(start_pos, end_pos, cleaned->str);
        request_viewport_position(global_gui, global_gui->viewport_start_line);

        Position select_end = advance_position(start_pos, cleaned->str);
        select_position_range(global_gui, start_pos, select_end);
        zig_undo_commit();

        g_string_free(cleaned, TRUE);
    }
    g_free(text);
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

static void init_app_shortcuts(void) {
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) == SQLITE_OK) {
        const char *create_sql = "CREATE TABLE IF NOT EXISTS keyboard_shortcuts ("
                                 "action_name TEXT PRIMARY KEY, "
                                 "shortcut_val TEXT NOT NULL);";
        sqlite3_exec(db, create_sql, NULL, NULL, NULL);

        for (size_t i = 0; i < NUM_APP_SHORTCUTS; i++) {
            sqlite3_stmt *stmt = NULL;
            const char *select_sql = "SELECT shortcut_val FROM keyboard_shortcuts WHERE action_name = ?;";
            if (sqlite3_prepare_v2(db, select_sql, -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, app_shortcuts[i].action_id, -1, SQLITE_STATIC);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char *val = (const char *)sqlite3_column_text(stmt, 0);
                    if (val && strlen(val) > 0) {
                        strncpy(app_shortcuts[i].shortcut_str, val, sizeof(app_shortcuts[i].shortcut_str) - 1);
                        app_shortcuts[i].shortcut_str[sizeof(app_shortcuts[i].shortcut_str) - 1] = '\0';
                    }
                }
                sqlite3_finalize(stmt);
            }
        }
        sqlite3_close(db);
    }

    for (size_t i = 0; i < NUM_APP_SHORTCUTS; i++) {
        parse_shortcut_string(app_shortcuts[i].shortcut_str, &app_shortcuts[i].keyval, &app_shortcuts[i].state);
    }
}

gboolean keycode_matches_latin_keyval(guint keycode, guint target_keyval) {
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return FALSE;

    guint target_lower = gdk_keyval_to_lower(target_keyval);

    /* Try modifier groups: no mod, shift, alt (AltGr), shift+alt */
    static const GdkModifierType groups[] = {
        0,
        GDK_SHIFT_MASK,
        GDK_ALT_MASK,
        GDK_SHIFT_MASK | GDK_ALT_MASK
    };

    for (gint g = 0; g < 4; g++) {
        guint kv = 0;
        gint effective_group = 0;
        gint level = 0;
        GdkModifierType consumed = 0;
        if (gdk_display_translate_key(display, keycode, groups[g], 0,
                                      &kv, &effective_group, &level, &consumed)) {
            if (gdk_keyval_to_lower(kv) == target_lower)
                return TRUE;
        }
    }
    return FALSE;
}


gboolean match_app_shortcut(const char *action_id, guint keyval, guint keycode, GdkModifierType state) {
    for (size_t i = 0; i < NUM_APP_SHORTCUTS; i++) {
        if (strcmp(app_shortcuts[i].action_id, action_id) == 0) {
            GdkModifierType clean_state = state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_ALT_MASK | GDK_SUPER_MASK);
            if (clean_state == app_shortcuts[i].state) {
                if (keyval == app_shortcuts[i].keyval || keycode_matches_latin_keyval(keycode, app_shortcuts[i].keyval)) {
                    return TRUE;
                }
            }
            break;
        }
    }
    return FALSE;
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

static void show_keybindings_window(AppGui *gui) {
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Keyboard Shortcuts");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 550, 600);
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
    #define KB_ROW(shortcut, description, act_id) { \
        GtkWidget *_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12); \
        gchar *_curr_shortcut = NULL; \
        int _idx = -1; \
        if (act_id && strlen(act_id) > 0) { \
            for (int i = 0; i < (int)NUM_APP_SHORTCUTS; i++) { \
                if (strcmp(app_shortcuts[i].action_id, act_id) == 0) { \
                    _idx = i; \
                    _curr_shortcut = get_pretty_shortcut_string(app_shortcuts[i].shortcut_str); \
                    break; \
                } \
            } \
        } \
        GtkWidget *_k = gtk_label_new(_curr_shortcut ? _curr_shortcut : shortcut); \
        gtk_widget_add_css_class(_k, "kb-key"); \
        gtk_widget_set_halign(_k, GTK_ALIGN_START); \
        if (_curr_shortcut) g_free(_curr_shortcut); \
        GtkWidget *_d = gtk_label_new(description); \
        gtk_widget_add_css_class(_d, "kb-desc"); \
        gtk_widget_set_halign(_d, GTK_ALIGN_START); \
        gtk_widget_set_hexpand(_d, TRUE); \
        gtk_box_append(GTK_BOX(_row), _k); \
        gtk_box_append(GTK_BOX(_row), _d); \
        if (_idx >= 0) { \
            shortcut_value_labels[_idx] = _k; \
            GtkWidget *_edit_btn = gtk_button_new_with_label("Edit"); \
            gtk_widget_add_css_class(_edit_btn, "pop-btn"); \
            g_signal_connect(_edit_btn, "clicked", G_CALLBACK(on_edit_shortcut_clicked), GINT_TO_POINTER(_idx)); \
            shortcut_edit_buttons[_idx] = _edit_btn; \
            gtk_box_append(GTK_BOX(_row), _edit_btn); \
        } \
        gtk_box_append(GTK_BOX(vbox), _row); \
    }

    KB_SECTION("1. TEXT FORMATTING & ALIGNMENT")
    KB_ROW("Ctrl + B",       "Bold text", "bold");
    KB_ROW("Ctrl + I",       "Italic text", "italic");
    KB_ROW("Ctrl + U",       "Underline text", "underline");
    KB_ROW("Ctrl + Shift+X", "Strikethrough text", "strikethrough");
    KB_ROW("Ctrl + Shift+L", "Left align text", "left_align");
    KB_ROW("Ctrl + Shift+E", "Center align text", "center_align");
    KB_ROW("Ctrl + Shift+R", "Right align text", "right_align");
    KB_ROW("Ctrl + Shift+J", "Justify text", "justify");
    KB_ROW("Ctrl + \\",      "Clear formatting", "clear_format");
    KB_ROW("Ctrl + K",       "Inline code", "inline_code");
    KB_ROW("Ctrl + H",       "Highlight", "highlight");
    KB_ROW("Ctrl + Q",       "Blockquote", "blockquote");
    KB_ROW("Ctrl + M",       "Math", "math");
    KB_ROW("Ctrl + Shift+O", "Ordered list", "ordered_list");
    KB_ROW("Ctrl + Shift+T", "Task list", "task_list");

    KB_SECTION("2. ZOOM & TYPOGRAPHY")
    KB_ROW("Ctrl + = / +",   "Zoom In", "zoom_in");
    KB_ROW("Ctrl + -",       "Zoom Out", "zoom_out");
    KB_ROW("Ctrl + 0",       "Reset Zoom", "reset_zoom");
    KB_ROW("F11",            "Fullscreen / Focus Mode", "fullscreen");

    KB_SECTION("3. ADVANCED EDITING")
    KB_ROW("Ctrl + C",       "Copy selected text", "copy");
    KB_ROW("Ctrl + X",       "Cut selected text", "cut");
    KB_ROW("Ctrl + V",       "Paste text", "paste");
    KB_ROW("Ctrl + Shift+V", "Paste plain text", "paste_plain");
    KB_ROW("Ctrl + Z",       "Undo last action", "undo");
    KB_ROW("Ctrl + Y / Shift+Z", "Redo last action", "redo");
    KB_ROW("Ctrl + A",       "Select all text", "select_all");
    KB_ROW("Ctrl + D / Enter", "Duplicate current line", "duplicate_line");
    KB_ROW("Alt + Up",       "Move line up", "move_line_up");
    KB_ROW("Alt + Down",     "Move line down", "move_line_down");
    KB_ROW("Ctrl + /",       "Toggle comment", "toggle_comment");

    KB_SECTION("4. NAVIGATION & DELETION")
    KB_ROW("Ctrl + Backspace", "Delete previous word", "delete_prev_word");
    KB_ROW("Ctrl + Delete",  "Delete next word", "delete_next_word");
    KB_ROW("Ctrl + Shift+K", "Delete current line", "delete_line");
    KB_ROW("Ctrl + Left",    "Move word left", "move_word_left");
    KB_ROW("Ctrl + Right",   "Move word right", "move_word_right");
    KB_ROW("Home",           "Move to start of line", "move_line_start");
    KB_ROW("End",            "Move to end of line", "move_line_end");
    KB_ROW("Ctrl + Home",    "Move to start of document", "move_doc_start");
    KB_ROW("Ctrl + End",     "Move to end of document", "move_doc_end");

    KB_SECTION("5. FILE & SEARCH")
    KB_ROW("Ctrl + N",       "Create new file", "new_file");
    KB_ROW("Ctrl + O",       "Open existing file", "open_file");
    KB_ROW("Ctrl + S",       "Save file", "save_file");
    KB_ROW("Ctrl + Shift+S", "Save file as...", "save_file_as");
    KB_ROW("Ctrl + P",       "Print / Export PDF", "export_pdf");
    KB_ROW("Ctrl + F",       "Toggle search bar", "toggle_search");
    KB_ROW("Ctrl + H",       "Replace text", "replace_text");
    KB_ROW("Ctrl + W / F4",  "Close file / tab", "close_tab");
    KB_ROW("Ctrl + ,",       "Open settings", "open_settings");
    KB_ROW("Ctrl + ?",       "Shortcuts reference", "shortcuts_ref");
    KB_ROW("Ctrl + \\",      "Toggle Sidebar", "toggle_sidebar");

    #undef KB_SECTION
    #undef KB_ROW

    GtkEventController *dialog_key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(dialog_key_ctrl, "key-pressed", G_CALLBACK(on_settings_key_pressed), gui);
    gtk_widget_add_controller(dialog, dialog_key_ctrl);

    g_signal_connect(dialog, "destroy", G_CALLBACK(on_kb_window_destroy), NULL);

    gtk_window_present(GTK_WINDOW(dialog));
}

gboolean keypress_has_text_modifier(GdkModifierType state) {
    return (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK)) != 0;
}

void insert_text_pair(GtkTextBuffer *buf, const char *open, const char *close) {
    GtkTextIter start, end;
    AppGui *gui = global_gui;

    if (gtk_text_buffer_get_selection_bounds(buf, &start, &end)) {
        gchar *selected = gtk_text_buffer_get_text(buf, &start, &end, TRUE);
        gchar *wrapped = g_strconcat(open, selected, close, NULL);
        Position start_pos = iter_to_position(&start);
        Position end_pos = iter_to_position(&end);
        zig_replace_range(start_pos, end_pos, wrapped);
        request_viewport_position(gui, gui->viewport_start_line);
        select_position_range(gui, start_pos, advance_position(start_pos, wrapped));
        zig_undo_commit();

        g_free(selected);
        g_free(wrapped);
    } else {
        GtkTextIter cursor;
        gtk_text_buffer_get_iter_at_mark(buf, &cursor, gtk_text_buffer_get_insert(buf));
        gchar *pair = g_strconcat(open, close, NULL);
        Position cursor_pos = iter_to_position(&cursor);
        zig_insert_text(cursor_pos, pair);
        request_viewport_position(gui, gui->viewport_start_line);
        Position after_open = advance_position(cursor_pos, open);
        gui_set_cursor_position(after_open.line + 1, after_open.col);
        zig_undo_commit();
        g_free(pair);
    }
}

gboolean maybe_skip_closing_pair(GtkTextBuffer *buf, const char *close) {
    GtkTextIter cursor, next;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor, gtk_text_buffer_get_insert(buf));
    next = cursor;
    if (!gtk_text_iter_forward_chars(&next, g_utf8_strlen(close, -1))) return FALSE;

    gchar *next_text = gtk_text_buffer_get_text(buf, &cursor, &next, TRUE);
    gboolean matches = (strcmp(next_text, close) == 0);
    g_free(next_text);

    if (matches) {
        gtk_text_buffer_place_cursor(buf, &next);
        return TRUE;
    }
    return FALSE;
}

gboolean maybe_delete_empty_pair(GtkTextBuffer *buf) {
    GtkTextIter cursor, prev, next;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor, gtk_text_buffer_get_insert(buf));

    prev = cursor;
    next = cursor;
    if (!gtk_text_iter_backward_char(&prev) || !gtk_text_iter_forward_char(&next)) return FALSE;

    gchar *around = gtk_text_buffer_get_text(buf, &prev, &next, TRUE);
    gboolean is_pair =
        strcmp(around, "()") == 0 ||
        strcmp(around, "[]") == 0 ||
        strcmp(around, "{}") == 0 ||
        strcmp(around, "\"\"") == 0 ||
        strcmp(around, "``") == 0;

    if (is_pair) {
        Position start_pos = iter_to_position(&prev);
        Position end_pos = iter_to_position(&next);
        zig_delete_range(start_pos, end_pos);
        request_viewport_position(global_gui, global_gui->viewport_start_line);
        gui_set_cursor_position(start_pos.line + 1, start_pos.col);
        zig_undo_commit();
    }

    g_free(around);
    return is_pair;
}

void toggle_comment_current_line(GtkTextBuffer *buf) {
    (void)buf;
    GtkTextIter cursor_iter;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor_iter, gtk_text_buffer_get_insert(buf));
    int abs_line = global_gui->viewport_start_line + gtk_text_iter_get_line(&cursor_iter);
    int col = gtk_text_iter_get_line_offset(&cursor_iter);

    int len = 0;
    extern const char *zig_get_text_for_line_range(int start_line, int end_line, int *out_len);
    const char *line_text = zig_get_text_for_line_range(abs_line, abs_line + 1, &len);
    if (!line_text) return;

    char *line_dup = g_strndup(line_text, len);

    int ws = 0;
    while (line_dup[ws] == ' ' || line_dup[ws] == '\t') ws++;

    char *content = line_dup + ws;
    gboolean is_commented = FALSE;
    size_t content_len = strlen(content);
    while (content_len > 0 && (content[content_len - 1] == '\n' || content[content_len - 1] == '\r')) {
        content[content_len - 1] = '\0';
        content_len--;
    }

    if (strncmp(content, "<!--", 4) == 0) {
        if (content_len >= 7 && strcmp(content + content_len - 3, "-->") == 0) {
            is_commented = TRUE;
        }
    }

    char *new_text = NULL;
    if (is_commented) {
        char *inner = g_strndup(content + 4, content_len - 7);
        char *start_p = inner;
        if (start_p[0] == ' ') start_p++;
        size_t inner_len = strlen(start_p);
        if (inner_len > 0 && start_p[inner_len - 1] == ' ') {
            start_p[inner_len - 1] = '\0';
        }
        char *indent = g_strndup(line_dup, ws);
        new_text = g_strconcat(indent, start_p, "\n", NULL);
        g_free(inner);
        g_free(indent);
    } else {
        char *indent = g_strndup(line_dup, ws);
        new_text = g_strconcat(indent, "<!-- ", content, " -->\n", NULL);
        g_free(indent);
    }

    Position start = { abs_line, 0 };
    Position end = { abs_line + 1, 0 };
    zig_replace_range(start, end, new_text);
    g_free(line_dup);
    g_free(new_text);

    // Reload viewport
    request_viewport_position(global_gui, global_gui->viewport_start_line);

    // Place cursor
    gui_set_cursor_position(abs_line + 1, col);
    zig_undo_commit();
}

void on_insert_text_after(GtkTextBuffer *buf, GtkTextIter *location, gchar *text, gint len, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (gui && gui->loading_viewport) return;

    int end_offset = gtk_text_iter_get_offset(location);
    int char_len = g_utf8_strlen(text, len);
    int start_offset = end_offset - char_len;
    if (start_offset < 0) start_offset = 0;

    GtkTextIter start_iter;
    debug_get_iter_at_offset(buf, &start_iter, start_offset, "on_insert_text_after");

    int start_line = gui->viewport_start_line + gtk_text_iter_get_line(&start_iter);
    int start_col = gtk_text_iter_get_line_offset(&start_iter);

    Position pos = { start_line, start_col };
    gchar *text_dup = g_strndup(text, len);
    extern void zig_insert_text(Position pos, const char *text);
    zig_insert_text(pos, text_dup);
    g_free(text_dup);

    gui_set_sync_status("Not Synced");
    g_idle_add_once((GSourceOnceFunc)update_all_paragraphs_direction, buf);
    
    extern void apply_wiki_link_tags_local(GtkTextBuffer *buf);
    apply_wiki_link_tags_local(buf);
}

void on_delete_range_before(GtkTextBuffer *buf, GtkTextIter *start, GtkTextIter *end, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (gui && gui->loading_viewport) return;

    int start_line = gui->viewport_start_line + gtk_text_iter_get_line(start);
    int start_col = gtk_text_iter_get_line_offset(start);

    int end_line = gui->viewport_start_line + gtk_text_iter_get_line(end);
    int end_col = gtk_text_iter_get_line_offset(end);

    Position p_start = { start_line, start_col };
    Position p_end = { end_line, end_col };

    extern void zig_delete_range(Position start, Position end);
    zig_delete_range(p_start, p_end);
}

void on_delete_range_after(GtkTextBuffer *buf, GtkTextIter *start, GtkTextIter *end, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (gui && gui->loading_viewport) return;
    (void)start; (void)end;
    gui_set_sync_status("Not Synced");
    g_idle_add_once((GSourceOnceFunc)update_all_paragraphs_direction, buf);
    extern void apply_wiki_link_tags_local(GtkTextBuffer *buf);
    apply_wiki_link_tags_local(buf);
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
        if (gui->btn_status_actions) gtk_widget_set_visible(gui->btn_status_actions, is_editor);

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

    if (global_gui && global_gui->virtual_scroll_enabled) {
        /* Virtual scrolling uses fixed logical line heights, so soft wrap must stay off. */
        g_signal_handlers_block_by_func(btn, G_CALLBACK(on_wrap_toggled), user_data);
        gtk_check_button_set_active(btn, FALSE);
        g_signal_handlers_unblock_by_func(btn, G_CALLBACK(on_wrap_toggled), user_data);
        gtk_text_view_set_wrap_mode(view, GTK_WRAP_NONE);
    } else {
        gboolean active = gtk_check_button_get_active(btn);
        gtk_text_view_set_wrap_mode(view, active ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    }
}




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
        extern void zig_open_file(const char *filename);
        zig_open_file("Untitled");
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
        gui_manual_save(gui);
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
        gui->current_font_size += 1.0;
        update_editor_font(gui);
        return TRUE;
    }

    /* Zoom Out */
    if (match_app_shortcut("zoom_out", keyval, keycode, state)) {
        if (gui->current_font_size > 6.0) {
            gui->current_font_size -= 1.0;
            update_editor_font(gui);
        }
        return TRUE;
    }

    /* Reset Zoom */
    if (match_app_shortcut("reset_zoom", keyval, keycode, state)) {
        gui->current_font_size = 16.0;
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
    (void)gobject;
    (void)pspec;
    AppGui *gui = (AppGui *)user_data;
    reorder_main_layout(gui);
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

            gtk_paned_set_position(GTK_PANED(gui->sidebar_editor_box), 260);
        } else { // Right
            gtk_paned_set_start_child(GTK_PANED(gui->sidebar_editor_box), gui->stack);
            gtk_paned_set_resize_start_child(GTK_PANED(gui->sidebar_editor_box), TRUE);
            gtk_paned_set_shrink_start_child(GTK_PANED(gui->sidebar_editor_box), FALSE);

            gtk_paned_set_end_child(GTK_PANED(gui->sidebar_editor_box), gui->sidebar);
            gtk_paned_set_resize_end_child(GTK_PANED(gui->sidebar_editor_box), FALSE);
            gtk_paned_set_shrink_end_child(GTK_PANED(gui->sidebar_editor_box), FALSE);

            int width = gtk_widget_get_width(gui->sidebar_editor_box);
            if (width > 260) {
                gtk_paned_set_position(GTK_PANED(gui->sidebar_editor_box), width - 260);
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

typedef struct { GtkAdjustment *vadj; gulong sig_id; } UnblockData;

static gboolean unblock_scroll_idle(gpointer user_data) {
    UnblockData *d = user_data;
    g_signal_handler_unblock(d->vadj, d->sig_id);
    g_free(d);
    return G_SOURCE_REMOVE;
}

static void gui_reset_preferred_x(AppGui *gui) {
    if (!gui || !gui->source_view) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
    GtkTextIter iter;
    GtkTextMark *insert = gtk_text_buffer_get_insert(buf);
    gtk_text_buffer_get_iter_at_mark(buf, &iter, insert);
    gtk_text_buffer_place_cursor(buf, &iter);
}

static void set_spacers_with_compensation(AppGui *gui, int new_top_h, int new_bottom_h);
static void viewport_set_range(AppGui *gui, int start_line, int end_line);
void load_viewport_page(
    AppGui *gui,
    int new_start
) {

    if (!gui) return;
    if (gui->loading_viewport) return;

    gui->buffer_generation++;

    int saved_abs_line = -1;
    int saved_abs_col = 0;
    int current_local_line = -1;
    int current_local_col = -1;
    if (gui->source_view) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
        GtkTextIter cursor_iter;
        gtk_text_buffer_get_iter_at_mark(buf, &cursor_iter, gtk_text_buffer_get_insert(buf));
        current_local_line = gtk_text_iter_get_line(&cursor_iter);
        current_local_col = gtk_text_iter_get_line_offset(&cursor_iter);
        saved_abs_line = gui->viewport_start_line + current_local_line;
        saved_abs_col = gtk_text_iter_get_line_offset(&cursor_iter);
    }

    g_print(
        "BEFORE_RELOAD old_viewport_start=%d old_viewport_end=%d current_local_line=%d current_local_col=%d saved_abs_line=%d saved_abs_col=%d pending_line=%d pending_col=%d\n",
        gui->viewport_start_line,
        gui->viewport_end_line,
        current_local_line,
        current_local_col,
        saved_abs_line,
        saved_abs_col,
        gui->pending_line,
        gui->pending_col
    );

    gdouble saved_scroll = 0;
    if (gui->vadjustment) {
        g_signal_handler_block(gui->vadjustment, gui->scroll_signal_id);
        g_object_freeze_notify(G_OBJECT(gui->vadjustment));
        saved_scroll = gtk_adjustment_get_value(gui->vadjustment);
    }
    gui->loading_viewport = TRUE;

    int page_size = get_page_size(gui->total_virtual_lines);
    int new_end = new_start + page_size;

    if (new_end > gui->total_virtual_lines) {
        new_end = gui->total_virtual_lines;
        new_start = MAX(0, new_end - page_size);
    }

    gui->last_scroll_requested_line = -1;

    extern const char *zig_get_text_for_line_range(
        int start_line,
        int end_line,
        int *out_len
    );

    int new_len = 0;

    const char *new_text =
        zig_get_text_for_line_range(
            new_start,
            new_end,
            &new_len
        );

    GtkTextBuffer *buf =
        gtk_text_view_get_buffer(
            GTK_TEXT_VIEW(gui->source_view)
        );
    g_print(
    	"LOAD_VIEWPORT start=%d end=%d\n",
    	new_start,
    	new_end
	);

    g_print(
        "VIEWPORT_CONTENT start=%d end=%d len=%d first80=\"%.80s\"\n",
        new_start,
        new_end,
        new_len,
        new_text ? new_text : "(null)"
    );

    viewport_set_range(gui, new_start, new_end);

    gtk_text_buffer_set_text(buf, new_text ? new_text : "", new_len);

    HrRenderData *hr_d = g_new0(HrRenderData, 1);
    hr_d->gui = gui;
    hr_d->generation = gui->buffer_generation;
    g_idle_add(idle_render_hrs_cb, hr_d);

    update_all_paragraphs_direction(buf);
    apply_wiki_link_tags(buf);

    if (saved_abs_line >= 0) {
        int rel_line = saved_abs_line - new_start;
        int line_count = gtk_text_buffer_get_line_count(buf);
        if (line_count > 0) {
            if (rel_line < 0) rel_line = 0;
            if (rel_line >= line_count) rel_line = line_count - 1;

            GtkTextIter iter;
            gtk_text_buffer_get_iter_at_line(buf, &iter, rel_line);
            int line_bytes = gtk_text_iter_get_bytes_in_line(&iter);
            int safe_col = saved_abs_col;
            if (safe_col < 0) safe_col = 0;
            if (safe_col > line_bytes) safe_col = line_bytes;
            gtk_text_buffer_get_iter_at_line_offset(buf, &iter, rel_line, safe_col);
            gtk_text_buffer_select_range(buf, &iter, &iter);

            g_print(
                "AFTER_RELOAD new_viewport_start=%d new_viewport_end=%d restored_rel_line=%d restored_rel_col=%d restored_abs_line=%d pending_line=%d pending_col=%d\n",
                new_start,
                new_end,
                rel_line,
                safe_col,
                new_start + rel_line,
                gui->pending_line,
                gui->pending_col
            );
        }
    }

    gui->loading_viewport = FALSE;

    if (gui->vadjustment) {
        gtk_adjustment_set_value(gui->vadjustment, saved_scroll);
        g_object_thaw_notify(G_OBJECT(gui->vadjustment));

        UnblockData *d = g_new(UnblockData, 1);
        d->vadj   = gui->vadjustment;
        d->sig_id = gui->scroll_signal_id;
        g_idle_add(unblock_scroll_idle, d);
    }

    if (gui->pending_line >= 0) {
        int line = gui->pending_line;
        int col = gui->pending_col;
        g_print(
            "LOAD_VIEWPORT_PENDING before_gui_set_cursor_position pending_line=%d pending_col=%d line=%d col=%d\n",
            gui->pending_line,
            gui->pending_col,
            line,
            col
        );
        gui->pending_line = -1;
        gui->pending_col = -1;
        gui_set_cursor_position(line + 1, col < 0 ? 0 : col);
    }
}

static int   last_loaded_start = -1;
static guint scroll_timer_id   = 0;
static int   queued_line       = -1;
static gint64 last_load_time_ms  = 0;
static int    last_load_direction = 0;

static gboolean fire_scroll(gpointer user_data) {
    AppGui *gui = user_data;
    scroll_timer_id = 0;

    if (queued_line >= 0) {
        int line = queued_line;
        queued_line = -1;
        
        int page_size = get_page_size(gui->total_virtual_lines);
        int new_start = MAX(0, line - (page_size / 2));
        int direction = (new_start > last_loaded_start) ? 1 : -1;

        gint64 now = g_get_monotonic_time() / 1000;

        // Only apply direction lock if we have a valid previous load
        if (last_loaded_start >= 0 && (direction != last_load_direction) && (now - last_load_time_ms) < 200) {
            return G_SOURCE_REMOVE;
        }
        
        if (new_start == last_loaded_start) return G_SOURCE_REMOVE;
        
        last_loaded_start   = new_start;
        last_load_direction = direction;
        last_load_time_ms   = now;
        
        request_viewport_position(gui, gui->viewport_start_line + line);
    }
    return G_SOURCE_REMOVE;
}

static void on_scroll_changed(GtkAdjustment *adj, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    if (!gui->virtual_scroll_enabled) return;
    if (gui->loading_viewport) return;

    static gdouble last_value = -1.0;
    gdouble value = gtk_adjustment_get_value(adj);

    // Skip if negligible movement (layout reflow noise)
    if (fabs(value - last_value) < 1.0) return;
    last_value = value;

    double page_size = gtk_adjustment_get_page_size(adj);
    int center_line = get_line_at_y(gui, value + page_size / 2.0);
    g_print("ON_SCROLL_CHANGED scroll_y=%g page_size=%g center_line=%d viewport_start_line=%d\n",
            value, page_size, center_line, gui->viewport_start_line);
    if (center_line == gui->last_scroll_requested_line) return;
    
    queued_line = center_line;
    if (scroll_timer_id != 0) {
        g_source_remove(scroll_timer_id);
    }
    scroll_timer_id = g_timeout_add(60, fire_scroll, gui);
}
static void set_spacers_with_compensation(AppGui *gui, int new_top_h, int new_bottom_h) {
    if (!gui->vadjustment) {
        gtk_widget_set_size_request(gui->top_spacer, -1, new_top_h);
        gtk_widget_set_size_request(gui->bottom_spacer, -1, new_bottom_h);
        return;
    }
    
    int old_top_h = 0;
    gtk_widget_get_size_request(gui->top_spacer, NULL, &old_top_h);
    if (old_top_h < 0) old_top_h = 0;
    
    int delta = new_top_h - old_top_h;
    
    g_signal_handler_block(gui->vadjustment, gui->scroll_signal_id);
    
    gtk_widget_set_size_request(gui->top_spacer, -1, new_top_h);
    gtk_widget_set_size_request(gui->bottom_spacer, -1, new_bottom_h);
    
    gtk_widget_queue_resize(gui->virtual_layout_box);
    
    
    UnblockData *d = g_new(UnblockData, 1);
    d->vadj   = gui->vadjustment;
    d->sig_id = gui->scroll_signal_id;
    g_idle_add(unblock_scroll_idle, d);
}


gboolean simulate_crash_cb(gpointer user_data) {
    static int state = 0;
    AppGui *gui = (AppGui *)user_data;
    extern void zig_open_file(const char *filename);

    switch (state) {
        case 0:
            g_print("\nSTEP 1\nAction:\nOpen large file (gui.c)\n\n");
            zig_open_file("gui.c");
            state = 1;
            break;
        case 1:
            g_print("\nSTEP 2\nAction:\nMove cursor to long line (line 679 column 305)\n\n");
            gui_set_cursor_position(679, 305);
            state = 2;
            break;
        case 2:
            g_print("\nSTEP 3\nAction:\nSwitch to small file (As-Built Specification Document.md)\n\n");
            zig_open_file("As-Built Specification Document.md");
            state = 3;
            break;
        default:
            if (state >= 3 && state < 35) {
                g_print("\nSTEP 4.%d\nAction:\nTrigger vertical navigation Down key\n\n", state - 2);
                g_signal_emit_by_name(gui->source_view, "move-cursor", GTK_MOVEMENT_DISPLAY_LINES, 1, FALSE);
                state++;
            } else {
                g_print("\nSTEP 5\nAction:\nDone with steps\n\n");
                return G_SOURCE_REMOVE;
            }
            break;
    }
    return G_SOURCE_CONTINUE;
}

static void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    init_app_shortcuts();

    g_object_set(gtk_settings_get_default(), 
                 "gtk-cursor-blink", FALSE, 
                 "gtk-cursor-aspect-ratio", 0.02, 
                 NULL);

    /* ── Window ── */
    GtkWidget *window = adw_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Qirtas");
    gtk_window_set_default_size(GTK_WINDOW(window), 1180, 760);
    gtk_widget_set_size_request(window, 350, 250);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    AppGui *gui = g_new0(AppGui, 1);
    gui->last_scroll_requested_line = -1;
    gui->pending_line = -1;
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
    gtk_icon_theme_add_search_path(icon_theme, "/home/sinkeat/projects/lawh/src/ui/icons");
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

    /* 1. Global Workspace Search Entry */
    GtkWidget *exp_search = gtk_search_entry_new();
    gtk_widget_add_css_class(exp_search, "workspace-search");
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(exp_search), "Search in workspace...");
    gtk_box_append(GTK_BOX(sidebar), exp_search);
    gui->exp_search_entry = exp_search;
    g_signal_connect(exp_search, "search-changed", G_CALLBACK(on_explorer_search_changed), gui);

    /* Parent the add popover to the sidebar so it can be displayed */
    gtk_widget_set_parent(w->popover, sidebar);


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
    gui->scrolled = scrolled;
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(scrolled, TRUE);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_widget_set_halign(scrolled, GTK_ALIGN_FILL);
    gtk_widget_set_valign(scrolled, GTK_ALIGN_FILL);
    gtk_widget_set_margin_start(scrolled, 28);
    gtk_widget_set_margin_end(scrolled, 28);
    gtk_widget_set_margin_top(scrolled, 24);
    gtk_widget_set_margin_bottom(scrolled, 20);
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

    gtk_box_append(GTK_BOX(editor_page), editor_overlay);

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
    gtk_widget_set_margin_start(source_view, 0);
    gtk_widget_set_margin_end(source_view, 0);
    gtk_widget_set_margin_top(source_view, 0);
    gtk_widget_set_margin_bottom(source_view, 0);
    gtk_widget_add_css_class(source_view, "editor-source");
    gtk_box_append(GTK_BOX(virtual_layout_box), source_view);
    gui->source_view = source_view;

    gui->bottom_spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(gui->bottom_spacer, FALSE);
    gtk_widget_set_valign(gui->bottom_spacer, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(virtual_layout_box), gui->bottom_spacer);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), virtual_layout_box);

    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
    gui->vadjustment = vadj;
    gui->scroll_signal_id = g_signal_connect(vadj, "value-changed", G_CALLBACK(on_scroll_changed), gui);

    /* Typography & Render spacing (line height equivalent) */
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(source_view), 64);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(source_view), 64);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(source_view), 46);
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
    gtk_widget_add_tick_callback(
        gui->source_view,
        (GtkTickCallback)on_cursor_tick,
        gui,
        NULL  /* GDestroyNotify */
    );



    /* ============================================================
     * SETTINGS WINDOW
     * ============================================================ */
    GtkWidget *pop_box  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(pop_box, "settings-sheet-body");
    gtk_widget_set_margin_start(pop_box, 14);
    gtk_widget_set_margin_end(pop_box, 14);
    gtk_widget_set_margin_top(pop_box, 14);
    gtk_widget_set_margin_bottom(pop_box, 14);

    /* --- VAULT GROUP --- */
    GtkWidget *vault_lbl = gtk_label_new("VAULT");
    gtk_widget_add_css_class(vault_lbl, "pop-section-label");
    gtk_widget_set_halign(vault_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pop_box), vault_lbl);

    GtkWidget *vault_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *vault_path_lbl = gtk_label_new("Current Vault");
    gtk_widget_set_hexpand(vault_path_lbl, TRUE);
    gtk_widget_set_halign(vault_path_lbl, GTK_ALIGN_START);

    char cwd_buf[PATH_MAX];
    if (getcwd(cwd_buf, sizeof(cwd_buf)) == NULL) {
        strcpy(cwd_buf, "Unknown");
    }
    gui->vault_path_lbl_val = gtk_label_new(cwd_buf);
    gtk_widget_add_css_class(gui->vault_path_lbl_val, "stats-value");
    gtk_widget_set_halign(gui->vault_path_lbl_val, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(gui->vault_path_lbl_val), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(gui->vault_path_lbl_val), 20);

    GtkWidget *vault_open_btn = gtk_button_new_with_label("Open Vault");
    gtk_widget_add_css_class(vault_open_btn, "pop-btn");
    g_signal_connect(vault_open_btn, "clicked", G_CALLBACK(on_open_vault_clicked), gui);

    gtk_box_append(GTK_BOX(vault_row), vault_path_lbl);
    gtk_box_append(GTK_BOX(vault_row), gui->vault_path_lbl_val);
    gtk_box_append(GTK_BOX(vault_row), vault_open_btn);
    gtk_box_append(GTK_BOX(pop_box), vault_row);

    gtk_box_append(GTK_BOX(pop_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *th_lbl = gtk_label_new("THEME");
    gtk_widget_add_css_class(th_lbl, "pop-section-label");
    gtk_widget_set_halign(th_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pop_box), th_lbl);

    GtkWidget *theme_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *theme_label = gtk_label_new("Theme");
    gtk_widget_set_hexpand(theme_label, TRUE);
    gtk_widget_set_halign(theme_label, GTK_ALIGN_START);

    const char *themes[] = {
        "Deep Slate",
        "Classic Sepia",
        "Midnight",
        "Things",
        "Typewriter Light",
        "Typewriter Dark",
        "Qirtas Ink",
        "Add Custom Theme...",
        NULL
    };
    GtkWidget *theme_dropdown = gtk_drop_down_new_from_strings(themes);
    
    int theme_idx = 0;
    if (strcmp(current_theme, "sepia") == 0) theme_idx = 1;
    else if (strcmp(current_theme, "midnight") == 0) theme_idx = 2;
    else if (strcmp(current_theme, "things") == 0) theme_idx = 3;
    else if (strcmp(current_theme, "typewriter-light") == 0) theme_idx = 4;
    else if (strcmp(current_theme, "typewriter-dark") == 0) theme_idx = 5;
    else if (strcmp(current_theme, "qirtas") == 0) theme_idx = 6;
    else if (strcmp(current_theme, "custom") == 0) theme_idx = 7;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(theme_dropdown), theme_idx);
    
    g_signal_connect(theme_dropdown, "notify::selected", G_CALLBACK(on_theme_dropdown_changed), gui);
    
    gtk_box_append(GTK_BOX(theme_row), theme_label);
    gtk_box_append(GTK_BOX(theme_row), theme_dropdown);
    gtk_box_append(GTK_BOX(pop_box), theme_row);

    gtk_box_append(GTK_BOX(pop_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *pr_lbl = gtk_label_new("PREFERENCES");
    gtk_widget_add_css_class(pr_lbl, "pop-section-label");
    gtk_widget_set_halign(pr_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pop_box), pr_lbl);

    GtkWidget *wrap_chk = gtk_check_button_new_with_label("Word Wrap");
    gui->wrap_chk = wrap_chk;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(wrap_chk), FALSE);
    g_signal_connect(wrap_chk, "toggled", G_CALLBACK(on_wrap_toggled), source_view);
    gtk_widget_set_sensitive(wrap_chk, FALSE);
    gtk_widget_set_tooltip_text(
        wrap_chk,
        "Disabled while virtual scrolling uses fixed logical line heights.");
    gtk_box_append(GTK_BOX(pop_box), wrap_chk);

    GtkWidget *trail_chk = gtk_check_button_new_with_label("Pointer Trail Animation");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(trail_chk), gui->enable_cursor_trail);
    g_signal_connect(trail_chk, "toggled", G_CALLBACK(on_trail_toggled), gui);
    gtk_box_append(GTK_BOX(pop_box), trail_chk);

    GtkWidget *trail_color_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *trail_color_lbl = gtk_label_new("Trail Color");
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

    GtkWidget *trail_color_custom_chk = gtk_check_button_new_with_label("Custom");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(trail_color_custom_chk), gui->use_custom_trail_color);
    g_signal_connect(trail_color_custom_chk, "toggled", G_CALLBACK(on_trail_color_custom_toggled), gui);
    gtk_box_append(GTK_BOX(trail_color_row), trail_color_custom_chk);
    gui->trail_color_chk = trail_color_custom_chk;

    gtk_box_append(GTK_BOX(pop_box), trail_color_row);

    GtkWidget *pointer_color_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *pointer_color_lbl = gtk_label_new("Pointer Color");
    gtk_widget_set_hexpand(pointer_color_lbl, TRUE);
    gtk_widget_set_halign(pointer_color_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pointer_color_row), pointer_color_lbl);

    GtkWidget *pointer_color_btn = gtk_color_dialog_button_new(color_dialog);
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(pointer_color_btn), &gui->custom_pointer_color);
    gtk_widget_set_sensitive(pointer_color_btn, gui->use_custom_pointer_color);
    g_signal_connect(pointer_color_btn, "notify::rgba", G_CALLBACK(on_pointer_color_changed), gui);
    gtk_box_append(GTK_BOX(pointer_color_row), pointer_color_btn);
    gui->pointer_color_btn = pointer_color_btn;

    GtkWidget *pointer_color_custom_chk = gtk_check_button_new_with_label("Custom");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(pointer_color_custom_chk), gui->use_custom_pointer_color);
    g_signal_connect(pointer_color_custom_chk, "toggled", G_CALLBACK(on_pointer_color_custom_toggled), gui);
    gtk_box_append(GTK_BOX(pointer_color_row), pointer_color_custom_chk);
    gui->pointer_color_chk = pointer_color_custom_chk;

    gtk_box_append(GTK_BOX(pop_box), pointer_color_row);


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
        "Inter",
        "Lora",
        "Merriweather",
        "JetBrains Mono",
        "Roboto",
        "Fira Code",
        "Source Code Pro",
        "Add Custom Font...",
        NULL
    };
    GtkWidget *en_font_dropdown = gtk_drop_down_new_from_strings(en_fonts);
    int en_idx = 0;
    for (int i = 0; i < 7; i++) {
        if (strcmp(gui->current_en_font, en_fonts[i]) == 0) {
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
        if (strcmp(gui->current_ar_font, ar_fonts[i]) == 0) {
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

    GtkWidget *layout_lbl = gtk_label_new("LAYOUT PREFERENCES");
    gtk_widget_add_css_class(layout_lbl, "pop-section-label");
    gtk_widget_set_halign(layout_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pop_box), layout_lbl);

    const char *status_bar_positions[] = {
        "Bottom",
        "Top",
        NULL
    };
    GtkWidget *sb_pos_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *sb_pos_lbl = gtk_label_new("Status Bar Position");
    gtk_widget_set_hexpand(sb_pos_lbl, TRUE);
    gtk_widget_set_halign(sb_pos_lbl, GTK_ALIGN_START);
    gui->sb_pos_dropdown = gtk_drop_down_new_from_strings(status_bar_positions);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(gui->sb_pos_dropdown), 1); // Default Top
    g_signal_connect(gui->sb_pos_dropdown, "notify::selected", G_CALLBACK(on_status_bar_pos_changed), gui);
    gtk_box_append(GTK_BOX(sb_pos_row), sb_pos_lbl);
    gtk_box_append(GTK_BOX(sb_pos_row), gui->sb_pos_dropdown);
    gtk_box_append(GTK_BOX(pop_box), sb_pos_row);

    const char *sidebar_sides[] = {
        "Left",
        "Right",
        NULL
    };
    GtkWidget *sb_side_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *sb_side_lbl = gtk_label_new("Sidebar Side");
    gtk_widget_set_hexpand(sb_side_lbl, TRUE);
    gtk_widget_set_halign(sb_side_lbl, GTK_ALIGN_START);
    gui->sb_side_dropdown = gtk_drop_down_new_from_strings(sidebar_sides);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(gui->sb_side_dropdown), 0); // Default Left
    g_signal_connect(gui->sb_side_dropdown, "notify::selected", G_CALLBACK(on_sidebar_side_changed), gui);
    gtk_box_append(GTK_BOX(sb_side_row), sb_side_lbl);
    gtk_box_append(GTK_BOX(sb_side_row), gui->sb_side_dropdown);
    gtk_box_append(GTK_BOX(pop_box), sb_side_row);

    gui->divider_chk = gtk_check_button_new_with_label("Show layout dividers");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(gui->divider_chk), gui->show_layout_dividers);
    g_signal_connect(gui->divider_chk, "toggled", G_CALLBACK(on_layout_dividers_toggled), gui);
    gtk_box_append(GTK_BOX(pop_box), gui->divider_chk);

    GtkWidget *bottom_margin_chk = gtk_check_button_new_with_label("Scroll past end (extra bottom space)");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(bottom_margin_chk), gui->enable_bottom_margin);
    g_signal_connect(bottom_margin_chk, "toggled", G_CALLBACK(on_bottom_margin_toggled), gui);
    gtk_box_append(GTK_BOX(pop_box), bottom_margin_chk);

    gui->focus_chk = gtk_check_button_new_with_label("Focus Mode");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(gui->focus_chk), gui->enable_focus_mode);
    g_signal_connect(gui->focus_chk, "toggled", G_CALLBACK(on_focus_mode_toggled), gui);
    gtk_box_append(GTK_BOX(pop_box), gui->focus_chk);

    GtkWidget *border_chk = gtk_check_button_new_with_label("Show editor border");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(border_chk), gui->enable_editor_border);
    g_signal_connect(border_chk, "toggled", G_CALLBACK(on_editor_border_toggled), gui);
    gtk_box_append(GTK_BOX(pop_box), border_chk);



    /* ── KEYBOARD SHORTCUTS ── */
    GtkWidget *kb_lbl = gtk_label_new("KEYBOARD SHORTCUTS");
    gtk_widget_add_css_class(kb_lbl, "pop-section-label");
    gtk_widget_set_halign(kb_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pop_box), kb_lbl);

    GtkWidget *kb_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *kb_lbl_widget = gtk_label_new("Keyboard Shortcuts");
    gtk_widget_set_hexpand(kb_lbl_widget, TRUE);
    gtk_widget_set_halign(kb_lbl_widget, GTK_ALIGN_START);
    GtkWidget *kb_open_btn = gtk_button_new_with_label("Open Reference  (Ctrl+?)");
    gtk_widget_add_css_class(kb_open_btn, "pop-btn");
    g_signal_connect_swapped(kb_open_btn, "clicked", G_CALLBACK(show_keybindings_window), gui);
    gtk_box_append(GTK_BOX(kb_row), kb_lbl_widget);
    gtk_box_append(GTK_BOX(kb_row), kb_open_btn);
    gtk_box_append(GTK_BOX(pop_box), kb_row);

    // Sync & Cloud Layout
    gtk_box_append(GTK_BOX(pop_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *sync_lbl = gtk_label_new("SYNC & CLOUD");
    gtk_widget_add_css_class(sync_lbl, "pop-section-label");
    gtk_widget_set_halign(sync_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pop_box), sync_lbl);

    // --- GOOGLE DRIVE SYNC ---
    GtkWidget *gd_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(gd_card, "sync-card");
    gtk_box_append(GTK_BOX(pop_box), gd_card);

    GtkWidget *gd_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(gd_row, "sync-card-header");
    GtkWidget *gd_lbl = gtk_label_new("Google Drive:");
    gtk_widget_add_css_class(gd_lbl, "stats-label");
    gtk_widget_add_css_class(gd_lbl, "sync-card-title");
    gui->sync_status_lbl = gtk_label_new("Disconnected");
    gtk_widget_add_css_class(gui->sync_status_lbl, "stats-value");
    gtk_widget_add_css_class(gui->sync_status_lbl, "sync-card-status");
    gtk_widget_set_halign(gui->sync_status_lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(gui->sync_status_lbl, TRUE);
    gui->sync_connect_btn = gtk_button_new_with_label("Connect to Google Drive");
    gtk_widget_add_css_class(gui->sync_connect_btn, "pop-btn");
    gtk_widget_add_css_class(gui->sync_connect_btn, "sync-card-action");
    gtk_widget_set_hexpand(gui->sync_connect_btn, TRUE);
    gtk_widget_set_halign(gui->sync_connect_btn, GTK_ALIGN_FILL);
    g_signal_connect(gui->sync_connect_btn, "clicked", G_CALLBACK(on_sync_connect_clicked), gui);
    gtk_box_append(GTK_BOX(gd_row), gd_lbl);
    gtk_box_append(GTK_BOX(gd_row), gui->sync_status_lbl);
    gtk_box_append(GTK_BOX(gd_card), gd_row);
    gtk_box_append(GTK_BOX(gd_card), gui->sync_connect_btn);

    gui->sync_now_btn = gtk_button_new_with_label("Sync Now");
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
    GtkWidget *db_lbl_sec = gtk_label_new("Dropbox:");
    gtk_widget_add_css_class(db_lbl_sec, "stats-label");
    gtk_widget_add_css_class(db_lbl_sec, "sync-card-title");
    gui->dropbox_status_lbl = gtk_label_new("Disconnected");
    gtk_widget_add_css_class(gui->dropbox_status_lbl, "stats-value");
    gtk_widget_add_css_class(gui->dropbox_status_lbl, "sync-card-status");
    gtk_widget_set_halign(gui->dropbox_status_lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(gui->dropbox_status_lbl, TRUE);
    gui->dropbox_connect_btn = gtk_button_new_with_label("Connect to Dropbox");
    gtk_widget_add_css_class(gui->dropbox_connect_btn, "pop-btn");
    gtk_widget_add_css_class(gui->dropbox_connect_btn, "sync-card-action");
    gtk_widget_set_hexpand(gui->dropbox_connect_btn, TRUE);
    gtk_widget_set_halign(gui->dropbox_connect_btn, GTK_ALIGN_FILL);
    g_signal_connect(gui->dropbox_connect_btn, "clicked", G_CALLBACK(on_dropbox_connect_clicked), gui);
    gtk_box_append(GTK_BOX(db_row), db_lbl_sec);
    gtk_box_append(GTK_BOX(db_row), gui->dropbox_status_lbl);
    gtk_box_append(GTK_BOX(db_card), db_row);
    gtk_box_append(GTK_BOX(db_card), gui->dropbox_connect_btn);

    gui->dropbox_now_btn = gtk_button_new_with_label("Sync Now");
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
    GtkWidget *gh_lbl_sec = gtk_label_new("GitHub:");
    gtk_widget_add_css_class(gh_lbl_sec, "stats-label");
    gtk_widget_add_css_class(gh_lbl_sec, "sync-card-title");
    gui->github_status_lbl = gtk_label_new("Disconnected");
    gtk_widget_add_css_class(gui->github_status_lbl, "stats-value");
    gtk_widget_add_css_class(gui->github_status_lbl, "sync-card-status");
    gtk_widget_set_halign(gui->github_status_lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(gui->github_status_lbl, TRUE);
    gui->github_connect_btn = gtk_button_new_with_label("Connect to GitHub");
    gtk_widget_add_css_class(gui->github_connect_btn, "pop-btn");
    gtk_widget_add_css_class(gui->github_connect_btn, "sync-card-action");
    gtk_widget_set_hexpand(gui->github_connect_btn, TRUE);
    gtk_widget_set_halign(gui->github_connect_btn, GTK_ALIGN_FILL);
    g_signal_connect(gui->github_connect_btn, "clicked", G_CALLBACK(on_github_connect_clicked), gui);
    gtk_box_append(GTK_BOX(gh_row), gh_lbl_sec);
    gtk_box_append(GTK_BOX(gh_row), gui->github_status_lbl);
    gtk_box_append(GTK_BOX(gh_card), gh_row);
    gtk_box_append(GTK_BOX(gh_card), gui->github_connect_btn);

    gui->github_now_btn = gtk_button_new_with_label("Sync Now");
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
    GtkWidget *local_lbl_sec = gtk_label_new("Local / Syncthing:");
    gtk_widget_add_css_class(local_lbl_sec, "stats-label");
    gtk_widget_add_css_class(local_lbl_sec, "sync-card-title");
    gui->local_sync_status_lbl = gtk_label_new("~/QirtasSync");
    gtk_widget_add_css_class(gui->local_sync_status_lbl, "stats-value");
    gtk_widget_add_css_class(gui->local_sync_status_lbl, "sync-card-status");
    gtk_widget_set_halign(gui->local_sync_status_lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(gui->local_sync_status_lbl, TRUE);
    gui->local_sync_btn = gtk_button_new_with_label("Sync Folder");
    gtk_widget_add_css_class(gui->local_sync_btn, "pop-btn");
    gtk_widget_add_css_class(gui->local_sync_btn, "sync-card-action");
    gtk_widget_set_hexpand(gui->local_sync_btn, TRUE);
    gtk_widget_set_halign(gui->local_sync_btn, GTK_ALIGN_FILL);
    g_signal_connect(gui->local_sync_btn, "clicked", G_CALLBACK(on_local_sync_clicked), gui);
    gtk_box_append(GTK_BOX(local_row), local_lbl_sec);
    gtk_box_append(GTK_BOX(local_row), gui->local_sync_status_lbl);
    gtk_box_append(GTK_BOX(local_card), local_row);
    gtk_box_append(GTK_BOX(local_card), gui->local_sync_btn);

    GtkWidget *pop_scroll = gtk_scrolled_window_new();
    gtk_widget_add_css_class(pop_scroll, "settings-sheet-scroll");
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(pop_scroll), 420);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(pop_scroll), 520);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(pop_scroll), TRUE);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(pop_scroll), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pop_scroll), pop_box);

    GtkWidget *settings_win = gtk_window_new();
    gtk_widget_add_css_class(settings_win, "settings-sheet-window");
    gtk_window_set_title(GTK_WINDOW(settings_win), "Settings");
    gtk_window_set_default_size(GTK_WINDOW(settings_win), 500, 620);
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

    /* ── Bottom-right: Menu icon button for quick file actions ── */
    gui->btn_status_actions = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(gui->btn_status_actions), "document-new-symbolic");
    gtk_widget_add_css_class(gui->btn_status_actions, "bottom-icon-btn");
    gtk_widget_set_tooltip_text(gui->btn_status_actions, "Quick File Actions");

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
    GtkWidget *img_new = gtk_image_new_from_icon_name("document-new-symbolic");
    GtkWidget *lbl_new = gtk_label_new("Add New File");
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
    GtkWidget *img_open = gtk_image_new_from_icon_name("document-open-symbolic");
    GtkWidget *lbl_open = gtk_label_new("Open File");
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
    GtkWidget *img_save = gtk_image_new_from_icon_name("document-save-symbolic");
    GtkWidget *lbl_save = gtk_label_new("Save File");
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
    GtkWidget *img_pdf = gtk_image_new_from_icon_name("document-print-symbolic");
    GtkWidget *lbl_pdf = gtk_label_new("Export as PDF");
    gtk_box_append(GTK_BOX(hbox_pdf), img_pdf);
    gtk_box_append(GTK_BOX(hbox_pdf), lbl_pdf);
    gtk_button_set_child(GTK_BUTTON(btn_pop_pdf), hbox_pdf);
    g_signal_connect(btn_pop_pdf, "clicked", G_CALLBACK(on_status_bar_export_pdf_clicked), gui);
    gtk_box_append(GTK_BOX(actions_box), btn_pop_pdf);

    gtk_popover_set_child(GTK_POPOVER(actions_popover), actions_box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(gui->btn_status_actions), actions_popover);

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

    GtkWidget *tab_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(tab_bar, "tab-bar");
    gtk_widget_set_valign(tab_bar, GTK_ALIGN_CENTER);
    gui->tab_bar_box = tab_bar;

    gtk_box_append(GTK_BOX(bottom_bar), gui->btn_sidebar_toggle);
    gtk_box_append(GTK_BOX(bottom_bar), tab_bar);
    gtk_box_append(GTK_BOX(bottom_bar), bottom_spacer);
    gtk_box_append(GTK_BOX(bottom_bar), gui->btn_search);
    gtk_box_append(GTK_BOX(bottom_bar), gui->btn_status_actions);
    gtk_box_append(GTK_BOX(bottom_bar), gui->lbl_words);
    gtk_box_append(GTK_BOX(bottom_bar), gui->lbl_chars);
    gtk_box_append(GTK_BOX(bottom_bar), gui->btn_sync_icon_bottom);

    gui->bottom_bar_widget = bottom_bar;
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

    load_sync_credentials(gui);
    int connected = zig_sync_check_status();
    gui_update_sync_status(connected, connected ? "Connected" : "Disconnected");

    int db_connected = zig_dropbox_check_status();
    gui_update_dropbox_status(db_connected, db_connected ? "Connected" : "Disconnected");

    int gh_connected = zig_github_check_status();
    gui_update_github_status(gh_connected, gh_connected ? "Connected" : "Disconnected");

    g_timeout_add(1000, simulate_crash_cb, gui);
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

void gui_set_text(const char *text, int len) {
    if (!global_source_view || !global_gui) return;
    if (!text || len <= 0) return;
    
    // Also verify we're on the main thread
    g_assert(g_main_context_is_owner(g_main_context_default()));
    
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));
    
    g_signal_handlers_block_by_func(buf, on_insert_text_before, global_gui);
    g_signal_handlers_block_by_func(buf, on_insert_text_after, global_gui);
    g_signal_handlers_block_by_func(buf, on_delete_range_after, global_gui);
    g_signal_handlers_block_by_func(buf, on_buffer_changed, global_gui);
    g_signal_handlers_block_by_func(buf, on_buffer_modified_changed, global_gui);
    
    gtk_text_buffer_set_text(buf, text, len);
    gtk_text_buffer_set_modified(buf, FALSE);
    reset_cursor_trail(global_gui);
    
    parse_and_render_hrs(buf, global_gui);
    
    g_signal_handlers_unblock_by_func(buf, on_insert_text_before, global_gui);
    g_signal_handlers_unblock_by_func(buf, on_insert_text_after, global_gui);
    g_signal_handlers_unblock_by_func(buf, on_delete_range_after, global_gui);
    g_signal_handlers_unblock_by_func(buf, on_buffer_changed, global_gui);
    g_signal_handlers_unblock_by_func(buf, on_buffer_modified_changed, global_gui);
    
    update_all_paragraphs_direction(buf);
    apply_wiki_link_tags(buf);
    gui_reset_preferred_x(global_gui);
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
    *line = (global_gui ? global_gui->viewport_start_line : 0) + rel_line + 1;
    *col = gtk_text_iter_get_line_offset(&iter);
}

void gui_set_cursor_position(int line, int col) {
    if (!global_source_view || !global_gui) return;
    g_print("GUI_SET_CURSOR_POSITION_ENTRY received_line=%d received_col=%d\n", line, col);
    
    int target_abs_line = line - 1; // 0-indexed
    if (target_abs_line < 0) target_abs_line = 0;
    
    // Check if within active page
  
    if (target_abs_line < global_gui->viewport_start_line ||
        target_abs_line >= global_gui->viewport_end_line) {
        request_viewport_position(global_gui, target_abs_line);
    }
    
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));
    GtkTextIter iter;
    int rel_line = target_abs_line - global_gui->viewport_start_line;
    if (rel_line < 0) rel_line = 0;
    
    int total_lines = gtk_text_buffer_get_line_count(buf);
    if (rel_line >= total_lines) {
        rel_line = total_lines - 1;
    }
    if (rel_line < 0) rel_line = 0;

    gtk_text_buffer_get_iter_at_line(buf, &iter, rel_line);
    int line_bytes = gtk_text_iter_get_bytes_in_line(&iter);
    
    int safe_col = col;
    if (safe_col < 0) safe_col = 0;
    if (safe_col > line_bytes) safe_col = line_bytes;

    debug_get_iter_at(buf, &iter, rel_line, safe_col, "gui_set_cursor_position");
    gtk_text_buffer_select_range(buf, &iter, &iter);
    reset_cursor_trail(global_gui);
    gui_reset_preferred_x(global_gui);
}

static void cleanup_measurement_state(AppGui *gui)
{
    (void)gui;
}

static void get_spacer_heights(
    AppGui *gui,
    int start_line,
    int end_line,
    int *out_top_h,
    int *out_bottom_h)
{
    int h = gui->line_height > 0 ? gui->line_height : 24;

    *out_top_h = start_line * h;
    if (*out_top_h < 1) *out_top_h = 1;

    *out_bottom_h =
        (gui->total_virtual_lines - end_line) * h;

    if (*out_bottom_h < 1)
        *out_bottom_h = 1;
}

static int get_line_at_y(AppGui *gui, double y)
{
    int h = gui->line_height > 0 ? gui->line_height : 24;
    int line = (int)(y / h);
    g_print("GET_LINE_AT_Y input_y=%g returned_line=%d line_height=%d viewport_start_line=%d\n",
            y, line, h, gui ? gui->viewport_start_line : -1);
    return line;
}

static double get_y_for_line(AppGui *gui, int line)
{
    int h = gui->line_height > 0 ? gui->line_height : 24;
    return (double)(line * h);
}

void gui_set_virtual_scroll_mode(int enabled, int total_lines) {
    if (!global_gui) return;
    
    cleanup_measurement_state(global_gui);
    global_gui->virtual_scroll_enabled = (gboolean)enabled;
    
    if (enabled) {
        gtk_widget_set_visible(global_gui->top_spacer, TRUE);
        gtk_widget_set_visible(global_gui->bottom_spacer, TRUE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(global_gui->source_view), GTK_WRAP_NONE);
        
        if (global_gui->wrap_chk) {
            g_signal_handlers_block_by_func(global_gui->wrap_chk, G_CALLBACK(on_wrap_toggled), global_gui->source_view);
            gtk_check_button_set_active(GTK_CHECK_BUTTON(global_gui->wrap_chk), FALSE);
            g_signal_handlers_unblock_by_func(global_gui->wrap_chk, G_CALLBACK(on_wrap_toggled), global_gui->source_view);
            gtk_widget_set_sensitive(global_gui->wrap_chk, FALSE);
            gtk_widget_set_tooltip_text(global_gui->wrap_chk, "Disabled while virtual scrolling uses fixed logical line heights.");
        }
    } else {
        gtk_widget_set_visible(global_gui->top_spacer, FALSE);
        gtk_widget_set_visible(global_gui->bottom_spacer, FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(global_gui->source_view), GTK_WRAP_WORD_CHAR);
        
        if (global_gui->wrap_chk) {
            g_signal_handlers_block_by_func(global_gui->wrap_chk, G_CALLBACK(on_wrap_toggled), global_gui->source_view);
            gtk_check_button_set_active(GTK_CHECK_BUTTON(global_gui->wrap_chk), TRUE);
            g_signal_handlers_unblock_by_func(global_gui->wrap_chk, G_CALLBACK(on_wrap_toggled), global_gui->source_view);
            gtk_widget_set_sensitive(global_gui->wrap_chk, TRUE);
            gtk_widget_set_tooltip_text(global_gui->wrap_chk, "Toggle word wrapping.");
        }
        
        global_gui->total_virtual_lines = total_lines;
        viewport_set_range(global_gui, 0, total_lines);
    }
}

static void viewport_set_range(AppGui *gui, int start_line, int end_line)
{
    if (!gui) return;

    gui->viewport_start_line = start_line;
    gui->viewport_end_line = end_line;

    int top_h = 0, bottom_h = 0;
    get_spacer_heights(gui, start_line, end_line, &top_h, &bottom_h);
    set_spacers_with_compensation(gui, top_h, bottom_h);
}

void gui_init_virtual_document(int total_lines, int start_line, int end_line) {
    if (!global_gui) return;
    
    cleanup_measurement_state(global_gui);
    
    global_gui->total_virtual_lines = total_lines;
    viewport_set_range(global_gui, start_line, end_line);
    
    global_gui->line_height =
    get_line_height(global_gui->source_view);

    if (global_gui->line_height <= 0)
    global_gui->line_height = 24;
   
    int top_h = 0, bottom_h = 0;
    get_spacer_heights(global_gui, start_line, end_line, &top_h, &bottom_h);
    
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
    if (global_gui->active_tab_index == -1) {
        gui_set_sync_status("Not Synced");
        return;
    }
    if (strcmp(global_gui->open_tabs[global_gui->active_tab_index], "Untitled") == 0) {
        return;
    }
    
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_source_view));
    gui_set_sync_status("Saving...");
    
    extern int zig_save_document(void);
    int status = zig_save_document();
    
    if (status == 0) {
        gui_set_sync_status("Saved");
        gtk_text_buffer_set_modified(buf, FALSE);
        if (global_gui->active_tab_index != -1 && global_gui->active_tab_index < global_gui->num_tabs) {
            global_gui->tab_modified[global_gui->active_tab_index] = FALSE;
            gui_tabs_refresh(global_gui);
        }
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

static void reorder_main_layout(AppGui *gui) {
    if (!gui || !gui->main_vertical_box || !gui->bottom_bar_widget || !gui->sidebar_editor_box) return;

    gboolean status_bar_is_top = TRUE; // Default to Top
    if (gui->enable_focus_mode) {
        status_bar_is_top = TRUE;
    } else if (gui->sb_pos_dropdown) {
        guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(gui->sb_pos_dropdown));
        if (selected == 0) {
            status_bar_is_top = FALSE;
        }
    }

    if (status_bar_is_top) {
        // Status Bar (Top) -> Editor
        gtk_box_reorder_child_after(GTK_BOX(gui->main_vertical_box), gui->bottom_bar_widget, NULL);
        gtk_box_reorder_child_after(GTK_BOX(gui->main_vertical_box), gui->sidebar_editor_box, gui->bottom_bar_widget);
    } else {
        // Editor -> Status Bar (Bottom)
        gtk_box_reorder_child_after(GTK_BOX(gui->main_vertical_box), gui->sidebar_editor_box, NULL);
        gtk_box_reorder_child_after(GTK_BOX(gui->main_vertical_box), gui->bottom_bar_widget, gui->sidebar_editor_box);
    }
}

void gui_tabs_save_active_to_cache(void) {
    AppGui *gui = global_gui;
    if (!gui) return;
    int idx = gui->active_tab_index;
    if (idx != -1 && idx < gui->num_tabs) {
        if (strcmp(gui->open_tabs[idx], "Untitled") == 0) {
            extern const char *zig_get_document_text(void);
            extern void zig_free_document_text(const char *ptr);
            const char *text = zig_get_document_text();
            g_free(gui->tab_contents[idx]);
            gui->tab_contents[idx] = text ? g_strdup(text) : NULL;
            zig_free_document_text(text);
        } else {
            extern int zig_save_document(void);
            zig_save_document();
        }
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
        gui->tab_modified[idx] = gtk_text_buffer_get_modified(buf);
    }
}

void gui_tabs_restore_active_from_cache(void) {
    AppGui *gui = global_gui;
    if (!gui) return;
    int idx = gui->active_tab_index;
    if (idx != -1 && idx < gui->num_tabs) {
        if (strcmp(gui->open_tabs[idx], "Untitled") == 0 && gui->tab_contents[idx] != NULL) {
            gui_set_text(gui->tab_contents[idx], -1);
            GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
            gtk_text_buffer_set_modified(buf, gui->tab_modified[idx]);
        }
    }
}

static void on_tab_button_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    const char *path = (const char *)user_data;
    extern void zig_open_file(const char *filename);
    zig_open_file(path);
}

static void on_tab_close_clicked(GtkButton *btn, gpointer user_data);
static void on_tab_close_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    const char *path = (const char *)user_data;
    AppGui *gui = global_gui;
    if (gui) {
        for (int i = 0; i < gui->num_tabs; i++) {
            if (strcmp(gui->open_tabs[i], path) == 0) {
                gui_tabs_close(gui, i);
                break;
            }
        }
    }
}

void gui_refresh_explorer(void) {
    gui_run_on_main_thread(refresh_explorer_idle_cb, NULL);
}

void gui_set_title(const char *title) {
    gui_reset_scroll_direction_state();
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
        }
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

void gui_get_active_page_bounds(int *start_line, int *end_line, int *total_lines) {
    if (!global_gui) {
        *start_line = 0;
        *end_line = 0;
        *total_lines = 0;
        return;
    }













    *start_line = global_gui->viewport_start_line;
    *end_line = global_gui->viewport_end_line;
    *total_lines = global_gui->total_virtual_lines;
}

void gui_prepare_tab_switch(void) {
    if (!global_gui) return;

    if (global_gui->source_view) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(global_gui->source_view));
        gtk_text_buffer_set_text(buf, "Loading…", -1);
    }

    while (g_main_context_pending(NULL)) {
        g_main_context_iteration(NULL, FALSE);
    }

    if (global_gui->vadjustment) {
        g_signal_handler_block(global_gui->vadjustment, global_gui->scroll_signal_id);
        gtk_adjustment_set_value(global_gui->vadjustment, 0.0);
        global_gui->viewport_start_line = 0;
        global_gui->viewport_end_line = 0;
        global_gui->last_scroll_requested_line = -1;
        global_gui->pending_line = -1;
        g_signal_handler_unblock(global_gui->vadjustment, global_gui->scroll_signal_id);
    }
    gui_reset_preferred_x(global_gui);
}

void gui_update_total_virtual_lines(int total_lines) {
    g_print("UPDATE_TOTAL_LINES: %d\n", total_lines);
    if (!global_gui) return;
    if (total_lines <= 0) return;
    if (global_gui->loading_viewport) return;
    global_gui->total_virtual_lines = total_lines;
    
    int line_h = global_gui->line_height;
    if (line_h <= 0) line_h = 24;
    int bottom_h = (total_lines - global_gui->viewport_end_line) * line_h;
    if (bottom_h < 0) bottom_h = 0;
    gtk_widget_set_size_request(global_gui->bottom_spacer, -1, bottom_h);
    gtk_widget_queue_resize(global_gui->virtual_layout_box);
}

void gui_reset_scroll_direction_state(void) {
    last_loaded_start = -1;
    last_load_direction = 0;
    last_load_time_ms = 0;
}

void gui_remeasure_line_height(void) {
    if (!global_gui || !global_gui->source_view) return;
    global_gui->line_height = get_line_height(global_gui->source_view);
}

gboolean debug_get_iter_at(
    GtkTextBuffer *buf,
    GtkTextIter *iter,
    int rel_line,
    int col,
    const char *caller
) {
    int line_bytes = -1;
    int line_chars = -1;
    int generation = -1;
    int view_start = -1;
    int view_end = -1;

    if (global_gui) {
        generation = (int)global_gui->buffer_generation;
        view_start = global_gui->viewport_start_line;
        view_end = global_gui->viewport_end_line;
    }

    int total_lines = gtk_text_buffer_get_line_count(buf);
    if (rel_line >= 0 && rel_line < total_lines) {
        GtkTextIter line_start_iter;
        gtk_text_buffer_get_iter_at_line(buf, &line_start_iter, rel_line);
        line_bytes = gtk_text_iter_get_bytes_in_line(&line_start_iter);
        line_chars = gtk_text_iter_get_chars_in_line(&line_start_iter);
    }

    g_print("ITER_DEBUG caller=%s\nline=%d\ncol=%d\nline_bytes=%d\nline_chars=%d\ngeneration=%d\nviewport=%d-%d\n",
            caller, rel_line, col, line_bytes, line_chars, generation, view_start, view_end);

    gtk_text_buffer_get_iter_at_line_offset(buf, iter, rel_line, col);
    
    g_print("ITER_DEBUG SUCCESS caller=%s\n", caller);
    return TRUE;
}

void debug_get_iter_at_offset(
    GtkTextBuffer *buf,
    GtkTextIter *iter,
    int char_offset,
    const char *caller
) {
    int generation = global_gui ? (int)global_gui->buffer_generation : -1;
    g_print("ITER_DEBUG_OFFSET before caller=%s offset=%d generation=%d\n", caller, char_offset, generation);
    gtk_text_buffer_get_iter_at_offset(buf, iter, char_offset);
    g_print("ITER_DEBUG_OFFSET SUCCESS caller=%s\n", caller);
}

void debug_set_line_offset(
    GtkTextIter *iter,
    int offset,
    const char *caller
) {
    g_print("ITER_DEBUG_SET_LINE_OFFSET before caller=%s offset=%d\n", caller, offset);
    gtk_text_iter_set_line_offset(iter, offset);
    g_print("ITER_DEBUG_SET_LINE_OFFSET SUCCESS caller=%s\n", caller);
}

#undef gtk_text_buffer_place_cursor
void debug_place_cursor(GtkTextBuffer *buf, const GtkTextIter *iter, const char *caller) {
    int line = gtk_text_iter_get_line(iter);
    int col = gtk_text_iter_get_line_offset(iter);
    int gen = global_gui ? (int)global_gui->buffer_generation : -1;
    g_print("CALL_PLACE_CURSOR caller=%s line=%d col=%d generation=%d\n", caller, line, col, gen);
    gtk_text_buffer_place_cursor(buf, iter);
}

#undef gtk_text_buffer_select_range
void debug_select_range(GtkTextBuffer *buf, const GtkTextIter *ins, const GtkTextIter *bound, const char *caller) {
    int ins_line = gtk_text_iter_get_line(ins);
    int ins_col = gtk_text_iter_get_line_offset(ins);
    int bound_line = gtk_text_iter_get_line(bound);
    int bound_col = gtk_text_iter_get_line_offset(bound);
    int gen = global_gui ? (int)global_gui->buffer_generation : -1;
    g_print("CALL_SELECT_RANGE caller=%s ins_line=%d ins_col=%d bound_line=%d bound_col=%d generation=%d\n", 
            caller, ins_line, ins_col, bound_line, bound_col, gen);
    gtk_text_buffer_select_range(buf, ins, bound);
}

#undef gtk_text_view_scroll_to_mark
void debug_scroll_to_mark(GtkTextView *text_view, GtkTextMark *mark, double within_margin, gboolean use_align, double xalign, double yalign, const char *caller) {
    const char *mark_name = gtk_text_mark_get_name(mark);
    int gen = global_gui ? (int)global_gui->buffer_generation : -1;
    g_print("CALL_SCROLL_TO_MARK caller=%s mark=%s generation=%d\n", caller, mark_name ? mark_name : "anonymous", gen);
    gtk_text_view_scroll_to_mark(text_view, mark, within_margin, use_align, xalign, yalign);
}

#undef gtk_text_view_scroll_mark_onscreen
void debug_scroll_mark_onscreen(GtkTextView *text_view, GtkTextMark *mark, const char *caller) {
    const char *mark_name = gtk_text_mark_get_name(mark);
    int gen = global_gui ? (int)global_gui->buffer_generation : -1;
    g_print("CALL_SCROLL_MARK_ONSCREEN caller=%s mark=%s generation=%d\n", caller, mark_name ? mark_name : "anonymous", gen);
    gtk_text_view_scroll_mark_onscreen(text_view, mark);
}
