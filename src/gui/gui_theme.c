#include "gui_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static char custom_theme_path[1024] = "";
static const char *CSS_FALLBACK_MINIMAL =
    "* { font-family: 'Inter', 'Lora', 'Merriweather', 'Cairo', 'system-ui', sans-serif; }\n"
    ".sidebar   { min-width: 120px; padding: 16px 12px; }\n"
    ".workspace { padding: 0; }\n"
    "textview   { padding: 24px; }\n"
    ".search-bar-revealer { padding: 8px 16px; }\n"
    ".tree-container { background: transparent; }\n"
    ".tree-row   { padding: 4px 8px; border-radius: 6px; }\n"
    ".bottom-bar { padding: 4px 12px; }\n";

/* Resolve a bundled resource (CSS, logo, icons) given a repo-relative path like
 * "src/ui/themes/base.css". Search order:
 *   1. $QIRTAS_DATA_DIR/<rel>            (explicit override)
 *   2. <exe>/../../<rel>                 (running from the build tree)
 *   3. <exe>/<rel>                       (assets alongside the binary)
 *   4. /usr/share/qirtas/<rel>,
 *      /usr/local/share/qirtas/<rel>     (system install)
 * Falls back to the bare relative path if nothing exists, so callers still get
 * a non-NULL string (load then fails into the minimal CSS / placeholder). */
static char *resolve_resource_path(const char *rel_path) {
    static char abs_path[2048];

    const char *data_dir = getenv("QIRTAS_DATA_DIR");
    if (data_dir && data_dir[0]) {
        snprintf(abs_path, sizeof(abs_path), "%s/%s", data_dir, rel_path);
        if (access(abs_path, F_OK) == 0) return abs_path;
    }

    char exe_path[1024] = {0};
    ssize_t exe_len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (exe_len > 0) {
        exe_path[exe_len] = '\0';
        char *last_slash = strrchr(exe_path, '/');
        if (last_slash) *last_slash = '\0';

        snprintf(abs_path, sizeof(abs_path), "%s/../../%s", exe_path, rel_path);
        if (access(abs_path, F_OK) == 0) return abs_path;

        snprintf(abs_path, sizeof(abs_path), "%s/%s", exe_path, rel_path);
        if (access(abs_path, F_OK) == 0) return abs_path;
    }

    static const char *const sys_prefixes[] = {
        "/usr/share/qirtas", "/usr/local/share/qirtas",
    };
    for (size_t i = 0; i < sizeof(sys_prefixes) / sizeof(sys_prefixes[0]); i++) {
        snprintf(abs_path, sizeof(abs_path), "%s/%s", sys_prefixes[i], rel_path);
        if (access(abs_path, F_OK) == 0) return abs_path;
    }

    strncpy(abs_path, rel_path, sizeof(abs_path) - 1);
    abs_path[sizeof(abs_path) - 1] = '\0';
    return abs_path;
}

static int theme_name_to_index(const char *theme_name) {
    if (!theme_name) return 0;
    if (strcmp(theme_name, "qirtas") == 0) return 0;
    if (strcmp(theme_name, "qirtas-dark") == 0) return 1;
    if (strcmp(theme_name, "navy") == 0) return 2;
    if (strcmp(theme_name, "custom") == 0) return 3;
    return 0;
}

static void on_custom_theme_dialog_response(GObject *source_object, GAsyncResult *res, gpointer user_data);
void on_theme_dropdown_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);

