const std = @import("std");
const Io = std.Io;
const sync = @import("sync.zig");

const DB_PATH = "/home/.config/lawh/vault.db";

comptime {
    std.testing.refAllDecls(sync);
}

// Import POSIX/libc headers
const c = @cImport({
    @cInclude("sys/inotify.h");
    @cInclude("unistd.h");
    @cInclude("sys/mman.h");
    @cInclude("sys/stat.h");
    @cInclude("fcntl.h");
    @cInclude("sqlite3.h");
    @cInclude("dirent.h");
});

extern fn run_gui(argc: c_int, argv: ?[*]? [*]const u8) c_int;
extern fn gui_get_text() ?[*]u8;
extern fn gui_free_text(text: [*]u8) void;
extern fn gui_set_text(text: [*]const u8, len: c_int) void;
extern fn gui_set_title(title: [*:0]const u8) void;
extern fn gui_set_sync_status(status: [*:0]const u8) void;
extern fn gui_show_editor() void;
extern fn gui_get_cursor_position(line: *c_int, col: *c_int) void;
extern fn gui_set_cursor_position(line: c_int, col: c_int) void;
extern fn gui_refresh_explorer() void;
extern fn gui_index_all_files() void;
extern fn gui_index_file(filename: [*:0]const u8) void;
extern fn gui_remove_file_from_index(filename: [*:0]const u8) void;
extern fn gui_init_virtual_document(total_lines: c_int, start_line: c_int, end_line: c_int) void;
extern fn gui_trigger_autosave() void;
extern fn gui_get_active_page_bounds(start_line: *c_int, end_line: *c_int, total_lines: *c_int) void;
extern fn gui_update_total_virtual_lines(total_lines: c_int) void;
extern fn gui_tabs_save_active_to_cache() void;
extern fn gui_tabs_restore_active_from_cache() void;

const GuiIdleCallback = *const fn (user_data: ?*anyopaque) callconv(.c) void;
extern fn gui_run_on_main_thread(callback: GuiIdleCallback, user_data: ?*anyopaque) void;

// Active file path
var active_file_path: [256]u8 = undefined;
var active_file_path_len: usize = 0;

// Active file mapping variables
var active_mmap_ptr: ?[*]const u8 = null;
var active_mmap_size: usize = 0;
var line_offsets: std.ArrayList(usize) = .empty;

// Global reference to the IO instance
pub var global_io: Io = undefined;

// Dynamic inotify watch descriptors
var global_inotify_fd: c_int = -1;
var global_wd = std.atomic.Value(c_int).init(-1);
var directory_wd = std.atomic.Value(c_int).init(-1);

// Initial cursor coordinates
var initial_cursor_line: c_int = 1;
var initial_cursor_col: c_int = 0;

pub fn main(init: std.process.Init) !void {
    // Ensure the config directory exists
    _ = c.mkdir("/home", 0o755);
    _ = c.mkdir("/home/.config", 0o755);
    _ = c.mkdir("/home/.config/lawh", 0o755);

    global_io = init.io;
    line_offsets = .empty;

    const gpa = std.heap.page_allocator;

    var workspace_path: []const u8 = ".";
    var saved_vault: ?[]const u8 = null;
    if (init.minimal.args.vector.len > 1) {
        workspace_path = std.mem.span(init.minimal.args.vector[1]);
    } else {
        var db: ?*c.sqlite3 = null;
        if (c.sqlite3_open(DB_PATH, &db) == c.SQLITE_OK) {
            defer _ = c.sqlite3_close(db);
            _ = c.sqlite3_busy_timeout(db, 5000);
            
            // Ensure table and column exist
            _ = c.sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS session_state (id INTEGER PRIMARY KEY CHECK (id = 1), active_file TEXT, cursor_line INTEGER, cursor_col INTEGER, vault_path TEXT);", null, null, null);
            _ = c.sqlite3_exec(db, "ALTER TABLE session_state ADD COLUMN vault_path TEXT;", null, null, null);
            _ = c.sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS system_keys (id INTEGER PRIMARY KEY CHECK (id = 1), encrypted_master_key_machine TEXT NOT NULL, encrypted_master_key_passphrase TEXT, passphrase_salt TEXT);", null, null, null);

            var stmt: ?*c.sqlite3_stmt = null;
            const query_sql = "SELECT vault_path FROM session_state WHERE id = 1;";
            if (c.sqlite3_prepare_v2(db, query_sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
                defer _ = c.sqlite3_finalize(stmt);
                if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
                    const vault_text = c.sqlite3_column_text(stmt, 0);
                    if (vault_text != null) {
                        const vault_span = std.mem.span(vault_text);
                        if (vault_span.len > 0) {
                            saved_vault = gpa.dupe(u8, vault_span) catch null;
                        }
                    }
                }
            }
        }
    }

    if (saved_vault) |sv| {
        workspace_path = sv;
    }
    defer if (saved_vault) |sv| gpa.free(sv);

    // Change working directory to the target workspace
    const workspace_path_z = try gpa.dupeZ(u8, workspace_path);
    defer gpa.free(workspace_path_z);
    if (c.chdir(workspace_path_z.ptr) != 0) {
        _ = c.chdir(".");
    }

    // Default to Economics_Notes.md on launch
    const default_file = "Economics_Notes.md";
    @memcpy(active_file_path[0..default_file.len], default_file);
    active_file_path_len = default_file.len;

    // Launch the GTK C GUI event loop
    const status = run_gui(0, null);
    std.debug.print("GTK Application exited with status: {}\n", .{status});
}

