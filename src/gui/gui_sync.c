#include "gui_internal.h"
#include <sqlite3.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>

#define GITHUB_CLIENT_ID "Iv23lipYNklHlDKatIw0"
#define DROPBOX_APP_KEY "YOUR_PUBLIC_DROPBOX_KEY"
#define GOOGLE_CLIENT_ID "YOUR_PUBLIC_GOOGLE_ID"
#define GITHUB_DEFAULT_REPO "qirtas-notes"

static gboolean provider_id_configured(const char *value, const char *placeholder) {
    return value && value[0] != '\0' && strcmp(value, placeholder) != 0;
}

/* Provider app IDs can be overridden at runtime so users don't have to
 * rebuild to plug in their own OAuth app:
 *   QIRTAS_GOOGLE_CLIENT_ID, QIRTAS_DROPBOX_APP_KEY, QIRTAS_GITHUB_CLIENT_ID */
static const char *google_client_id(void) {
    const char *env = g_getenv("QIRTAS_GOOGLE_CLIENT_ID");
    return (env && env[0]) ? env : GOOGLE_CLIENT_ID;
}

static const char *dropbox_app_key(void) {
    const char *env = g_getenv("QIRTAS_DROPBOX_APP_KEY");
    return (env && env[0]) ? env : DROPBOX_APP_KEY;
}

static const char *github_client_id(void) {
    const char *env = g_getenv("QIRTAS_GITHUB_CLIENT_ID");
    return (env && env[0]) ? env : GITHUB_CLIENT_ID;
}

/* Helper: parse simple JSON string value */
static char *parse_json_value(const char *json, const char *key) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    const char *pos = strstr(json, search_key);
    if (!pos) {
        snprintf(search_key, sizeof(search_key), "\"%s\" :", key);
        pos = strstr(json, search_key);
    }
    if (!pos) return NULL;
    pos = strchr(pos, ':');
    if (!pos) return NULL;
    pos++;
    // skip whitespace
    while (*pos == ' ' || *pos == '\t') pos++;
    if (*pos == '"') {
        pos++;
        const char *end = strchr(pos, '"');
        if (!end) return NULL;
        return g_strndup(pos, end - pos);
    } else {
        const char *end = pos;
        while (*end && *end != ',' && *end != '}' && *end != '\r' && *end != '\n') end++;
        return g_strndup(pos, end - pos);
    }
}

/* Helper: run curl without invoking a shell. */
static int run_curl_post(const char *url, const char *body, char *response_buf, int buf_size) {
    if (!response_buf || buf_size <= 0) return -1;
    response_buf[0] = '\0';

    const char *argv[] = {
        "curl", "-sS", "-X", "POST",
        "-H", "Accept: application/json",
        "-d", body,
        url,
        NULL
    };

    gchar *stdout_data = NULL;
    gchar *stderr_data = NULL;
    gint exit_status = 0;
    GError *error = NULL;
    gboolean ok = g_spawn_sync(NULL, (gchar **)argv, NULL,
                               G_SPAWN_SEARCH_PATH,
                               NULL, NULL,
                               &stdout_data, &stderr_data,
                               &exit_status, &error);
    if (!ok) {
        if (error) g_error_free(error);
        g_free(stdout_data);
        g_free(stderr_data);
        return -1;
    }

    if (exit_status != 0 || !stdout_data) {
        g_free(stdout_data);
        g_free(stderr_data);
        return -1;
    }

    g_strlcpy(response_buf, stdout_data, (gsize)buf_size);
    int total = (int)strlen(response_buf);
    g_free(stdout_data);
    g_free(stderr_data);
    return total;
}

/* ──────────────────────────────────────────────────────────
 * GITHUB DEVICE FLOW IMPLEMENTATION
 * ────────────────────────────────────────────────────────── */

typedef struct {
    AppGui *gui;
    char *device_code;
    char *user_code;
    char *verification_uri;
    gboolean *cancelled;
    GtkWidget *dialog;
} GithubPollData;