void update_editor_font(AppGui *gui) {
    if (!gui->font_provider) {
        gui->font_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(),
            GTK_STYLE_PROVIDER(gui->font_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
    /* This provider sits at APPLICATION+1, above the theme provider —
     * the caret color it emits always wins, so the custom pointer color
     * must be honored HERE (emitting it only in the theme provider made
     * the setting a no-op). */
    char caret_value[64];
    if (gui->cursor.use_custom_pointer_color) {
        gchar *rgba_str = gdk_rgba_to_string(&gui->cursor.custom_pointer_color);
        g_strlcpy(caret_value, rgba_str, sizeof(caret_value));
        g_free(rgba_str);
    } else {
        g_strlcpy(caret_value, "var(--caret-color, var(--accent))", sizeof(caret_value));
    }

    char css[1024];
    snprintf(css, sizeof(css),
        "textview text, textview.sourceview text, textview {\n"
        "    font-family: \"%s\", \"%s\", \"serif\";\n"
        "    font-size: %.0fpx;\n"
        "    line-height: 1.45;\n"
        "    caret-color: %s;\n"
        "}\n"
        "h1, h2, h3 {\n"
        "    font-family: \"Cairo\", \"Inter\", \"system-ui\", sans-serif;\n"
        "}",
        gui->current_en_font, gui->current_ar_font, gui->current_font_size,
        caret_value);
    gtk_css_provider_load_from_string(gui->font_provider, css);
    gui_remeasure_line_height();
}

static void on_custom_font_dialog_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkFontDialog *dialog = GTK_FONT_DIALOG(source_object);
    GError *error = NULL;
    PangoFontFamily *family = gtk_font_dialog_choose_family_finish(dialog, res, &error);
    AppGui *gui = (AppGui *)user_data;
    GtkDropDown *dropdown = g_object_get_data(G_OBJECT(dialog), "en-font-dropdown");

    if (family) {
        const char *font_name = pango_font_family_get_name(family);
        if (font_name && dropdown) {
            GListModel *model = gtk_drop_down_get_model(dropdown);
            if (model && GTK_IS_STRING_LIST(model)) {
                GtkStringList *string_list = GTK_STRING_LIST(model);
                guint n_items = g_list_model_get_n_items(model);
                int found_idx = -1;
                for (guint i = 0; i < n_items; i++) {
                    const char *item_text = gtk_string_list_get_string(string_list, i);
                    if (item_text && strcmp(item_text, font_name) == 0) {
                        found_idx = i;
                        break;
                    }
                }
                
                if (found_idx != -1) {
                    gtk_drop_down_set_selected(dropdown, found_idx);
                } else {
                    if (n_items > 0) {
                        gtk_string_list_remove(string_list, n_items - 1);
                        gtk_string_list_append(string_list, font_name);
                        gtk_string_list_append(string_list, "Add Custom Font...");
                        gtk_drop_down_set_selected(dropdown, n_items - 1);
                    }
                }
            }
        }
        g_object_unref(family);
    } else {
        if (error) {
            g_printerr("[font] Font dialog error: %s\n", error->message);
            g_error_free(error);
        }
        if (dropdown) {
            GListModel *model = gtk_drop_down_get_model(dropdown);
            if (model && GTK_IS_STRING_LIST(model)) {
                GtkStringList *string_list = GTK_STRING_LIST(model);
                guint n_items = g_list_model_get_n_items(model);
                int prev_idx = 0;
                for (guint i = 0; i < n_items; i++) {
                    const char *item_text = gtk_string_list_get_string(string_list, i);
                    if (item_text && strcmp(item_text, gui->current_en_font) == 0) {
                        prev_idx = i;
                        break;
                    }
                }
                g_signal_handlers_block_by_func(dropdown, G_CALLBACK(on_en_font_changed), gui);
                gtk_drop_down_set_selected(dropdown, prev_idx);
                g_signal_handlers_unblock_by_func(dropdown, G_CALLBACK(on_en_font_changed), gui);
            }
        }
    }
}

void on_en_font_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    AppGui *gui = (AppGui *)user_data;
    if (!gui) return;
    GtkDropDown *dropdown = GTK_DROP_DOWN(gobject);
    guint selected = gtk_drop_down_get_selected(dropdown);

    GListModel *model = gtk_drop_down_get_model(dropdown);
    if (!model || !GTK_IS_STRING_LIST(model)) return;
    GtkStringList *string_list = GTK_STRING_LIST(model);
    const char *selected_text = gtk_string_list_get_string(string_list, selected);

    if (selected_text) {
        if (strcmp(selected_text, "Add Custom Font...") == 0) {
            GtkFontDialog *dialog = gtk_font_dialog_new();
            gtk_font_dialog_set_modal(dialog, TRUE);
            g_object_set_data(G_OBJECT(dialog), "en-font-dropdown", dropdown);
            
            GtkWindow *parent_win = gui->settings_window ? GTK_WINDOW(gui->settings_window) : GTK_WINDOW(gui->window);
            gtk_font_dialog_choose_family(dialog, parent_win, NULL, NULL, on_custom_font_dialog_response, gui);
            g_object_unref(dialog);
        } else {
            g_strlcpy(gui->current_en_font, selected_text, sizeof(gui->current_en_font));
            update_editor_font(gui);
        }
    }
}