fn is_indexable_file(filename: []const u8) bool {
    return std.mem.endsWith(u8, filename, ".md") or 
           std.mem.endsWith(u8, filename, ".txt") or 
           std.mem.endsWith(u8, filename, ".zig") or 
           std.mem.endsWith(u8, filename, ".zon") or
           std.mem.endsWith(u8, filename, ".c") or
           std.mem.endsWith(u8, filename, ".h");
}

fn find_first_indexable_file_posix(out_buf: []u8) bool {
    const dir = c.opendir(".") orelse return false;
    defer _ = c.closedir(dir);
    
    while (true) {
        const entry = c.readdir(dir) orelse break;
        const name_ptr = @as([*:0]const u8, @ptrCast(&entry.*.d_name));
        const name = std.mem.span(name_ptr);
        if (name.len == 0 or name[0] == '.') continue;
        if (is_indexable_file(name)) {
            if (name.len < out_buf.len) {
                @memcpy(out_buf[0..name.len], name);
                out_buf[name.len] = 0;
                return true;
            }
        }
    }
    return false;
}

// Exported function called by the C GUI layer when the interface is active
pub export fn zig_on_gui_ready() callconv(.c) void {
    // Load saved session if exists
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(DB_PATH, &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
    } else {
        defer _ = c.sqlite3_close(db);
        _ = c.sqlite3_busy_timeout(db, 5000);
        // Load saved session if exists
        var stmt: ?*c.sqlite3_stmt = null;
        const query_sql = "SELECT active_file, cursor_line, cursor_col FROM session_state WHERE id = 1;";
        if (c.sqlite3_prepare_v2(db, query_sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
            defer _ = c.sqlite3_finalize(stmt);
            if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
                const file_text = c.sqlite3_column_text(stmt, 0);
                const line = c.sqlite3_column_int(stmt, 1);
                const col = c.sqlite3_column_int(stmt, 2);
                
                if (file_text != null) {
                    const file_span = std.mem.span(file_text);
                    if (file_span.len > 0) {
                        @memcpy(active_file_path[0..file_span.len], file_span);
                        active_file_path_len = file_span.len;
                        initial_cursor_line = line;
                        initial_cursor_col = col;
                    }
                }
            }
        }
    }

    // Run initial scan and index all files in background/boot-time via C FTS5 engine
    gui_index_all_files();

    const gpa = std.heap.page_allocator;

    // Register initial watch in inotify
    global_inotify_fd = c.inotify_init();
    if (global_inotify_fd >= 0) {
        register_file_watch(active_file_path[0..active_file_path_len]);
        // Watch current directory for explorer grid updates
        directory_wd.store(c.inotify_add_watch(global_inotify_fd, ".", c.IN_CREATE | c.IN_DELETE | c.IN_MOVED_TO | c.IN_MOVED_FROM | c.IN_MODIFY), .monotonic);
    }

    // Load initial notes (either restored from db or default or first file found)
    var file_to_load: []const u8 = active_file_path[0..active_file_path_len];
    var path_exists = true;
    const file_to_load_z = gpa.dupeZ(u8, file_to_load) catch file_to_load;
    defer if (file_to_load_z.ptr != file_to_load.ptr) gpa.free(file_to_load_z);
    Io.Dir.cwd().access(global_io, file_to_load_z, .{}) catch {
        path_exists = false;
    };

    if (!path_exists) {
        var file_buf: [256]u8 = undefined;
        if (find_first_indexable_file_posix(&file_buf)) {
            const tf = std.mem.span(@as([*:0]const u8, @ptrCast(&file_buf)));
            @memcpy(active_file_path[0..tf.len], tf);
            active_file_path_len = tf.len;
            file_to_load = active_file_path[0..active_file_path_len];
        }
    }

    load_file_and_update_gui(file_to_load) catch |err| {
        std.debug.print("Failed to load active notes: {}\n", .{err});
    };

    // Restore cursor position
    gui_set_cursor_position(initial_cursor_line, initial_cursor_col);

    // Spawn Asynchronous Autosave Thread
    // _ = std.Thread.spawn(.{}, autosave_thread_loop, .{}) catch |err| {
    //     std.debug.print("Failed to spawn autosave thread: {}\n", .{err});
    // };

    // Spawn File System Watcher Thread
    _ = std.Thread.spawn(.{}, file_watcher_thread_loop, .{}) catch |err| {
        std.debug.print("Failed to spawn file watcher thread: {}\n", .{err});
    };
}