typedef struct {
    AppGui *gui;
    char *token;
    gboolean success;
    GtkWidget *dialog_to_close;
} UIUpdateData;

static gboolean update_github_ui_idle(gpointer user_data) {
    UIUpdateData *ud = (UIUpdateData *)user_data;
    if (ud->success) {
        const char *repo = GITHUB_DEFAULT_REPO;
        if (ud->gui->github_repo_entry) {
            const char *entered = gtk_editable_get_text(GTK_EDITABLE(ud->gui->github_repo_entry));
            if (entered && entered[0] != '\0') repo = entered;
        }
        zig_save_github_credentials(ud->token, repo);
        gui_update_github_status(1, "Connected");
    } else {
        gui_update_github_status(0, "Disconnected");
    }
    if (ud->dialog_to_close && GTK_IS_WINDOW(ud->dialog_to_close)) {
        gtk_window_destroy(GTK_WINDOW(ud->dialog_to_close));
    }
    g_free(ud->token);
    g_free(ud);
    return FALSE;
}

static gpointer github_poll_thread_func(gpointer user_data) {
    GithubPollData *pd = (GithubPollData *)user_data;
    int elapsed = 0;
    
    while (elapsed < 900) {
        if (*(pd->cancelled)) {
            break;
        }
        
        g_usleep(5000000); // Sleep 5 seconds
        
        if (*(pd->cancelled)) {
            break;
        }
        
        char response[2048];
        char *body = g_strdup_printf("client_id=%s&device_code=%s&grant_type=urn:ietf:params:oauth:grant-type:device_code",
                                    github_client_id(), pd->device_code);
        int len = run_curl_post("https://github.com/login/oauth/access_token", body, response, sizeof(response));
        g_free(body);
        
        if (len > 0) {
            char *err = parse_json_value(response, "error");
            if (err) {
                if (strcmp(err, "authorization_pending") == 0) {
                    g_free(err);
                } else if (strcmp(err, "slow_down") == 0) {
                    g_free(err);
                    g_usleep(5000000);
                } else {
                    g_free(err);
                    break;
                }
            } else {
                char *access_token = parse_json_value(response, "access_token");
                if (access_token) {
                    UIUpdateData *ud = g_new0(UIUpdateData, 1);
                    ud->gui = pd->gui;
                    ud->token = access_token;
                    ud->success = TRUE;
                    ud->dialog_to_close = pd->dialog;
                    g_idle_add(update_github_ui_idle, ud);
                    break;
                }
            }
        }
        elapsed += 5;
    }
    
    g_free(pd->device_code);
    g_free(pd->user_code);
    g_free(pd->verification_uri);
    g_free(pd);
    return NULL;
}

static void on_dialog_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    gboolean *cancelled = (gboolean *)user_data;
    *cancelled = TRUE;
}

static void on_open_activation_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    const char *uri = (const char *)user_data;
    gtk_show_uri(NULL, uri, 0);
}

typedef struct {
    AppGui *gui;
    char *user_code;
    char *verification_uri;
    char *device_code;
} GithubDialogData;

