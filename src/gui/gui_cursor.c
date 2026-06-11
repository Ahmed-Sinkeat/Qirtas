#include "gui_internal.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <stdio.h>

/* 0.55: snappier catch-up — 0.30 made plain horizontal arrow movement
 * feel like the caret was dragging behind. */
#define SMEAR_STIFFNESS 0.55
#define SMEAR_TRAILING_LENGTH 0.30
#define SMEAR_TRAILING_WIDTH 0.1
#define GHOST_DECAY (1.0 / (SMEAR_TRAILING_LENGTH * 60.0))

static GdkRGBA trail_color_for_theme(const char *theme_name) {
    if (theme_name) {
        if (strcmp(theme_name, "sepia") == 0)
            return (GdkRGBA){ 164.0/255.0,  46.0/255.0, 121.0/255.0, 1.0 };
        if (strcmp(theme_name, "things") == 0)
            return (GdkRGBA){  46.0/255.0, 128.0/255.0, 242.0/255.0, 1.0 };
        if (strcmp(theme_name, "typewriter-light") == 0)
            return (GdkRGBA){ 184.0/255.0,  46.0/255.0,  46.0/255.0, 1.0 };
        if (strcmp(theme_name, "typewriter-dark") == 0)
            return (GdkRGBA){ 255.0/255.0, 107.0/255.0, 107.0/255.0, 1.0 };
        if (strcmp(theme_name, "qirtas") == 0)
            return (GdkRGBA){  27.0/255.0,  24.0/255.0,  22.0/255.0, 1.0 };
        if (strcmp(theme_name, "qirtas-dark") == 0)
            return (GdkRGBA){ 236.0/255.0, 231.0/255.0, 219.0/255.0, 1.0 };
        if (strcmp(theme_name, "midnight") == 0)
            return (GdkRGBA){ 170.0/255.0, 196.0/255.0, 255.0/255.0, 1.0 };
    }
    return (GdkRGBA){ 255.0/255.0, 121.0/255.0, 198.0/255.0, 1.0 };
}

/* ── Generic key/value preference store ──
 * Lives in vault.db alongside session_state but in its own table so we
 * never fight the zig-side schema. All accessors silent-fail. */
static sqlite3 *prefs_open(void) {
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 5000);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS app_prefs (key TEXT PRIMARY KEY, value TEXT);",
        NULL, NULL, NULL);
    return db;
}

char *qirtas_pref_get_string(const char *key) {
    char *result = NULL;
    sqlite3 *db = prefs_open();
    if (!db) return NULL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT value FROM app_prefs WHERE key = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *v = sqlite3_column_text(stmt, 0);
            if (v) result = g_strdup((const char *)v);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return result;
}

void qirtas_pref_set_string(const char *key, const char *value) {
    sqlite3 *db = prefs_open();
    if (!db) return;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO app_prefs (key, value) VALUES (?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value;",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, value ? value : "", -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
}

int qirtas_pref_get_int(const char *key, int fallback) {
    char *s = qirtas_pref_get_string(key);
    if (!s) return fallback;
    int v = atoi(s);
    g_free(s);
    return v;
}

void qirtas_pref_set_int(const char *key, int value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    qirtas_pref_set_string(key, buf);
}

void reset_cursor_trail(AppGui *gui) {
    if (!gui) return;
    gui->cursor_initialized = FALSE;
    gui->trail_len = 0;
    gui->trail_needs_clear = TRUE;
    if (gui->cursor_trail_area) {
        gtk_widget_queue_draw(gui->cursor_trail_area);
    }
}

void load_trail_color_settings(AppGui *gui);
void save_trail_color_settings(AppGui *gui);
void load_pointer_color_settings(AppGui *gui);
void save_pointer_color_settings(AppGui *gui);
gboolean on_cursor_tick(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data);
void draw_cursor_trail(GtkDrawingArea *drawing_area, cairo_t *cr, int width, int height, gpointer user_data);

void load_trail_color_settings(AppGui *gui) {
    if (!gui) return;
    gui->use_custom_trail_color = FALSE;
    gui->custom_trail_color = trail_color_for_theme(gui->current_theme);
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }
    const char *sql = "SELECT enable_custom_trail_color, trail_color FROM session_state WHERE id = 1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
                gui->use_custom_trail_color = sqlite3_column_int(stmt, 0) != 0;
            }
            const unsigned char *trail_color = sqlite3_column_text(stmt, 1);
            if (trail_color && trail_color[0] != '\0') {
                GdkRGBA parsed;
                if (gdk_rgba_parse(&parsed, (const char *)trail_color)) {
                    gui->custom_trail_color = parsed;
                }
            }
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
}