pub export fn zig_open_file(filename_ptr: [*:0]const u8) callconv(.c) void {
    gui_tabs_save_active_to_cache();

    const filename = std.mem.span(filename_ptr);

    // Save previous cursor position first
    var old_line: c_int = 1;
    var old_col: c_int = 0;
    gui_get_cursor_position(&old_line, &old_col);
    if (active_file_path_len > 0 and !std.mem.eql(u8, active_file_path[0..active_file_path_len], "Untitled")) {
        save_session(active_file_path[0..active_file_path_len], old_line, old_col) catch {};
    }

    if (std.mem.eql(u8, filename, "Untitled")) {
        // Unload existing mapping
        if (active_mmap_ptr) |ptr| {
            unload_file_mmap(ptr, active_mmap_size);
            active_mmap_ptr = null;
            active_mmap_size = 0;
        }

        // Clear line offsets
        line_offsets.clearRetainingCapacity();
        const gpa = std.heap.page_allocator;
        line_offsets.append(gpa, 0) catch {};

        @memcpy(active_file_path[0..filename.len], filename);
        active_file_path_len = filename.len;

        gui_set_text("", 0);
        gui_init_virtual_document(1, 0, 1);
        gui_set_title("Untitled - Qirtas");
        gui_set_sync_status("Not Synced");
        gui_show_editor();
        gui_tabs_restore_active_from_cache();
        return;
    }

    // Filter editable extensions
    if (std.mem.endsWith(u8, filename, ".md") or 
        std.mem.endsWith(u8, filename, ".txt") or 
        std.mem.endsWith(u8, filename, ".zig") or 
        std.mem.endsWith(u8, filename, ".zon") or
        std.mem.endsWith(u8, filename, ".c") or
        std.mem.endsWith(u8, filename, ".h")) 
    {
        var display_name: []const u8 = filename;
        const is_absolute = filename.len > 0 and filename[0] == '/';
        if (is_absolute) {
            var cwd_path_buf: [4096]u8 = undefined;
            const cwd_ptr = c.getcwd(&cwd_path_buf, cwd_path_buf.len);
            if (cwd_ptr != null) {
                const cwd_path = std.mem.span(cwd_ptr);
                if (std.mem.startsWith(u8, filename, cwd_path)) {
                    if (filename.len > cwd_path.len + 1) {
                        display_name = filename[cwd_path.len + 1..];
                    } else {
                        display_name = filename;
                    }
                } else {
                    display_name = get_basename(filename);
                }
            } else {
                display_name = get_basename(filename);
            }
        }

        @memcpy(active_file_path[0..display_name.len], display_name);
        active_file_path_len = display_name.len;

        // Register the file watch descriptor dynamically
        register_file_watch(display_name);

        // Load the new file contents
        load_file_and_update_gui(display_name) catch |err| {
            std.debug.print("Failed to open file {s}: {}\n", .{display_name, err});
        };
        gui_tabs_restore_active_from_cache();
    }
}

pub export fn zig_set_cursor_trail(enabled: c_int) callconv(.c) void {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(DB_PATH, &db) == c.SQLITE_OK) {
        defer _ = c.sqlite3_close(db);
        _ = c.sqlite3_busy_timeout(db, 5000);
        _ = c.sqlite3_exec(db, "INSERT OR IGNORE INTO session_state (id) VALUES (1);", null, null, null);
        const sql = "UPDATE session_state SET enable_cursor_trail = ? WHERE id = 1;";
        var stmt: ?*c.sqlite3_stmt = null;
        if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
            defer _ = c.sqlite3_finalize(stmt);
            _ = c.sqlite3_bind_int(stmt, 1, enabled);
            _ = c.sqlite3_step(stmt);
        }
    }
}

pub export fn zig_get_cursor_trail() callconv(.c) c_int {
    var db: ?*c.sqlite3 = null;
    var enabled: c_int = 1; // default enabled
    if (c.sqlite3_open(DB_PATH, &db) == c.SQLITE_OK) {
        defer _ = c.sqlite3_close(db);
        _ = c.sqlite3_busy_timeout(db, 5000);
        _ = c.sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS session_state (id INTEGER PRIMARY KEY CHECK (id = 1), active_file TEXT, cursor_line INTEGER, cursor_col INTEGER, vault_path TEXT, enable_cursor_trail INTEGER DEFAULT 1);", null, null, null);
        _ = c.sqlite3_exec(db, "ALTER TABLE session_state ADD COLUMN enable_cursor_trail INTEGER DEFAULT 1;", null, null, null);

        var stmt: ?*c.sqlite3_stmt = null;
        const sql = "SELECT enable_cursor_trail FROM session_state WHERE id = 1;";
        if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
            defer _ = c.sqlite3_finalize(stmt);
            if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
                if (c.sqlite3_column_type(stmt, 0) != c.SQLITE_NULL) {
                    enabled = c.sqlite3_column_int(stmt, 0);
                }
            }
        }
    }
    return enabled;
}

pub export fn zig_set_layout_dividers(enabled: c_int) callconv(.c) void {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(DB_PATH, &db) == c.SQLITE_OK) {
        defer _ = c.sqlite3_close(db);
        _ = c.sqlite3_busy_timeout(db, 5000);
        _ = c.sqlite3_exec(db, "INSERT OR IGNORE INTO session_state (id) VALUES (1);", null, null, null);
        const sql = "UPDATE session_state SET enable_layout_dividers = ? WHERE id = 1;";
        var stmt: ?*c.sqlite3_stmt = null;
        if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
            defer _ = c.sqlite3_finalize(stmt);
            _ = c.sqlite3_bind_int(stmt, 1, enabled);
            _ = c.sqlite3_step(stmt);
        }
    }
}

pub export fn zig_get_layout_dividers() callconv(.c) c_int {
    var db: ?*c.sqlite3 = null;
    var enabled: c_int = 0; // default disabled
    if (c.sqlite3_open(DB_PATH, &db) == c.SQLITE_OK) {
        defer _ = c.sqlite3_close(db);
        _ = c.sqlite3_busy_timeout(db, 5000);
        _ = c.sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS session_state (id INTEGER PRIMARY KEY CHECK (id = 1), active_file TEXT, cursor_line INTEGER, cursor_col INTEGER, vault_path TEXT, enable_cursor_trail INTEGER DEFAULT 1, enable_layout_dividers INTEGER DEFAULT 0);", null, null, null);
        _ = c.sqlite3_exec(db, "ALTER TABLE session_state ADD COLUMN enable_layout_dividers INTEGER DEFAULT 0;", null, null, null);

        var stmt: ?*c.sqlite3_stmt = null;
        const sql = "SELECT enable_layout_dividers FROM session_state WHERE id = 1;";
        if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
            defer _ = c.sqlite3_finalize(stmt);
            if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
                if (c.sqlite3_column_type(stmt, 0) != c.SQLITE_NULL) {
                    enabled = c.sqlite3_column_int(stmt, 0);
                }
            }
        }
    }
    return enabled;
}