static gboolean show_github_dialog_idle(gpointer user_data) {
    GithubDialogData *dd = (GithubDialogData *)user_data;
    AppGui *gui = dd->gui;
    
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "GitHub Authorization");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 380, 240);
    if (gui->settings_window) {
        gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(gui->settings_window));
    }
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(vbox, 24);
    gtk_widget_set_margin_end(vbox, 24);
    gtk_widget_set_margin_top(vbox, 24);
    gtk_widget_set_margin_bottom(vbox, 24);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
    gtk_window_set_child(GTK_WINDOW(dialog), vbox);
    
    GtkWidget *lbl_title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_title), "<span size='large' weight='bold'>Link GitHub Account</span>");
    gtk_widget_set_halign(lbl_title, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(vbox), lbl_title);
    
    GtkWidget *lbl_desc = gtk_label_new("Activate your device by entering the following code on GitHub:");
    gtk_widget_set_halign(lbl_desc, GTK_ALIGN_CENTER);
    gtk_label_set_wrap(GTK_LABEL(lbl_desc), TRUE);
    gtk_box_append(GTK_BOX(vbox), lbl_desc);
    
    GtkWidget *lbl_code = gtk_label_new(NULL);
    char *markup = g_strdup_printf("<span size='xx-large' weight='bold' font_family='monospace'>%s</span>", dd->user_code);
    gtk_label_set_markup(GTK_LABEL(lbl_code), markup);
    g_free(markup);
    gtk_widget_set_halign(lbl_code, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(vbox), lbl_code);
    
    GtkWidget *btn_open = gtk_button_new_with_label("Open Activation Page");
    gtk_widget_add_css_class(btn_open, "pop-btn");
    g_signal_connect(btn_open, "clicked", G_CALLBACK(on_open_activation_clicked), dd->verification_uri);
    gtk_box_append(GTK_BOX(vbox), btn_open);
    
    GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
    gtk_widget_add_css_class(btn_cancel, "pop-btn");
    g_signal_connect_swapped(btn_cancel, "clicked", G_CALLBACK(gtk_window_destroy), dialog);
    gtk_box_append(GTK_BOX(vbox), btn_cancel);
    
    gboolean *cancelled = g_new0(gboolean, 1);
    *cancelled = FALSE;
    g_object_set_data_full(G_OBJECT(dialog), "cancelled-ptr", cancelled, g_free);
    g_signal_connect(dialog, "destroy", G_CALLBACK(on_dialog_destroy), cancelled);
    
    gtk_window_present(GTK_WINDOW(dialog));
    
    // Automatically trigger browser open
    gtk_show_uri(NULL, dd->verification_uri, 0);
    
    // Start polling thread
    GithubPollData *pd = g_new0(GithubPollData, 1);
    pd->gui = gui;
    pd->device_code = g_strdup(dd->device_code);
    pd->user_code = g_strdup(dd->user_code);
    pd->verification_uri = g_strdup(dd->verification_uri);
    pd->cancelled = cancelled;
    pd->dialog = dialog;
    
    g_thread_new("github_poll", github_poll_thread_func, pd);
    
    g_free(dd->device_code);
    g_free(dd->user_code);
    g_free(dd->verification_uri);
    g_free(dd);
    return FALSE;
}

static gpointer github_device_flow_thread(gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    char response[2048];
    char *body = g_strdup_printf("client_id=%s&scope=repo", github_client_id());
    int len = run_curl_post("https://github.com/login/device/code", body, response, sizeof(response));
    g_free(body);
    if (len > 0) {
        char *device_code = parse_json_value(response, "device_code");
        char *user_code = parse_json_value(response, "user_code");
        char *verification_uri = parse_json_value(response, "verification_uri");
        
        if (device_code && user_code && verification_uri) {
            GithubDialogData *dd = g_new0(GithubDialogData, 1);
            dd->gui = gui;
            dd->device_code = device_code; // transfer ownership
            dd->user_code = user_code;     // transfer ownership
            dd->verification_uri = verification_uri; // transfer ownership
            g_idle_add(show_github_dialog_idle, dd);
            return NULL;
        }
        g_free(device_code);
        g_free(user_code);
        g_free(verification_uri);
    }
    
    gui_update_github_status(0, "Connection failed");
    return NULL;
}

void start_github_device_flow(AppGui *gui) {
    gui_update_github_status(2, "Generating code...");
    g_thread_new("github_device_flow", github_device_flow_thread, gui);
}

/* ──────────────────────────────────────────────────────────
 * DROPBOX & GOOGLE DRIVE LOOPBACK FLOW IMPLEMENTATION
 * ────────────────────────────────────────────────────────── */

typedef struct {
    AppGui *gui;
    int service_type; // 1 = Google Drive, 2 = Dropbox
    int port;
    int server_fd;
} LoopbackThreadData;