void on_ar_font_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    AppGui *gui = (AppGui *)user_data;
    if (!gui) return;
    GtkDropDown *dropdown = GTK_DROP_DOWN(gobject);
    guint selected = gtk_drop_down_get_selected(dropdown);
    static const char *const ar_fonts[] = {
        "Amiri",
        "Cairo",
        "IBM Plex Sans Arabic",
        "KFGQPC Uthman Taha Naskh",
        "Noto Naskh Arabic",
    };
    if (selected < G_N_ELEMENTS(ar_fonts)) {
        g_strlcpy(gui->current_ar_font, ar_fonts[selected], sizeof(gui->current_ar_font));
        update_editor_font(gui);
    }
}

gboolean qirtas_theme_is_dark(const char *theme_name) {
    if (!theme_name) return FALSE;
    if (strcmp(theme_name, "sepia") == 0 ||
        strcmp(theme_name, "typewriter-light") == 0 ||
        strcmp(theme_name, "qirtas") == 0 ||
        strcmp(theme_name, "navy") == 0) {
        return FALSE;
    }
    return TRUE;
}

/* Swap the sidebar brand logo between the light-mode (dark ink on pale
 * paper) and dark-mode (pale ink) PNGs. Safe to call before gui->logo_image
 * exists (initial build) or repeatedly on every theme change. */
void gui_update_brand_logo(AppGui *gui) {
    if (!gui || !gui->logo_image || !GTK_IS_IMAGE(gui->logo_image)) return;

    const char *rel_path = qirtas_theme_is_dark(gui->current_theme)
        ? "src/ui/icons/qirtas-logo-dark.png"
        : "src/ui/icons/qirtas-logo.png";

    GError *logo_err = NULL;
    GdkPixbuf *logo_pixbuf = gdk_pixbuf_new_from_file_at_scale(
        resolve_resource_path(rel_path), -1, 160, TRUE, &logo_err);
    if (logo_pixbuf) {
        GdkTexture *logo_tex = gdk_texture_new_for_pixbuf(logo_pixbuf);
        gtk_image_set_from_paintable(GTK_IMAGE(gui->logo_image), GDK_PAINTABLE(logo_tex));
        gtk_image_set_pixel_size(GTK_IMAGE(gui->logo_image), 48);
        g_object_unref(logo_tex);
        g_object_unref(logo_pixbuf);
    } else {
        if (logo_err) g_error_free(logo_err);
        gtk_image_set_from_icon_name(GTK_IMAGE(gui->logo_image), "text-editor-symbolic");
        gtk_image_set_pixel_size(GTK_IMAGE(gui->logo_image), 32);
    }
}