pub export fn zig_set_bottom_margin(enabled: c_int) callconv(.c) void {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(DB_PATH, &db) == c.SQLITE_OK) {
        defer _ = c.sqlite3_close(db);
        _ = c.sqlite3_busy_timeout(db, 5000);
        _ = c.sqlite3_exec(db, "INSERT OR IGNORE INTO session_state (id) VALUES (1);", null, null, null);
        const sql = "UPDATE session_state SET enable_bottom_margin = ? WHERE id = 1;";
        var stmt: ?*c.sqlite3_stmt = null;
        if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
            defer _ = c.sqlite3_finalize(stmt);
            _ = c.sqlite3_bind_int(stmt, 1, enabled);
            _ = c.sqlite3_step(stmt);
        }
    }
}

pub export fn zig_get_bottom_margin() callconv(.c) c_int {
    var db: ?*c.sqlite3 = null;
    var enabled: c_int = 0; // default disabled
    if (c.sqlite3_open(DB_PATH, &db) == c.SQLITE_OK) {
        defer _ = c.sqlite3_close(db);
        _ = c.sqlite3_busy_timeout(db, 5000);
        _ = c.sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS session_state (id INTEGER PRIMARY KEY CHECK (id = 1), active_file TEXT, cursor_line INTEGER, cursor_col INTEGER, vault_path TEXT, enable_cursor_trail INTEGER DEFAULT 1, enable_layout_dividers INTEGER DEFAULT 0, enable_bottom_margin INTEGER DEFAULT 0);", null, null, null);
        _ = c.sqlite3_exec(db, "ALTER TABLE session_state ADD COLUMN enable_bottom_margin INTEGER DEFAULT 0;", null, null, null);

        var stmt: ?*c.sqlite3_stmt = null;
        const sql = "SELECT enable_bottom_margin FROM session_state WHERE id = 1;";
        if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
            defer _ = c.sqlite3_finalize(stmt);
            if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
                if (c.sqlite3_column_type(stmt, 0) != c.SQLITE_NULL) {
                    enabled = c.sqlite3_column_int(stmt, 0);
                }
            }
        }
    }
    return enabled;
}

pub export fn zig_set_focus_mode(enabled: c_int) callconv(.c) void {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(DB_PATH, &db) == c.SQLITE_OK) {
        defer _ = c.sqlite3_close(db);
        _ = c.sqlite3_busy_timeout(db, 5000);
        _ = c.sqlite3_exec(db, "INSERT OR IGNORE INTO session_state (id) VALUES (1);", null, null, null);
        const sql = "UPDATE session_state SET enable_focus_mode = ? WHERE id = 1;";
        var stmt: ?*c.sqlite3_stmt = null;
        if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
            defer _ = c.sqlite3_finalize(stmt);
            _ = c.sqlite3_bind_int(stmt, 1, enabled);
            _ = c.sqlite3_step(stmt);
        }
    }
}

pub export fn zig_get_focus_mode() callconv(.c) c_int {
    var db: ?*c.sqlite3 = null;
    var enabled: c_int = 0; // default disabled
    if (c.sqlite3_open(DB_PATH, &db) == c.SQLITE_OK) {
        defer _ = c.sqlite3_close(db);
        _ = c.sqlite3_busy_timeout(db, 5000);
        _ = c.sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS session_state (id INTEGER PRIMARY KEY CHECK (id = 1), active_file TEXT, cursor_line INTEGER, cursor_col INTEGER, vault_path TEXT, enable_cursor_trail INTEGER DEFAULT 1, enable_layout_dividers INTEGER DEFAULT 0, enable_bottom_margin INTEGER DEFAULT 0, enable_focus_mode INTEGER DEFAULT 0, enable_editor_border INTEGER DEFAULT 1);", null, null, null);
        _ = c.sqlite3_exec(db, "ALTER TABLE session_state ADD COLUMN enable_focus_mode INTEGER DEFAULT 0;", null, null, null);
        _ = c.sqlite3_exec(db, "ALTER TABLE session_state ADD COLUMN enable_editor_border INTEGER DEFAULT 1;", null, null, null);

        var stmt: ?*c.sqlite3_stmt = null;
        const sql = "SELECT enable_focus_mode FROM session_state WHERE id = 1;";
        if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
            defer _ = c.sqlite3_finalize(stmt);
            if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
                if (c.sqlite3_column_type(stmt, 0) != c.SQLITE_NULL) {
                    enabled = c.sqlite3_column_int(stmt, 0);
                }
            }
        }
    }
    return enabled;
}

pub export fn zig_set_editor_border(enabled: c_int) callconv(.c) void {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(DB_PATH, &db) == c.SQLITE_OK) {
        defer _ = c.sqlite3_close(db);
        _ = c.sqlite3_busy_timeout(db, 5000);
        _ = c.sqlite3_exec(db, "INSERT OR IGNORE INTO session_state (id) VALUES (1);", null, null, null);
        const sql = "UPDATE session_state SET enable_editor_border = ? WHERE id = 1;";
        var stmt: ?*c.sqlite3_stmt = null;
        if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
            defer _ = c.sqlite3_finalize(stmt);
            _ = c.sqlite3_bind_int(stmt, 1, enabled);
            _ = c.sqlite3_step(stmt);
        }
    }
}