static gpointer loopback_thread_func(gpointer user_data) {
    LoopbackThreadData *ld = (LoopbackThreadData *)user_data;
    int server_fd = ld->server_fd;
    int service = ld->service_type;
    
    // Timeout of 5 minutes (300 seconds)
    struct timeval tv;
    tv.tv_sec = 300;
    tv.tv_usec = 0;
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd >= 0) {
        char req_buf[4096] = {0};
        int bytes_read = recv(client_fd, req_buf, sizeof(req_buf) - 1, 0);
        if (bytes_read > 0) {
            char *code_start = strstr(req_buf, "code=");
            if (code_start) {
                code_start += 5;
                char *code_end = strpbrk(code_start, " &\r\n");
                if (code_end) {
                    *code_end = '\0';
                }
                
                const char *response_tmpl =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/html\r\n"
                    "Connection: close\r\n\r\n"
                    "<html>"
                    "<head><style>"
                    "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; text-align: center; padding: 50px; background-color: #faf9f6; color: #4a3e3d; }"
                    ".card { max-width: 450px; margin: 0 auto; padding: 40px; border-radius: 16px; background-color: #fff; box-shadow: 0 4px 20px rgba(0,0,0,0.05); border: 1px solid #e8e6df; }"
                    "h1 { color: #5c8b57; font-size: 26px; margin-bottom: 16px; font-weight: 600; }"
                    "p { font-size: 16px; line-height: 1.6; color: #6e6564; }"
                    "</style></head>"
                    "<body><div class='card'>"
                    "<h1>Qirtas Connected Successfully</h1>"
                    "<p>Authentication code received. You can now close this browser tab and return to the application.</p>"
                    "</div></body></html>";
                
                send(client_fd, response_tmpl, strlen(response_tmpl), 0);
                close(client_fd);
                
                if (service == 1) { // Google Drive
                    sqlite3 *db = NULL;
                    if (sqlite3_open(DB_PATH, &db) == SQLITE_OK) {
                        sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS sync_tokens (id INTEGER PRIMARY KEY CHECK (id = 1), client_id TEXT NOT NULL, client_secret TEXT NOT NULL, access_token TEXT, refresh_token TEXT, expiry_time INTEGER DEFAULT 0);", NULL, NULL, NULL);
                        sqlite3_stmt *stmt = NULL;
                        if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO sync_tokens (id, client_id, client_secret) VALUES (1, ?, '');", -1, &stmt, NULL) == SQLITE_OK) {
                            sqlite3_bind_text(stmt, 1, google_client_id(), -1, SQLITE_TRANSIENT);
                            sqlite3_step(stmt);
                            sqlite3_finalize(stmt);
                        }
                        sqlite3_close(db);
                    }
                    zig_sync_submit_code(code_start);
                } else if (service == 2) { // Dropbox
                    sqlite3 *db = NULL;
                    if (sqlite3_open(DB_PATH, &db) == SQLITE_OK) {
                        sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS dropbox_sync_tokens (id INTEGER PRIMARY KEY CHECK (id = 1), client_id TEXT NOT NULL, client_secret TEXT NOT NULL, access_token TEXT, refresh_token TEXT, expiry_time INTEGER DEFAULT 0);", NULL, NULL, NULL);
                        sqlite3_stmt *stmt = NULL;
                        if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO dropbox_sync_tokens (id, client_id, client_secret) VALUES (1, ?, '');", -1, &stmt, NULL) == SQLITE_OK) {
                            sqlite3_bind_text(stmt, 1, dropbox_app_key(), -1, SQLITE_TRANSIENT);
                            sqlite3_step(stmt);
                            sqlite3_finalize(stmt);
                        }
                        sqlite3_close(db);
                    }
                    zig_dropbox_submit_code(code_start);
                }
            } else {
                close(client_fd);
            }
        } else {
            close(client_fd);
        }
    } else {
        if (service == 1) {
            gui_update_sync_status(0, "Connection timed out");
        } else {
            gui_update_dropbox_status(0, "Connection timed out");
        }
    }
    
    close(server_fd);
    g_free(ld);
    return NULL;
}