void save_trail_color_settings(AppGui *gui) {
    if (!gui) return;
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }
    sqlite3_busy_timeout(db, 5000);
    char *rgba_str = gdk_rgba_to_string(&gui->custom_trail_color);
    const char *sql = "UPDATE session_state SET enable_custom_trail_color = ?, trail_color = ? WHERE id = 1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, gui->use_custom_trail_color ? 1 : 0);
        sqlite3_bind_text(stmt, 2, rgba_str ? rgba_str : "", -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    g_free(rgba_str);
    sqlite3_close(db);
}

void load_pointer_color_settings(AppGui *gui) {
    if (!gui) return;
    gui->use_custom_pointer_color = FALSE;
    gdk_rgba_parse(&gui->custom_pointer_color, "#1f6feb");
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }
    const char *sql = "SELECT enable_custom_pointer_color, pointer_color FROM session_state WHERE id = 1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
                gui->use_custom_pointer_color = sqlite3_column_int(stmt, 0) != 0;
            }
            const unsigned char *p_color = sqlite3_column_text(stmt, 1);
            if (p_color && p_color[0] != '\0') {
                GdkRGBA parsed;
                if (gdk_rgba_parse(&parsed, (const char *)p_color)) {
                    gui->custom_pointer_color = parsed;
                }
            }
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
}

void save_pointer_color_settings(AppGui *gui) {
    if (!gui) return;
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }
    sqlite3_busy_timeout(db, 5000);
    char *rgba_str = gdk_rgba_to_string(&gui->custom_pointer_color);
    const char *sql = "UPDATE session_state SET enable_custom_pointer_color = ?, pointer_color = ? WHERE id = 1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, gui->use_custom_pointer_color ? 1 : 0);
        sqlite3_bind_text(stmt, 2, rgba_str ? rgba_str : "", -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    g_free(rgba_str);
    sqlite3_close(db);
}