pub export fn zig_get_editor_border() callconv(.c) c_int {
    var db: ?*c.sqlite3 = null;
    var enabled: c_int = 1; // default enabled
    if (c.sqlite3_open(DB_PATH, &db) == c.SQLITE_OK) {
        defer _ = c.sqlite3_close(db);
        _ = c.sqlite3_busy_timeout(db, 5000);
        _ = c.sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS session_state (id INTEGER PRIMARY KEY CHECK (id = 1), active_file TEXT, cursor_line INTEGER, cursor_col INTEGER, vault_path TEXT, enable_cursor_trail INTEGER DEFAULT 1, enable_layout_dividers INTEGER DEFAULT 0, enable_bottom_margin INTEGER DEFAULT 0, enable_focus_mode INTEGER DEFAULT 0, enable_editor_border INTEGER DEFAULT 1);", null, null, null);
        _ = c.sqlite3_exec(db, "ALTER TABLE session_state ADD COLUMN enable_editor_border INTEGER DEFAULT 1;", null, null, null);

        var stmt: ?*c.sqlite3_stmt = null;
        const sql = "SELECT enable_editor_border FROM session_state WHERE id = 1;";
        if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
            defer _ = c.sqlite3_finalize(stmt);
            if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
                if (c.sqlite3_column_type(stmt, 0) != c.SQLITE_NULL) {
                    enabled = c.sqlite3_column_int(stmt, 0);
                }
            }
        }
    }
    return enabled;
}

pub export fn zig_open_vault(dir_path_ptr: [*:0]const u8) callconv(.c) void {
    const dir_path = std.mem.span(dir_path_ptr);
    const gpa = std.heap.page_allocator;

    // 1. Change working directory
    const dir_path_z = gpa.dupeZ(u8, dir_path) catch return;
    defer gpa.free(dir_path_z);
    if (c.chdir(dir_path_z.ptr) != 0) {
        std.debug.print("Failed to chdir to {s}\n", .{dir_path});
        return;
    }

    // 2. Clear database index tables for the old vault
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(DB_PATH, &db) == c.SQLITE_OK) {
        defer _ = c.sqlite3_close(db);
        _ = c.sqlite3_busy_timeout(db, 5000);
        _ = c.sqlite3_exec(db, "DELETE FROM file_metadata;", null, null, null);
        _ = c.sqlite3_exec(db, "DELETE FROM note_search;", null, null, null);
        _ = c.sqlite3_exec(db, "DELETE FROM file_content_fts;", null, null, null);
        
        // Save the new vault path
        _ = c.sqlite3_exec(db, "INSERT OR IGNORE INTO session_state (id) VALUES (1);", null, null, null);
        const save_sql = "UPDATE session_state SET vault_path = ?, active_file = NULL, cursor_line = 1, cursor_col = 0 WHERE id = 1;";
        var stmt: ?*c.sqlite3_stmt = null;
        if (c.sqlite3_prepare_v2(db, save_sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
            defer _ = c.sqlite3_finalize(stmt);
            _ = c.sqlite3_bind_text(stmt, 1, dir_path_z.ptr, -1, null);
            _ = c.sqlite3_step(stmt);
        }
    }

    // 3. Update the directory watcher for inotify
    if (global_inotify_fd >= 0) {
        const old_dir_wd = directory_wd.load(.monotonic);
        if (old_dir_wd >= 0) {
            _ = c.inotify_rm_watch(global_inotify_fd, old_dir_wd);
        }
        directory_wd.store(c.inotify_add_watch(global_inotify_fd, ".", c.IN_CREATE | c.IN_DELETE | c.IN_MOVED_TO | c.IN_MOVED_FROM | c.IN_MODIFY), .monotonic);
    }

    // 4. Find the first indexable file in the new vault, or create a default one
    var file_buf: [256]u8 = undefined;
    const file_to_open = if (find_first_indexable_file_posix(&file_buf))
        std.mem.span(@as([*:0]const u8, @ptrCast(&file_buf)))
    else
        "Welcome.md";

    @memcpy(active_file_path[0..file_to_open.len], file_to_open);
    active_file_path_len = file_to_open.len;

    // 5. Update UI title, refresh explorer, re-index
    gui_index_all_files();
    gui_refresh_explorer();

    // 6. Register file watch on the active file
    register_file_watch(file_to_open);

    // 7. Load file contents
    load_file_and_update_gui(file_to_open) catch |err| {
        std.debug.print("Failed to open file {s}: {}\n", .{file_to_open, err});
    };

    // 8. Reset cursor
    gui_set_cursor_position(1, 0);
}


// Exported FFI: Create new file dynamically
pub export fn zig_create_new_file(name_ptr: [*:0]const u8) callconv(.c) void {
    const raw_name = std.mem.span(name_ptr);
    const gpa = std.heap.page_allocator;
    
    var has_ext = false;
    if (std.mem.lastIndexOfScalar(u8, raw_name, '.')) |dot_idx| {
        if (dot_idx > 0 and dot_idx < raw_name.len - 1) {
            has_ext = true;
        }
    }
    
    var final_name: []const u8 = undefined;
    if (has_ext) {
        final_name = gpa.dupe(u8, raw_name) catch return;
    } else {
        final_name = std.fmt.allocPrint(gpa, "{s}.md", .{raw_name}) catch return;
    }
    defer gpa.free(final_name);

    const final_name_z = gpa.dupeZ(u8, final_name) catch return;
    defer gpa.free(final_name_z);
    
    var file = Io.Dir.cwd().createFile(global_io, final_name_z, .{}) catch |err| {
        std.debug.print("Failed to create file {s}: {}\n", .{final_name_z, err});
        return;
    };
    file.close(global_io);

    gui_refresh_explorer();
    zig_open_file(final_name_z.ptr);
}

// Exported FFI: Resolve and open wiki-link
pub export fn zig_open_wiki_link(note_name_ptr: [*:0]const u8) callconv(.c) void {
    const note_name = std.mem.span(note_name_ptr);
    const gpa = std.heap.page_allocator;
    
    const temp = std.fmt.allocPrint(gpa, "{s}.md", .{note_name}) catch return;
    defer gpa.free(temp);
    const filename_z = gpa.dupeZ(u8, temp) catch return;
    defer gpa.free(filename_z);
    
    var file_exists = true;
    Io.Dir.cwd().access(global_io, filename_z, .{}) catch {
        file_exists = false;
    };
    
    if (!file_exists) {
        var file = Io.Dir.cwd().createFile(global_io, filename_z, .{}) catch |err| {
            std.debug.print("Failed to auto-generate note {s}: {}\n", .{filename_z, err});
            return;
        };
        file.close(global_io);
        gui_refresh_explorer();
    }
    
    zig_open_file(filename_z.ptr);
}