static void start_loopback_listener(AppGui *gui, int service_type, int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        if (service_type == 1) gui_update_sync_status(0, "Failed to create socket");
        else gui_update_dropbox_status(0, "Failed to create socket");
        return;
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(server_fd);
        if (service_type == 1) gui_update_sync_status(0, "Port 12345 busy");
        else gui_update_dropbox_status(0, "Port 5173 busy");
        return;
    }
    
    if (listen(server_fd, 1) < 0) {
        close(server_fd);
        if (service_type == 1) gui_update_sync_status(0, "Listen failed");
        else gui_update_dropbox_status(0, "Listen failed");
        return;
    }
    
    LoopbackThreadData *ld = g_new0(LoopbackThreadData, 1);
    ld->gui = gui;
    ld->service_type = service_type;
    ld->port = port;
    ld->server_fd = server_fd;
    
    g_thread_new("loopback_listener", loopback_thread_func, ld);
}

/* ──────────────────────────────────────────────────────────
 * EXPORTED SIGNALS / CALLBACKS FOR GUI
 * ────────────────────────────────────────────────────────── */

void on_sync_connect_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppGui *gui = (AppGui *)user_data;
    int connected = zig_sync_check_status();
    if (connected) {
        zig_sync_disconnect();
    } else {
        if (!provider_id_configured(google_client_id(), "YOUR_PUBLIC_GOOGLE_ID")) {
            gui_update_sync_status(0, "Set QIRTAS_GOOGLE_CLIENT_ID");
            return;
        }
        gui_update_sync_status(2, "Waiting for browser...");
        gchar *auth_url = g_strdup_printf(
            "https://accounts.google.com/o/oauth2/v2/auth"
            "?client_id=%s"
            "&redirect_uri=http://localhost:12345"
            "&response_type=code"
            "&scope=https://www.googleapis.com/auth/drive.appdata"
            "&access_type=offline"
            "&prompt=consent", google_client_id());
        gtk_show_uri(NULL, auth_url, 0);
        g_free(auth_url);
        start_loopback_listener(gui, 1, 12345);
    }
}

void on_dropbox_connect_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppGui *gui = (AppGui *)user_data;
    int connected = zig_dropbox_check_status();
    if (connected) {
        zig_dropbox_disconnect();
    } else {
        if (!provider_id_configured(dropbox_app_key(), "YOUR_PUBLIC_DROPBOX_KEY")) {
            gui_update_dropbox_status(0, "Set QIRTAS_DROPBOX_APP_KEY");
            return;
        }
        gui_update_dropbox_status(2, "Waiting for browser...");
        gchar *auth_url = g_strdup_printf(
            "https://www.dropbox.com/oauth2/authorize"
            "?client_id=%s"
            "&token_access_type=offline"
            "&response_type=code"
            "&redirect_uri=http://localhost:5173", dropbox_app_key());
        gtk_show_uri(NULL, auth_url, 0);
        g_free(auth_url);
        start_loopback_listener(gui, 2, 5173);
    }
}

void on_github_connect_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppGui *gui = (AppGui *)user_data;
    int connected = zig_github_check_status();
    if (connected) {
        zig_github_disconnect();
    } else {
        start_github_device_flow(gui);
    }
}

void on_sync_now_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    zig_sync_now();
}

void on_dropbox_now_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    zig_dropbox_now();
}

void on_github_now_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    zig_github_now();
}

void on_local_sync_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    zig_local_sync_now();
}

/* Unused manual setup callbacks left as no-ops */
void load_sync_credentials(AppGui *gui) {
    (void)gui;
}

void on_save_credentials_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
}

void on_sync_submit_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
}

void on_dropbox_save_credentials_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
}

void on_dropbox_submit_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
}

void on_github_save_credentials_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
}