void apply_theme(AppGui *gui, const char *theme_name) {
    if (!gui || !theme_name) return;
    /* Removed themes (sepia, typewriter-*, legacy dark/midnight/things) fall
     * back to the default light theme so old saved prefs still resolve. */
    if (strcmp(theme_name, "sepia") == 0 || strcmp(theme_name, "typewriter-light") == 0 ||
        strcmp(theme_name, "typewriter-dark") == 0 || strcmp(theme_name, "dark") == 0 ||
        strcmp(theme_name, "midnight") == 0 || strcmp(theme_name, "things") == 0) {
        theme_name = "qirtas";
    }
    g_strlcpy(gui->current_theme, theme_name, sizeof(gui->current_theme));
    /* Persist the choice so it is the default on next launch. */
    qirtas_pref_set_string("theme", theme_name);

    AdwStyleManager *style_manager = adw_style_manager_get_default();
    if (strcmp(theme_name, "sepia") == 0 || strcmp(theme_name, "typewriter-light") == 0 || strcmp(theme_name, "qirtas") == 0 || strcmp(theme_name, "navy") == 0) {
        adw_style_manager_set_color_scheme(style_manager, ADW_COLOR_SCHEME_FORCE_LIGHT);
    } else {
        adw_style_manager_set_color_scheme(style_manager, ADW_COLOR_SCHEME_FORCE_DARK);
    }

    gui_update_brand_logo(gui);

    const char *fallback_css     = CSS_FALLBACK_MINIMAL;
    const char *gutter_color     = "#8a8173";
    const char *active_num_color = "#1b1816";
    const char *theme_css_path   = "src/ui/themes/theme-qirtas-light.css";

    if (strcmp(theme_name, "sepia") == 0) {
        gutter_color     = "#586e75";
        active_num_color = "#a42e79";
        theme_css_path   = "src/ui/themes/theme-sepia.css";
    } else if (strcmp(theme_name, "typewriter-light") == 0) {
        gutter_color     = "#777777";
        active_num_color = "#b82e2e";
        theme_css_path   = "src/ui/themes/theme-typewriter-light.css";
    } else if (strcmp(theme_name, "typewriter-dark") == 0) {
        gutter_color     = "#666666";
        active_num_color = "#ff4d4d";
        theme_css_path   = "src/ui/themes/theme-typewriter-dark.css";
    } else if (strcmp(theme_name, "qirtas") == 0) {
        gutter_color     = "#8a8173";
        active_num_color = "#1b1816";
        theme_css_path   = "src/ui/themes/theme-qirtas-light.css";
    } else if (strcmp(theme_name, "qirtas-dark") == 0) {
        gutter_color     = "#6d7385";
        active_num_color = "#c9a86b";
        theme_css_path   = "src/ui/themes/theme-qirtas-dark.css";
    } else if (strcmp(theme_name, "navy") == 0) {
        gutter_color     = "#a59d88";
        active_num_color = "#213a63";
        theme_css_path   = "src/ui/themes/theme-qirtas-navy.css";
    } else if (strcmp(theme_name, "custom") == 0) {
        gutter_color     = "#555555";
        active_num_color = "#2e80f2";
        theme_css_path   = custom_theme_path;
    }

    gchar *file_css        = NULL;
    gchar *base_layout_css = NULL;

    const char *resolved_theme_path = (theme_css_path[0] == '/') ? theme_css_path : resolve_resource_path(theme_css_path);
    GFile  *theme_file = g_file_new_for_path(resolved_theme_path);
    GError *file_error = NULL;

    if (g_file_query_exists(theme_file, NULL)) {
        gsize len = 0;
        if (!g_file_load_contents(theme_file, NULL, &file_css, &len, NULL, &file_error)) {
            g_printerr("[theme] Could not read theme variables %s: %s\n",
                       resolved_theme_path, file_error ? file_error->message : "unknown");
            if (file_error) { g_error_free(file_error); file_error = NULL; }
        }
    }
    g_object_unref(theme_file);

    const char *resolved_base_path = resolve_resource_path("src/ui/themes/base.css");
    GFile *base_file = g_file_new_for_path(resolved_base_path);
    if (g_file_query_exists(base_file, NULL)) {
        g_file_load_contents(base_file, NULL, &base_layout_css, NULL, NULL, NULL);
    }
    g_object_unref(base_file);

    gchar *css_body = NULL;
    if (file_css) {
        css_body = g_strdup_printf("%s\n%s", file_css, base_layout_css ? base_layout_css : fallback_css);
    } else {
        css_body = g_strdup(fallback_css);
    }

    if (gui->css_provider) {
        gchar *caret_css = NULL;
        if (gui->cursor.use_custom_pointer_color) {
            gchar *rgba_str = gdk_rgba_to_string(&gui->cursor.custom_pointer_color);
            caret_css = g_strdup_printf(
                "textview, textview.sourceview, .editor-source {\n"
                "  caret-color: %s;\n"
                "}\n"
                "textview text, textview.sourceview text, .editor-source text {\n"
                "  caret-color: %s;\n"
                "}\n",
                rgba_str, rgba_str
            );
            g_free(rgba_str);
        } else {
            caret_css = g_strdup("");
        }

        gchar *full_css = g_strdup_printf(
            "%s\n"
            "%s\n"
            "textview selection { padding: 0px; }\n"
            "textview.sourceview selection { padding: 0px; }\n"
            "textview > border { background-color: var(--bg-window); }\n"
            "gutterview { background-color: var(--bg-card); color: %s; }\n"
            "gutterview > line { color: %s; }\n"
            "gutterview > line.current-line-number {\n"
            "  color: %s;\n"
            "  font-weight: bold;\n"
            "}\n"
            "gutter { background-color: var(--bg-card); color: %s; }\n"
            "gutter > line { color: %s; }\n"
            "gutter > line.current-line-number {\n"
            "  color: %s;\n"
            "  font-weight: bold;\n"
            "}\n"
            ".linked > menubutton.add-action-btn:first-child,\n"
            ".linked > menubutton.add-action-btn:first-child > button {\n"
            "  border-top-right-radius: 0px;\n"
            "  border-bottom-right-radius: 0px;\n"
            "  border-right-style: none;\n"
            "}\n"
            ".linked > menubutton.add-action-btn:last-child,\n"
            ".linked > menubutton.add-action-btn:last-child > button {\n"
            "  border-top-left-radius: 0px;\n"
            "  border-bottom-left-radius: 0px;\n"
            "  border-left-style: none;\n"
            "}\n"
            "separator.hr-line {\n"
            "  background-color: %s;\n"
            "  min-height: 1px;\n"
            "  margin-top: 16px;\n"
            "  margin-bottom: 16px;\n"
            "}\n"
            /* Fenced-code-block header pill. Theme-agnostic: a translucent grey
             * fill works on light and dark, and the label/button inherit the
             * view's text colour via opacity rather than a hard-coded colour. */
            ".code-pill {\n"
            "  background-color: rgba(127,127,127,0.12);\n"
            "  border-radius: 8px;\n"
            "  padding: 1px 6px 1px 12px;\n"
            "  margin-top: 8px;\n"
            "  margin-bottom: 2px;\n"
            "}\n"
            ".code-pill-lang {\n"
            "  font-size: 0.82em;\n"
            "  font-weight: 600;\n"
            "  letter-spacing: 0.04em;\n"
            "  opacity: 0.65;\n"
            "}\n"
            ".code-pill-copy {\n"
            "  min-height: 22px;\n"
            "  min-width: 22px;\n"
            "  padding: 2px;\n"
            "  opacity: 0.7;\n"
            "}\n"
            ".code-pill-copy:hover { opacity: 1; }\n"
            /* Rendered markdown table. Translucent borders/fills read on light
             * and dark; cell text inherits the view colour. */
            ".md-table {\n"
            "  border: 1px solid rgba(127,127,127,0.35);\n"
            "  border-radius: 6px;\n"
            "  margin-top: 6px;\n"
            "  margin-bottom: 6px;\n"
            "}\n"
            ".md-table-cell {\n"
            "  padding: 4px 12px;\n"
            "  border-right: 1px solid rgba(127,127,127,0.18);\n"
            "  border-bottom: 1px solid rgba(127,127,127,0.18);\n"
            "}\n"
            ".md-table-header {\n"
            "  font-weight: 700;\n"
            "  background-color: rgba(127,127,127,0.12);\n"
            "}\n"
            ".copy-toast {\n"
            "  background-color: rgba(0,0,0,0.82);\n"
            "  color: #ffffff;\n"
            "  padding: 6px 14px;\n"
            "  border-radius: 14px;\n"
            "  font-size: 0.9em;\n"
            "}\n",
            css_body,
            caret_css,
            gutter_color, gutter_color, active_num_color,
            gutter_color, gutter_color, active_num_color,
            gutter_color
        );
        gtk_css_provider_load_from_string(gui->css_provider, full_css);
        g_free(full_css);
        g_free(caret_css);
    }

    if (file_css) g_free(file_css);
    if (base_layout_css) g_free(base_layout_css);
    if (css_body) g_free(css_body);

    if (gui->source_view) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
        GtkSourceBuffer *src_buf = GTK_SOURCE_BUFFER(buf);
        GtkSourceStyleSchemeManager *sm = gtk_source_style_scheme_manager_get_default();
        GtkSourceStyleScheme *scheme = NULL;
        if (strcmp(theme_name, "sepia") == 0) {
            scheme = gtk_source_style_scheme_manager_get_scheme(sm, "qirtas-sepia");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "classic");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "solarized-light");
        } else if (strcmp(theme_name, "typewriter-light") == 0) {
            scheme = gtk_source_style_scheme_manager_get_scheme(sm, "qirtas-typewriter-light");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "classic");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "solarized-light");
        } else if (strcmp(theme_name, "typewriter-dark") == 0) {
            scheme = gtk_source_style_scheme_manager_get_scheme(sm, "qirtas-typewriter-dark");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "qirtas-dark");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "adwaita-dark");
        } else if (strcmp(theme_name, "qirtas") == 0) {
            scheme = gtk_source_style_scheme_manager_get_scheme(sm, "qirtas");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "classic");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "solarized-light");
        } else if (strcmp(theme_name, "qirtas-dark") == 0) {
            scheme = gtk_source_style_scheme_manager_get_scheme(sm, "qirtas-night");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "qirtas-dark");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "adwaita-dark");
        } else if (strcmp(theme_name, "navy") == 0) {
            scheme = gtk_source_style_scheme_manager_get_scheme(sm, "qirtas-navy");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "qirtas");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "classic");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "solarized-light");
        } else if (strcmp(theme_name, "custom") == 0) {
            scheme = gtk_source_style_scheme_manager_get_scheme(sm, "qirtas-dark");
            if (!scheme) scheme = gtk_source_style_scheme_manager_get_scheme(sm, "adwaita-dark");
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
    gui_remeasure_line_height();
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
            int idx = theme_name_to_index(gui->current_theme);
            g_signal_handlers_block_by_func(dropdown, G_CALLBACK(on_theme_dropdown_changed), gui);
            gtk_drop_down_set_selected(dropdown, idx);
            g_signal_handlers_unblock_by_func(dropdown, G_CALLBACK(on_theme_dropdown_changed), gui);
        }
    }
}