// Exported FFI: Save cursor and state on app shutdown
pub export fn zig_on_shutdown() callconv(.c) void {
    var line: c_int = 1;
    var col: c_int = 0;
    gui_get_cursor_position(&line, &col);
    if (active_file_path_len > 0 and !std.mem.eql(u8, active_file_path[0..active_file_path_len], "Untitled")) {
        save_session(active_file_path[0..active_file_path_len], line, col) catch {};
    }
    if (active_mmap_ptr) |ptr| {
        unload_file_mmap(ptr, active_mmap_size);
        active_mmap_ptr = null;
        active_mmap_size = 0;
    }
    line_offsets.deinit(std.heap.page_allocator);
}

// Exported FFI: Force an immediate save from the editor (Ctrl+S)
pub export fn zig_force_save() callconv(.c) void {
    gui_trigger_autosave();
}

fn register_file_watch(filename: []const u8) void {
    if (global_inotify_fd < 0) return;
    
    // Remove old watch if it exists
    const old_wd = global_wd.load(.monotonic);
    if (old_wd >= 0) {
        _ = c.inotify_rm_watch(global_inotify_fd, old_wd);
        global_wd.store(-1, .monotonic);
    }

    const gpa = std.heap.page_allocator;
    const sentinel_path = gpa.dupeZ(u8, filename) catch return;
    defer gpa.free(sentinel_path);

    global_wd.store(c.inotify_add_watch(global_inotify_fd, sentinel_path.ptr, c.IN_MODIFY), .monotonic);
}

fn load_file_mmap(filename: [*:0]const u8, size: *usize) ![*]const u8 {
    const fd = c.open(filename, c.O_RDONLY);
    if (fd < 0) return error.OpenFileFailed;
    defer _ = c.close(fd);

    var st: c.struct_stat = undefined;
    if (c.fstat(fd, &st) < 0) return error.StatFileFailed;

    const file_size = @as(usize, @intCast(st.st_size));
    size.* = file_size;
    if (file_size == 0) {
        return "";
    }

    const ptr = c.mmap(null, file_size, c.PROT_READ, c.MAP_SHARED, fd, 0);
    if (ptr == c.MAP_FAILED) return error.MmapFailed;

    return @ptrCast(ptr);
}

fn unload_file_mmap(ptr: [*]const u8, size: usize) void {
    if (size == 0) return;
    _ = c.munmap(@constCast(@ptrCast(ptr)), size);
}

fn populate_line_offsets(content: []const u8) !void {
    const gpa = std.heap.page_allocator;
    line_offsets.clearRetainingCapacity();
    
    // The first line starts at offset 0
    try line_offsets.append(gpa, 0);
    
    var i: usize = 0;
    while (i < content.len) : (i += 1) {
        if (content[i] == '\n') {
            try line_offsets.append(gpa, i + 1);
        }
    }
}

fn remap_active_file() !void {
    const gpa = std.heap.page_allocator;
    const path = active_file_path[0..active_file_path_len];
    const path_z = try gpa.dupeZ(u8, path);
    defer gpa.free(path_z);

    // Unload existing mapping
    if (active_mmap_ptr) |ptr| {
        unload_file_mmap(ptr, active_mmap_size);
        active_mmap_ptr = null;
        active_mmap_size = 0;
    }

    var size: usize = 0;
    const ptr = try load_file_mmap(path_z.ptr, &size);
    active_mmap_ptr = ptr;
    active_mmap_size = size;

    const content = if (size > 0) ptr[0..size] else "";
    try populate_line_offsets(content);
    gui_update_total_virtual_lines(@as(c_int, @intCast(line_offsets.items.len)));
}

fn load_file_and_update_gui(filename: []const u8) !void {
    const gpa = std.heap.page_allocator;
    const filename_z = try gpa.dupeZ(u8, filename);
    defer gpa.free(filename_z);

    var file_exists = true;
    Io.Dir.cwd().access(global_io, filename_z, .{}) catch {
        file_exists = false;
    };
    if (!file_exists) {
        var f = try Io.Dir.cwd().createFile(global_io, filename, .{});
        defer f.close(global_io);
        try f.writeStreamingAll(global_io, "# New Notebook Document\n\n- Start writing here...\n");
    }

    try remap_active_file();

    const total_lines = @as(c_int, @intCast(line_offsets.items.len));
    const end_line = if (total_lines > 100) @as(c_int, 100) else total_lines;
    
    var page_len: c_int = 0;
    const page_text_ptr = zig_get_text_for_line_range(0, end_line, &page_len);
    
    gui_set_text(page_text_ptr.?, page_len);
    gui_init_virtual_document(total_lines, 0, end_line);
    
    // Update path headers dynamically
    var title_buf: [300]u8 = undefined;
    const title = try std.fmt.bufPrint(&title_buf, "{s} - Qirtas", .{filename});
    const title_z = try gpa.dupeZ(u8, title);
    defer gpa.free(title_z);

    gui_set_title(title_z.ptr);
    gui_set_sync_status("Synced");
    gui_show_editor();
}

// Background autosave scheduler (checks every 30s)
fn autosave_thread_loop() void {
    while (true) {
        _ = c.usleep(30 * 1000 * 1000); // 30 seconds
        gui_run_on_main_thread(&autosave_callback, null);
    }
}