gboolean on_cursor_tick(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data) {
    (void)widget;
    (void)frame_clock;
    AppGui *gui = (AppGui *)user_data;
    if (!gui->enable_cursor_trail) {
        gui->trail_len = 0;
        return G_SOURCE_CONTINUE;
    }
    if (!gui->source_view || !gtk_widget_get_mapped(gui->source_view))
        return G_SOURCE_CONTINUE;

    GtkTextBuffer *buf         = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
    GtkTextMark   *insert_mark = gtk_text_buffer_get_insert(buf);
    GtkTextIter    iter;
    gtk_text_buffer_get_iter_at_mark(buf, &iter, insert_mark);

    GdkRectangle strong;
    gtk_text_view_get_cursor_locations(GTK_TEXT_VIEW(gui->source_view), &iter, &strong, NULL);

    double new_x       = (double)strong.x;
    double new_y       = (double)strong.y;
    gui->cursor_height = (double)strong.height;
    gui->cursor_width  = strong.width > 0 ? (double)strong.width : 2.0;

    if (!gui->cursor_initialized) {
        gui->cursor_current_x   = new_x;
        gui->cursor_current_y   = new_y;
        gui->cursor_target_x    = new_x;
        gui->cursor_target_y    = new_y;
        gui->cursor_initialized = TRUE;
        gui->trail_len          = 0;
        gui->trail_needs_clear  = FALSE;
        return G_SOURCE_CONTINUE;
    }

    gui->cursor_target_x = new_x;
    gui->cursor_target_y = new_y;

    double dx = gui->cursor_target_x - gui->cursor_current_x;
    double dy = gui->cursor_target_y - gui->cursor_current_y;
    double dist = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);

    double jump_threshold = gui->cursor_height * 8.0;
    if (dist > jump_threshold) {
        gui->cursor_current_x = gui->cursor_target_x;
        gui->cursor_current_y = gui->cursor_target_y;
        gui->trail_len = 0;
    } else {
        if (dist > 0.05) {
            gui->cursor_current_x += dx * SMEAR_STIFFNESS;
            gui->cursor_current_y += dy * SMEAR_STIFFNESS;
        } else {
            gui->cursor_current_x = gui->cursor_target_x;
            gui->cursor_current_y = gui->cursor_target_y;
        }
    }

    for (int i = 0; i < gui->trail_len; i++) {
        gui->trail[i].alpha -= GHOST_DECAY;
    }

    int alive = 0;
    for (int i = 0; i < gui->trail_len; i++) {
        if (gui->trail[i].alpha > 0.01) {
            gui->trail[alive++] = gui->trail[i];
        }
    }
    gui->trail_len = alive;

    double last_x = (gui->trail_len > 0) ? gui->trail[gui->trail_len - 1].x : gui->cursor_current_x;
    double last_y = (gui->trail_len > 0) ? gui->trail[gui->trail_len - 1].y : gui->cursor_current_y;
    double move_dx = gui->cursor_current_x - last_x;
    double move_dy = gui->cursor_current_y - last_y;
    double move_dist = (move_dx < 0 ? -move_dx : move_dx) + (move_dy < 0 ? -move_dy : move_dy);

    if (move_dist > 1.0) {
        if (gui->trail_len < GHOST_COUNT) {
            gui->trail[gui->trail_len].x     = last_x;
            gui->trail[gui->trail_len].y     = last_y;
            gui->trail[gui->trail_len].alpha = 0.5;
            gui->trail_len++;
        } else {
            for (int i = 0; i < GHOST_COUNT - 1; i++) {
                gui->trail[i] = gui->trail[i + 1];
            }
            gui->trail[GHOST_COUNT - 1].x     = last_x;
            gui->trail[GHOST_COUNT - 1].y     = last_y;
            gui->trail[GHOST_COUNT - 1].alpha = 0.5;
        }
    }

    gboolean needs_draw = (gui->trail_len > 0) ||
                           (gui->cursor_current_x != gui->cursor_target_x) ||
                           (gui->cursor_current_y != gui->cursor_target_y);
    if (needs_draw) {
        gui->trail_needs_clear = TRUE;
    }

    if (needs_draw || gui->trail_needs_clear) {
        if (gui->cursor_trail_area) {
            gtk_widget_queue_draw(gui->cursor_trail_area);
        }
        if (!needs_draw) {
            gui->trail_needs_clear = FALSE;
        }
    }

    return G_SOURCE_CONTINUE;
}

