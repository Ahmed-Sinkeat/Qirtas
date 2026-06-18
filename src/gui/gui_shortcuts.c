#include <gtk/gtk.h>
#include <sqlite3.h>
#include <string.h>
#include "gui_internal.h"

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
    { "open_file", "Open existing file", "<Control>o", 0, 0 },
    { "save_file", "Save file", "<Control>s", 0, 0 },
    { "save_file_as", "Save file as...", "<Control><Shift>s", 0, 0 },
    { "export_pdf", "Print / Export PDF", "<Control><Shift>p", 0, 0 },
    { "quick_switch", "Quick open file", "<Control>p", 0, 0 },
    { "toggle_search", "Toggle search bar", "<Control>f", 0, 0 },
    { "replace_text", "Replace text", "<Control>h", 0, 0 },
    { "close_tab", "Close file / tab", "<Control>w", 0, 0 },
    { "open_settings", "Open settings", "<Control>comma", 0, 0 },
    { "shortcuts_ref", "Shortcuts reference", "<Control><Shift>slash", 0, 0 },
    { "toggle_sidebar", "Toggle Sidebar", "F9", 0, 0 },
    { "toggle_read_mode", "Toggle read mode", "<Control>e", 0, 0 },
    { "inline_code", "Inline code", "<Control>k", 0, 0 },
    { "highlight", "Highlight", "<Control><Shift>h", 0, 0 },
    { "blockquote", "Blockquote", "<Control>q", 0, 0 },
    { "math", "Math", "<Control>m", 0, 0 },
    { "ordered_list", "Ordered list", "<Control><Shift>o", 0, 0 },
    { "task_list", "Task list", "<Control><Shift>t", 0, 0 },
    { "insert_hr", "Insert horizontal rule", "<Control><Shift>minus", 0, 0 }
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

void init_app_shortcuts(void) {
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

void shortcuts_cancel_listening(void) {
    if (shortcut_listening_index >= 0) {
        int old_idx = shortcut_listening_index;
        shortcut_listening_index = -1;
        if (shortcut_edit_buttons[old_idx]) {
            gtk_button_set_label(GTK_BUTTON(shortcut_edit_buttons[old_idx]), "Edit");
        }
    }
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

void show_keybindings_window(AppGui *gui) {
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), qirtas_tr("Keyboard Shortcuts"));
    gtk_window_set_default_size(GTK_WINDOW(dialog), 550, 600);
    /* Float above the settings window if it's open, else the main window. */
    GtkWindow *parent = (gui->settings_window && gtk_widget_get_visible(gui->settings_window))
                            ? GTK_WINDOW(gui->settings_window) : GTK_WINDOW(gui->window);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);
    /* Non-modal: a modal shortcuts window blocked editing AND prevented the
     * settings window from closing until it was dismissed. */
    gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
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
        GtkWidget *_s = gtk_label_new(qirtas_tr(label_text)); \
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
        GtkWidget *_d = gtk_label_new(qirtas_tr(description)); \
        gtk_widget_add_css_class(_d, "kb-desc"); \
        gtk_widget_set_halign(_d, GTK_ALIGN_START); \
        gtk_widget_set_hexpand(_d, TRUE); \
        gtk_box_append(GTK_BOX(_row), _k); \
        gtk_box_append(GTK_BOX(_row), _d); \
        if (_idx >= 0) { \
            shortcut_value_labels[_idx] = _k; \
            GtkWidget *_edit_btn = gtk_button_new_with_label(qirtas_tr("Edit")); \
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
    KB_ROW("F11",            "Fullscreen", "fullscreen");
    KB_ROW("Ctrl + Shift+F", "Focus mode", "focus_mode");

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
    KB_ROW("Ctrl + Shift+/", "Shortcuts reference", "shortcuts_ref");
    KB_ROW("F9",             "Toggle Sidebar", "toggle_sidebar");

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