fn autosave_callback(user_data: ?*anyopaque) callconv(.c) void {
    _ = user_data;
    gui_trigger_autosave();
}

pub export fn zig_get_text_for_line_range(start_line: c_int, end_line: c_int, out_len: *c_int) callconv(.c) ?[*]const u8 {
    const start_idx = @as(usize, @intCast(start_line));
    const end_idx = @as(usize, @intCast(end_line));
    
    if (start_idx >= line_offsets.items.len) {
        out_len.* = 0;
        return "";
    }
    
    const start_offset = line_offsets.items[start_idx];
    const end_offset = if (end_idx >= line_offsets.items.len) 
        active_mmap_size
    else 
        line_offsets.items[end_idx];
        
    if (active_mmap_ptr) |ptr| {
        const slice = ptr[start_offset..end_offset];
        out_len.* = @as(c_int, @intCast(slice.len));
        return slice.ptr;
    }
    out_len.* = 0;
    return "";
}

pub export fn zig_save_active_page(start_line: c_int, end_line: c_int, text: [*:0]const u8) callconv(.c) c_int {
    const gpa = std.heap.page_allocator;
    const start_idx = @as(usize, @intCast(start_line));
    const end_idx = @as(usize, @intCast(end_line));
    
    if (start_idx > line_offsets.items.len) return 1;
    
    const start_offset = line_offsets.items[start_idx];
    const end_offset = if (end_idx >= line_offsets.items.len) 
        active_mmap_size
    else 
        line_offsets.items[end_idx];
        
    const new_text_slice = std.mem.span(text);
    
    const prefix_len = start_offset;
    const suffix_len = active_mmap_size - end_offset;
    const new_size = prefix_len + new_text_slice.len + suffix_len;
    
    const new_content = gpa.alloc(u8, new_size) catch return 1;
    defer gpa.free(new_content);
    
    if (active_mmap_ptr) |ptr| {
        @memcpy(new_content[0..prefix_len], ptr[0..prefix_len]);
        @memcpy(new_content[prefix_len .. prefix_len + new_text_slice.len], new_text_slice);
        @memcpy(new_content[prefix_len + new_text_slice.len ..], ptr[end_offset..active_mmap_size]);
    } else {
        @memcpy(new_content[0..new_text_slice.len], new_text_slice);
    }
    
    const path = active_file_path[0..active_file_path_len];
    var file = Io.Dir.cwd().createFile(global_io, path, .{}) catch return 1;
    defer file.close(global_io);
    
    file.writeStreamingAll(global_io, new_content) catch return 1;
    
    remap_active_file() catch return 1;
    
    const path_z = gpa.dupeZ(u8, path) catch return 1;
    defer gpa.free(path_z);
    gui_index_file(path_z.ptr);
    
    return 0;
}

// Background file watcher loop using standard inotify
fn file_watcher_thread_loop() void {
    var buf: [1024]u8 align(@alignOf(c.struct_inotify_event)) = undefined;

    while (true) {
        if (global_inotify_fd < 0) {
            _ = c.usleep(500 * 1000);
            continue;
        }

        const len = c.read(global_inotify_fd, &buf, buf.len);
        if (len < 0) {
            _ = c.usleep(100 * 1000);
            continue;
        }
        
        var i: usize = 0;
        while (i < @as(usize, @intCast(len))) {
            const event = @as(*c.struct_inotify_event, @ptrCast(@alignCast(&buf[i])));
            
            const g_wd = global_wd.load(.monotonic);
            const d_wd = directory_wd.load(.monotonic);
            if (event.wd == g_wd) {
                gui_run_on_main_thread(&reload_file_callback, null);
            } else if (event.wd == d_wd) {
                if (event.len > 0) {
                    const event_ptr_bytes = @as([*]const u8, @ptrCast(event));
                    const name_ptr = @as([*:0]const u8, @ptrCast(event_ptr_bytes + @sizeOf(c.struct_inotify_event)));
                    const name = std.mem.span(name_ptr);
                    
                    if (is_indexable_file(name)) {
                        const gpa = std.heap.page_allocator;
                        const name_z = gpa.dupeZ(u8, name) catch continue;
                        defer gpa.free(name_z);
                        if ((event.mask & (c.IN_MODIFY | c.IN_CREATE | c.IN_MOVED_TO)) != 0) {
                            gui_index_file(name_z.ptr);
                        } else if ((event.mask & (c.IN_DELETE | c.IN_MOVED_FROM)) != 0) {
                            gui_remove_file_from_index(name_z.ptr);
                        }
                    }
                }
                gui_run_on_main_thread(&refresh_explorer_callback, null);
            }
            
            i += @sizeOf(c.struct_inotify_event) + event.len;
        }
    }
}

fn refresh_explorer_callback(user_data: ?*anyopaque) callconv(.c) void {
    _ = user_data;
    gui_refresh_explorer();
}