static void draw_ghost_caret(cairo_t *cr,
                             double gx, double gy,
                             double caret_w, double caret_h,
                             double r, double g_col, double b,
                             double alpha)
{
    if (alpha <= 0.01) return;

    double w      = caret_w < 0.5 ? 0.5 : caret_w;
    double h      = caret_h;
    double radius = w / 2.0;
    if (radius < 0.25) radius = 0.25;
    if (radius > h / 2.0) radius = h / 2.0;

    cairo_new_sub_path(cr);
    cairo_arc(cr, gx + w - radius, gy + radius, radius, -G_PI_2, 0);
    cairo_arc(cr, gx + w - radius, gy + h - radius, radius, 0, G_PI_2);
    cairo_arc(cr, gx + radius, gy + h - radius, radius, G_PI_2, G_PI);
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

void draw_cursor_trail(GtkDrawingArea *drawing_area, cairo_t *cr, int width, int height, gpointer user_data) {
    (void)drawing_area;
    (void)width;
    (void)height;
    AppGui *gui = (AppGui *)user_data;
    if (!gui->source_view || !gui->cursor_initialized) return;
    if (gui->trail_len == 0 && gui->cursor_current_x == gui->cursor_target_x && gui->cursor_current_y == gui->cursor_target_y) return;

    GdkRGBA cursor_color = trail_color_for_theme(gui->current_theme);
    double r = cursor_color.red;
    double g = cursor_color.green;
    double b = cursor_color.blue;

    double ox = 0.0, oy = 0.0;
    graphene_point_t p_in;
    p_in.x = 0.0f;
    p_in.y = 0.0f;
    graphene_point_t p_out;
    if (gtk_widget_compute_point(gui->source_view, gui->cursor_trail_area, &p_in, &p_out)) {
        ox = (double)p_out.x;
        oy = (double)p_out.y;
    }

    double caret_w = gui->cursor_width  < 2.5 ? 2.5 : gui->cursor_width;
    double caret_h = gui->cursor_height;

    int count = 0;
    struct {
        double x;
        double y;
        double alpha;
    } points[GHOST_COUNT + 2];

    for (int i = 0; i < gui->trail_len; i++) {
        int wx, wy;
        gtk_text_view_buffer_to_window_coords(
            GTK_TEXT_VIEW(gui->source_view), GTK_TEXT_WINDOW_WIDGET,
            (int)gui->trail[i].x, (int)gui->trail[i].y,
            &wx, &wy);
        points[count].x = (double)wx + ox;
        points[count].y = (double)wy + oy;
        points[count].alpha = gui->trail[i].alpha;
        count++;
    }

    {
        int wx, wy;
        gtk_text_view_buffer_to_window_coords(
            GTK_TEXT_VIEW(gui->source_view), GTK_TEXT_WINDOW_WIDGET,
            (int)gui->cursor_current_x, (int)gui->cursor_current_y,
            &wx, &wy);
        points[count].x = (double)wx + ox;
        points[count].y = (double)wy + oy;
        points[count].alpha = 0.5;
        count++;
    }

    {
        int wx, wy;
        gtk_text_view_buffer_to_window_coords(
            GTK_TEXT_VIEW(gui->source_view), GTK_TEXT_WINDOW_WIDGET,
            (int)gui->cursor_target_x, (int)gui->cursor_target_y,
            &wx, &wy);
        points[count].x = (double)wx + ox;
        points[count].y = (double)wy + oy;
        points[count].alpha = 0.5;
        count++;
    }

    for (int i = 0; i < count - 1; i++) {
        double xa = points[i].x;
        double ya = points[i].y;
        double alpha_a = points[i].alpha;
        double xb = points[i + 1].x;
        double yb = points[i + 1].y;
        double alpha_b = points[i + 1].alpha;
        double dx = xb - xa;
        double dy = yb - ya;
        double abs_dx = dx < 0 ? -dx : dx;
        double abs_dy = dy < 0 ? -dy : dy;

        double jump_threshold = caret_h * 8.0;
        if (abs_dx + abs_dy > jump_threshold) {
            double w_a = caret_w * (SMEAR_TRAILING_WIDTH + (1.0 - SMEAR_TRAILING_WIDTH) * alpha_a);
            draw_ghost_caret(cr, xa, ya, w_a, caret_h, r, g, b, alpha_a);
        } else {
            double w_a = caret_w * (SMEAR_TRAILING_WIDTH + (1.0 - SMEAR_TRAILING_WIDTH) * alpha_a);
            double w_b = caret_w * (SMEAR_TRAILING_WIDTH + (1.0 - SMEAR_TRAILING_WIDTH) * alpha_b);
            draw_smear_segment(cr, xa, ya, w_a, xb, yb, w_b, caret_h, r, g, b, alpha_a, alpha_b);
        }
    }
}

void init_cursor_trail(AppGui *gui) {
    load_trail_color_settings(gui);
    load_pointer_color_settings(gui);

    gtk_widget_add_tick_callback(gui->source_view, on_cursor_tick, gui, NULL);

    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(gui->cursor_trail_area),
                                   draw_cursor_trail,
                                   gui,
                                   NULL);
}