void on_theme_dropdown_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    GtkDropDown *dropdown = GTK_DROP_DOWN(gobject);
    guint selected = gtk_drop_down_get_selected(dropdown);
    AppGui *gui = (AppGui *)user_data;
    
    if (selected == 0) {
        apply_theme(gui, "qirtas");
    } else if (selected == 1) {
        apply_theme(gui, "qirtas-dark");
    } else if (selected == 2) {
        apply_theme(gui, "navy");
    } else if (selected == 3) {
        GtkFileDialog *dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, qirtas_tr("Select Custom Theme CSS"));
        
        GtkFileFilter *filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, "CSS Files (*.css)");
        gtk_file_filter_add_pattern(filter, "*.css");
        
        GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
        g_list_store_append(filters, filter);
        g_object_unref(filter);
        gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
        g_object_unref(filters);
        
        g_object_set_data(G_OBJECT(dialog), "theme-dropdown", dropdown);
        gtk_file_dialog_open(dialog, GTK_WINDOW(gui->window), NULL, on_custom_theme_dialog_response, gui);
    }
}

void init_css(AppGui *gui) {
    (void)gui;

    GtkCssProvider *p = gtk_css_provider_new();
    gui->css_provider = p;
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(p),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(p);
    
    apply_theme(gui, gui->current_theme);
    update_editor_font(gui);
}