// GUI-Thread callback: reloads file only if content has changed (prevents self-reload loops)
fn reload_file_callback(user_data: ?*anyopaque) callconv(.c) void {
    _ = user_data;

    const local_path = active_file_path[0..active_file_path_len];
    const gpa = std.heap.page_allocator;
    const path_z = gpa.dupeZ(u8, local_path) catch return;
    defer gpa.free(path_z);

    var file_size: usize = 0;
    const ptr = load_file_mmap(path_z.ptr, &file_size) catch return;
    defer unload_file_mmap(ptr, file_size);

    const file_text = ptr[0..file_size];

    const current_mmap_slice = if (active_mmap_ptr) |m_ptr| m_ptr[0..active_mmap_size] else "";
    if (std.mem.eql(u8, current_mmap_slice, file_text)) return;

    remap_active_file() catch return;

    // Retrieve active bounds from GUI and reload just that page range
    var start: c_int = 0;
    var end: c_int = 0;
    var tot: c_int = 0;
    gui_get_active_page_bounds(&start, &end, &tot);

    const total_lines = @as(c_int, @intCast(line_offsets.items.len));
    if (start >= total_lines) start = 0;
    if (end > total_lines) end = total_lines;
    if (end <= start) {
        end = if (total_lines > 100) 100 else total_lines;
        start = 0;
    }

    var page_len: c_int = 0;
    const page_text_ptr = zig_get_text_for_line_range(start, end, &page_len);
    gui_set_text(page_text_ptr.?, page_len);
    gui_init_virtual_document(total_lines, start, end);

    gui_set_sync_status("Updated");
}

fn save_session(file_path: []const u8, line: c_int, col: c_int) !void {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(DB_PATH, &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return error.DbOpenFailed;
    }
    defer _ = c.sqlite3_close(db);
    _ = c.sqlite3_busy_timeout(db, 5000);

    _ = c.sqlite3_exec(db, "INSERT OR IGNORE INTO session_state (id) VALUES (1);", null, null, null);
    const sql = "UPDATE session_state SET active_file = ?, cursor_line = ?, cursor_col = ? WHERE id = 1;";
    var stmt: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) != c.SQLITE_OK) return error.DbPrepareFailed;
    defer _ = c.sqlite3_finalize(stmt);

    const gpa = std.heap.page_allocator;
    const file_z = try gpa.dupeZ(u8, file_path);
    defer gpa.free(file_z);

    _ = c.sqlite3_bind_text(stmt, 1, file_z.ptr, -1, null);
    _ = c.sqlite3_bind_int(stmt, 2, line);
    _ = c.sqlite3_bind_int(stmt, 3, col);
    
    _ = c.sqlite3_step(stmt);
}

fn get_basename(path: []const u8) []const u8 {
    if (std.mem.lastIndexOfScalar(u8, path, '/')) |idx| {
        return path[idx + 1..];
    }
    return path;
}

pub export fn zig_load_ui_builder(app: *anyopaque, out_main_layout: **anyopaque) callconv(.c) ?*anyopaque {
    _ = app;
    _ = out_main_layout;
    return null;
}

var search_mutex: std.atomic.Mutex = .unlocked;
var search_results: ?std.StringHashMap([:0]const u8) = null;
var search_rank: ?std.StringHashMap(usize) = null;

fn lock_search() void {
    while (!search_mutex.tryLock()) {
        _ = c.usleep(10);
    }
}

fn unlock_search() void {
    search_mutex.unlock();
}

pub export fn zig_search_workspace(query_ptr: [*:0]const u8) callconv(.c) void {
    lock_search();
    defer unlock_search();

    const gpa = std.heap.page_allocator;
    if (search_results == null) {
        search_results = std.StringHashMap([:0]const u8).init(gpa);
    }
    if (search_rank == null) {
        search_rank = std.StringHashMap(usize).init(gpa);
    }

    // Clear old results
    var it = search_results.?.iterator();
    while (it.next()) |entry| {
        gpa.free(entry.key_ptr.*);
        gpa.free(entry.value_ptr.*);
    }
    search_results.?.clearRetainingCapacity();
    search_rank.?.clearRetainingCapacity();

    const query = std.mem.span(query_ptr);
    if (query.len == 0) return;

    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(DB_PATH, &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return;
    }
    defer _ = c.sqlite3_close(db);
    _ = c.sqlite3_busy_timeout(db, 5000);

    const sql = "SELECT filepath, snippet(file_content_fts, 0, '<b>', '</b>', '...', 10) FROM file_content_fts WHERE file_content_fts MATCH ?;";
    var stmt: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) != c.SQLITE_OK) return;
    defer _ = c.sqlite3_finalize(stmt);

    const query_z = gpa.dupeZ(u8, query) catch return;
    defer gpa.free(query_z);

    _ = c.sqlite3_bind_text(stmt, 1, query_z.ptr, -1, null);

    var rank: usize = 0;
    while (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
        const filepath_c = c.sqlite3_column_text(stmt, 0);
        const snippet_c = c.sqlite3_column_text(stmt, 1);

        if (filepath_c != null and snippet_c != null) {
            const filepath = std.mem.span(filepath_c);
            const snippet = std.mem.span(snippet_c);

            const filepath_dup = gpa.dupeZ(u8, filepath) catch continue;
            const snippet_dup = gpa.dupeZ(u8, snippet) catch {
                gpa.free(filepath_dup);
                continue;
            };

            search_results.?.put(filepath_dup, snippet_dup) catch {
                gpa.free(filepath_dup);
                gpa.free(snippet_dup);
                continue;
            };
            search_rank.?.put(filepath_dup, rank) catch {};
            rank += 1;
        }
    }
}

pub export fn zig_get_search_snippet(filepath_ptr: [*:0]const u8) callconv(.c) ?[*:0]const u8 {
    lock_search();
    defer unlock_search();

    if (search_results == null) return null;
    const filepath = std.mem.span(filepath_ptr);
    if (search_results.?.get(filepath)) |snippet| {
        return snippet.ptr;
    }
    return null;
}

pub export fn zig_get_search_rank(filepath_ptr: [*:0]const u8) callconv(.c) c_int {
    lock_search();
    defer unlock_search();

    if (search_rank == null) return -1;
    const filepath = std.mem.span(filepath_ptr);
    if (search_rank.?.get(filepath)) |rank| {
        return @intCast(rank);
    }
    return -1;
}

pub fn alert_file_updated() void {
    gui_run_on_main_thread(&reload_file_callback, null);
}
