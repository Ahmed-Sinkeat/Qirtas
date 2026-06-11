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
extern fn gui_set_text(text: [*]const u8, len: c_int) void;
extern fn gui_set_title(title: [*:0]const u8) void;
extern fn gui_set_sync_status(status: [*:0]const u8) void;
extern fn gui_show_editor() void;
extern fn gui_get_cursor_position(line: *c_int, col: *c_int) void;
extern fn gui_set_cursor_position(line: c_int, col: c_int) void;
extern fn gui_reload_full_buffer() void;
extern fn gui_set_buffer_modified(modified: c_int) void;
extern fn gui_refresh_explorer() void;
extern fn gui_index_all_files() void;
extern fn gui_index_file(filename: [*:0]const u8) void;
extern fn gui_remove_file_from_index(filename: [*:0]const u8) void;
extern fn gui_trigger_autosave() void;
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
var active_file_is_encrypted: bool = true;
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

pub var active_master_key: ?[32]u8 = null;

const UndoEntry = struct {
    text: []u8,
    cursor_line: c_int,
    cursor_col: c_int,
};

const UNDO_CAPACITY: usize = 100;

var undo_stack: [UNDO_CAPACITY]UndoEntry = undefined;
var undo_top: usize = 0;
var redo_stack: [UNDO_CAPACITY]UndoEntry = undefined;
var redo_top: usize = 0;
var undo_push_pending: bool = false;
var undo_pending_line: c_int = 0;
var undo_pending_col: c_int = 0;
var file_open_in_progress: bool = false;

pub export fn zig_has_active_master_key() callconv(.c) c_int {
    return if (active_master_key != null) 1 else 0;
}

const SYSTEM_KEYS_SCHEMA_SQL = "CREATE TABLE IF NOT EXISTS system_keys (id INTEGER PRIMARY KEY CHECK (id = 1), encrypted_master_key_machine TEXT NOT NULL, encrypted_master_key_passphrase TEXT, passphrase_salt TEXT);";

fn ensureSystemKeysSchema(db: *c.sqlite3) void {
    _ = c.sqlite3_exec(db, SYSTEM_KEYS_SCHEMA_SQL, null, null, null);
}

fn fillRandomBytes(buf: []u8) !void {
    var i: usize = 0;
    while (i < buf.len) {
        const rc = std.os.linux.getrandom(buf[i..].ptr, buf.len - i, 0);
        switch (std.posix.errno(rc)) {
            .SUCCESS => i += @intCast(rc),
            .INTR => continue,
            else => return error.RandomFailed,
        }
    }
}

fn currentDocumentSlice() []const u8 {
    return if (active_mmap_ptr) |ptr| ptr[0..active_mmap_size] else "";
}

fn freeUndoEntry(entry: *UndoEntry) void {
    if (entry.text.len > 0) {
        std.heap.page_allocator.free(entry.text);
    }
}

fn dropOldestEntry(stack: *[UNDO_CAPACITY]UndoEntry, top: *usize) void {
    if (top.* == 0) return;
    freeUndoEntry(&stack[0]);
    var i: usize = 1;
    while (i < top.*) : (i += 1) {
        stack[i - 1] = stack[i];
    }
    top.* -= 1;
}

fn clearUndoStack(stack: *[UNDO_CAPACITY]UndoEntry, top: *usize) void {
    while (top.* > 0) {
        top.* -= 1;
        freeUndoEntry(&stack[top.*]);
    }
}

fn pushUndoEntry(stack: *[UNDO_CAPACITY]UndoEntry, top: *usize, entry: UndoEntry) void {
    if (top.* == UNDO_CAPACITY) {
        dropOldestEntry(stack, top);
    }
    stack[top.*] = entry;
    top.* += 1;
}

fn captureUndoEntry(cursor_line: c_int, cursor_col: c_int) !UndoEntry {
    const ptr = active_mmap_ptr orelse return UndoEntry{
        .text = @constCast(""[0..0]),
        .cursor_line = cursor_line,
        .cursor_col = cursor_col,
    };
    const len = active_mmap_size;
    if (len == 0) return UndoEntry{
        .text = @constCast(""[0..0]),
        .cursor_line = cursor_line,
        .cursor_col = cursor_col,
    };
    const snapshot = try std.heap.page_allocator.alloc(u8, len);
    @memcpy(snapshot, ptr[0..len]);
    return UndoEntry{
        .text = snapshot,
        .cursor_line = cursor_line,
        .cursor_col = cursor_col,
    };
}

pub fn encryptWithMasterKey(allocator: std.mem.Allocator, key: [32]u8, plaintext: []const u8) ![]u8 {
    var nonce: [12]u8 = undefined;
    try fillRandomBytes(&nonce);

    const cipher = std.crypto.aead.chacha_poly.ChaCha20Poly1305;
    const out_len = 12 + plaintext.len + 16;
    const raw_buf = try allocator.alloc(u8, out_len);
    errdefer allocator.free(raw_buf);

    @memcpy(raw_buf[0..12], &nonce);
    var tag: [16]u8 = undefined;
    cipher.encrypt(raw_buf[12 .. 12 + plaintext.len], &tag, plaintext, "", nonce, key);
    @memcpy(raw_buf[12 + plaintext.len ..], &tag);
    return raw_buf;
}

pub fn decryptWithMasterKey(allocator: std.mem.Allocator, key: [32]u8, blob: []const u8) ![]u8 {
    if (blob.len < 28) return error.InvalidFileFormat;

    var nonce: [12]u8 = undefined;
    @memcpy(&nonce, blob[0..12]);

    const ct_len = blob.len - 12 - 16;
    var tag: [16]u8 = undefined;
    @memcpy(&tag, blob[12 + ct_len ..]);

    const pt_buf = try allocator.alloc(u8, ct_len);
    errdefer allocator.free(pt_buf);

    const cipher = std.crypto.aead.chacha_poly.ChaCha20Poly1305;
    try cipher.decrypt(pt_buf, blob[12 .. 12 + ct_len], tag, "", nonce, key);
    return pt_buf;
}

fn initMasterKey() void {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(DB_PATH, &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return;
    }
    defer _ = c.sqlite3_close(db);
    _ = c.sqlite3_busy_timeout(db, 5000);

    ensureSystemKeysSchema(db.?);

    const query_sql = "SELECT encrypted_master_key_machine FROM system_keys WHERE id = 1;";
    var stmt: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v2(db, query_sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
        defer _ = c.sqlite3_finalize(stmt);
        if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
            const encrypted_text = c.sqlite3_column_text(stmt, 0);
            if (encrypted_text != null) {
                const hex_span = std.mem.span(encrypted_text);
                const decrypted = sync.decryptToken(std.heap.page_allocator, hex_span) catch null;
                if (decrypted) |d| {
                    if (d.len == 32) {
                        var key: [32]u8 = undefined;
                        @memcpy(&key, d);
                        active_master_key = key;
                    }
                    std.heap.page_allocator.free(d);
                }
            }
        } else {
            var new_key: [32]u8 = undefined;
            fillRandomBytes(&new_key) catch return;
            const encrypted_hex = sync.encryptToken(std.heap.page_allocator, &new_key) catch return;
            defer std.heap.page_allocator.free(encrypted_hex);

            const insert_sql = "INSERT INTO system_keys (id, encrypted_master_key_machine) VALUES (1, ?);";
            var ins_stmt: ?*c.sqlite3_stmt = null;
            if (c.sqlite3_prepare_v2(db, insert_sql.ptr, -1, &ins_stmt, null) == c.SQLITE_OK) {
                defer _ = c.sqlite3_finalize(ins_stmt);
                const hex_z = std.heap.page_allocator.dupeZ(u8, encrypted_hex) catch return;
                defer std.heap.page_allocator.free(hex_z);
                _ = c.sqlite3_bind_text(ins_stmt, 1, hex_z.ptr, -1, null);
                _ = c.sqlite3_step(ins_stmt);
            }
            active_master_key = new_key;
        }
    }
}

pub fn main(init: std.process.Init) !void {
    // Ensure the config directory exists
    _ = c.mkdir("/home", 0o755);
    _ = c.mkdir("/home/.config", 0o755);
    _ = c.mkdir("/home/.config/lawh", 0o755);

    initMasterKey();

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
            ensureSystemKeysSchema(db.?);

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

    if (active_master_key == null) {
        gui_set_sync_status("Recovery required");
        return;
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

    file_open_in_progress = true;
    defer file_open_in_progress = false;
    load_file_and_update_gui(file_to_load) catch |err| {
        std.debug.print("Failed to load active notes: {}\n", .{err});
    };

    // Restore cursor position
    gui_set_cursor_position(initial_cursor_line, initial_cursor_col);
    zig_undo_push(initial_cursor_line, initial_cursor_col);

    // Spawn Asynchronous Autosave Thread
    // _ = std.Thread.spawn(.{}, autosave_thread_loop, .{}) catch |err| {
    //     std.debug.print("Failed to spawn autosave thread: {}\n", .{err});
    // };

    // Spawn File System Watcher Thread
    _ = std.Thread.spawn(.{}, file_watcher_thread_loop, .{}) catch |err| {
        std.debug.print("Failed to spawn file watcher thread: {}\n", .{err});
    };
}

extern fn gui_prepare_tab_switch() void;

fn isSupported(filename: []const u8) bool {
    const supported = &[_][]const u8{
        ".md", ".txt",
        ".zig", ".c", ".h",
        ".css", ".js", ".ts",
        ".json", ".yaml", ".toml",
        ".html", ".xml",
    };
    for (supported) |ext| {
        if (std.mem.endsWith(u8, filename, ext)) return true;
    }
    return false;
}

pub export fn zig_open_file(filename_ptr: [*:0]const u8) callconv(.c) void {
    if (file_open_in_progress) return;
    file_open_in_progress = true;
    defer file_open_in_progress = false;

    gui_tabs_save_active_to_cache();
    gui_prepare_tab_switch();

    const filename = std.mem.span(filename_ptr);

    // Save previous cursor position first
    var old_line: c_int = 1;
    var old_col: c_int = 0;
    gui_get_cursor_position(&old_line, &old_col);
    if (active_file_path_len > 0 and !std.mem.eql(u8, active_file_path[0..active_file_path_len], "Untitled")) {
        save_session(active_file_path[0..active_file_path_len], old_line, old_col) catch {};
    }

    if (std.mem.eql(u8, filename, "Untitled")) {
        zig_undo_clear();

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
        gui_set_title("Untitled - Qirtas");
        gui_set_sync_status("Not Synced");
        gui_show_editor();
        gui_tabs_restore_active_from_cache();
        var seed_line: c_int = 0;
        var seed_col: c_int = 0;
        gui_get_cursor_position(&seed_line, &seed_col);
        zig_undo_push(seed_line, seed_col);
        return;
    }

    // Filter editable extensions
    if (isSupported(filename)) {
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
        var seed_line: c_int = 0;
        var seed_col: c_int = 0;
        gui_get_cursor_position(&seed_line, &seed_col);
        zig_undo_push(seed_line, seed_col);
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
    file_open_in_progress = true;
    defer file_open_in_progress = false;
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
    zig_undo_clear();

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
    if (file_size == 0) {
        size.* = 0;
        active_file_is_encrypted = false;
        return "";
    }

    const ptr = c.mmap(null, file_size, c.PROT_READ, c.MAP_SHARED, fd, 0);
    if (ptr == c.MAP_FAILED) return error.MmapFailed;
    defer _ = c.munmap(ptr, file_size);

    const mmap_slice: [*]const u8 = @ptrCast(ptr);
    const mmap_content = mmap_slice[0..file_size];
    if (active_master_key) |key| {
        const pt_buf = decryptWithMasterKey(std.heap.page_allocator, key, mmap_content) catch null;
        if (pt_buf) |buf| {
            active_file_is_encrypted = true;
            size.* = buf.len;
            return buf.ptr;
        }
    }

    const plain_buf = try std.heap.page_allocator.dupe(u8, mmap_content);
    active_file_is_encrypted = false;
    size.* = plain_buf.len;
    return plain_buf.ptr;
}

fn unload_file_mmap(ptr: [*]const u8, size: usize) void {
    if (size == 0) return;
    std.heap.page_allocator.free(ptr[0..size]);
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
        } else if (content[i] == '\r' and i + 1 < content.len and content[i + 1] == '\n') {
            continue;
        }
    }
}

fn remap_active_file() !void {
    if (!file_open_in_progress) return error.RemapWithoutGuard;
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
}

fn load_file_and_update_gui(filename: []const u8) !void {
    const gpa = std.heap.page_allocator;
    zig_undo_clear();
    const filename_z = try gpa.dupeZ(u8, filename);
    defer gpa.free(filename_z);

    var file_exists = true;
    Io.Dir.cwd().access(global_io, filename_z, .{}) catch {
        file_exists = false;
    };
    if (!file_exists) {
        var f = try Io.Dir.cwd().createFile(global_io, filename, .{});
        defer f.close(global_io);
        
        if (active_master_key) |key| {
            active_file_is_encrypted = true;
            const pt = "# New Notebook Document\n\n- Start writing here...\n";
            const enc_blob = encryptWithMasterKey(std.heap.page_allocator, key, pt) catch return;
            defer std.heap.page_allocator.free(enc_blob);
            try f.writeStreamingAll(global_io, enc_blob);
        } else {
            active_file_is_encrypted = false;
        }
    }

    try remap_active_file();

    const total_lines = @as(c_int, @intCast(line_offsets.items.len));
    var page_len: c_int = 0;
    const page_text_ptr = zig_get_text_for_line_range(0, total_lines, &page_len);
    gui_set_text(page_text_ptr.?, page_len);
    
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
    std.debug.print(
    "GET_RANGE start={} end={} start_off={} end_off={}\n",
    .{
        start_line,
        end_line,
        start_offset,
        end_offset,
    },
);        

    if (active_mmap_ptr) |ptr| {
        var s = start_offset;
        var e = end_offset;
        const doc = ptr[0..active_mmap_size];

        while (s < e and s < active_mmap_size and (doc[s] & 0xC0) == 0x80) {
            s += 1;
        }
        while (e > s and e <= active_mmap_size and (doc[e - 1] & 0xC0) == 0x80) {
            e -= 1;
        }

        if (s >= e) {
            out_len.* = 0;
            return "";
        }

        const slice = ptr[s..e];
        out_len.* = @as(c_int, @intCast(slice.len));
        return slice.ptr;
    }
    out_len.* = 0;
    return "";
}

pub export fn zig_save_active_page(start_line: c_int, end_line: c_int, text: [*:0]const u8) callconv(.c) c_int {
    
    std.debug.print(
    "SAVE_PAGE start={} end={} text_len={}\n",
    .{ start_line, end_line, std.mem.len(text) }
 ); 
    if (start_line < 0 or end_line < 0) return 1;
    if (active_file_is_encrypted and active_master_key == null) return 1;

    const gpa = std.heap.page_allocator;
    const start_idx = @as(usize, @intCast(start_line));
    const end_idx = @as(usize, @intCast(end_line));

    if (line_offsets.items.len == 0) return 1;
    if (start_idx >= line_offsets.items.len) return 1;

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

    if (active_file_is_encrypted) {
        const key = active_master_key.?;
        const enc_blob = encryptWithMasterKey(gpa, key, new_content) catch return 1;
        defer gpa.free(enc_blob);
        file.writeStreamingAll(global_io, enc_blob) catch return 1;
    } else {
        file.writeStreamingAll(global_io, new_content) catch return 1;
    }
    
    
    std.debug.print(
        "SAVE_PAGE start={} end={} new_text_len={} new_size={}\n",
        .{ start_idx, end_idx, new_text_slice.len, new_size }
    );

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
                if (file_open_in_progress) continue;
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
    if (file_open_in_progress) return;

    std.debug.print("RELOAD_BLOCKED_TEST\n", .{});

    if (true) return;

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

    const total_lines = @as(c_int, @intCast(line_offsets.items.len));
    var page_len: c_int = 0;
    const page_text_ptr = zig_get_text_for_line_range(0, total_lines, &page_len);
    gui_set_text(page_text_ptr.?, page_len);

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

pub const Position = extern struct {
    line: c_int,
    col: c_int,
};

fn positionToOffset(pos: Position) usize {
    if (pos.line < 0) return 0;
    const u_line = @as(usize, @intCast(pos.line));
    if (u_line >= line_offsets.items.len) {
        return active_mmap_size;
    }
    const line_start = line_offsets.items[u_line];
    const line_end = if (u_line + 1 < line_offsets.items.len)
        line_offsets.items[u_line + 1]
    else
        active_mmap_size;

    const content = if (active_mmap_ptr) |ptr| ptr[0..active_mmap_size] else "";
    const line_text = content[line_start..line_end];

    var byte_idx: usize = 0;
    var char_idx: usize = 0;
    const u_col = if (pos.col < 0) @as(usize, 0) else @as(usize, @intCast(pos.col));
    while (char_idx < u_col and byte_idx < line_text.len) {
        const len = std.unicode.utf8ByteSequenceLength(line_text[byte_idx]) catch 1;
        byte_idx += len;
        char_idx += 1;
    }
    return line_start + byte_idx;
}

fn write_document_content_and_remap(new_content: []const u8) !void {
    if (active_file_path_len == 0) return error.NoActiveFile;
    if (file_open_in_progress) return error.RemapWithoutGuard;
    file_open_in_progress = true;
    defer file_open_in_progress = false;
    const path = active_file_path[0..active_file_path_len];
    const gpa = std.heap.page_allocator;

    if (std.mem.eql(u8, path, "Untitled")) {
        const plain_buf = try gpa.dupe(u8, new_content);
        if (active_mmap_ptr) |ptr| {
            gpa.free(ptr[0..active_mmap_size]);
        }
        active_mmap_ptr = plain_buf.ptr;
        active_mmap_size = plain_buf.len;
        try populate_line_offsets(new_content);

        return;
    }

    var file = try Io.Dir.cwd().createFile(global_io, path, .{});
    defer file.close(global_io);

    if (active_file_is_encrypted) {
        const key = active_master_key orelse return error.NoMasterKey;
        const enc_blob = try encryptWithMasterKey(gpa, key, new_content);
        defer gpa.free(enc_blob);
        try file.writeStreamingAll(global_io, enc_blob);
    } else {
        try file.writeStreamingAll(global_io, new_content);
    }

    try remap_active_file();
}

fn restoreSnapshot(entry: UndoEntry) void {
    file_open_in_progress = true;
    defer file_open_in_progress = false;
    write_document_content_and_remap(entry.text) catch return;
    gui_reload_full_buffer();
    gui_set_buffer_modified(1);
    gui_set_sync_status("Not Synced");
    gui_set_cursor_position(entry.cursor_line, entry.cursor_col);
}

pub export fn zig_undo_push(cursor_line: c_int, cursor_col: c_int) callconv(.c) void {
    if (!undo_push_pending) {
        undo_push_pending = true;
        undo_pending_line = cursor_line;
        undo_pending_col = cursor_col;
    }
}

pub export fn zig_undo_commit() callconv(.c) void {
    if (!undo_push_pending) return;
    undo_push_pending = false;
    const entry = captureUndoEntry(undo_pending_line, undo_pending_col) catch return;
    pushUndoEntry(&undo_stack, &undo_top, entry);
    clearUndoStack(&redo_stack, &redo_top);
}

pub export fn zig_undo_clear() callconv(.c) void {
    clearUndoStack(&undo_stack, &undo_top);
    clearUndoStack(&redo_stack, &redo_top);
}

pub export fn zig_undo() callconv(.c) void {
    if (undo_top < 2) return;

    const current = undo_stack[undo_top - 1];
    undo_top -= 1;
    pushUndoEntry(&redo_stack, &redo_top, current);

    const previous = undo_stack[undo_top - 1];
    restoreSnapshot(previous);
}

pub export fn zig_redo() callconv(.c) void {
    if (redo_top == 0) return;

    var current_line: c_int = 0;
    var current_col: c_int = 0;
    gui_get_cursor_position(&current_line, &current_col);
    const current = captureUndoEntry(current_line, current_col) catch UndoEntry{
        .text = @constCast(""[0..0]),
        .cursor_line = current_line,
        .cursor_col = current_col,
    };
    pushUndoEntry(&undo_stack, &undo_top, current);

    const redo_entry = redo_stack[redo_top - 1];
    redo_top -= 1;
    restoreSnapshot(redo_entry);
}

pub export fn zig_insert_text(pos: Position, text: [*:0]const u8) callconv(.c) void {
    const text_slice = std.mem.span(text);
    if (text_slice.len == 0) return;

    const gpa = std.heap.page_allocator;
    const content = if (active_mmap_ptr) |ptr| ptr[0..active_mmap_size] else "";

    const offset = positionToOffset(pos);
    const new_size = content.len + text_slice.len;
    const new_content = gpa.alloc(u8, new_size) catch return;
    defer gpa.free(new_content);

    @memcpy(new_content[0..offset], content[0..offset]);
    @memcpy(new_content[offset .. offset + text_slice.len], text_slice);
    @memcpy(new_content[offset + text_slice.len ..], content[offset..]);

    write_document_content_and_remap(new_content) catch {};
}

pub export fn zig_delete_range(start: Position, end: Position) callconv(.c) void {
    const content = if (active_mmap_ptr) |ptr| ptr[0..active_mmap_size] else "";
    const start_offset = positionToOffset(start);
    const end_offset = positionToOffset(end);

    if (start_offset >= end_offset) return;

    const gpa = std.heap.page_allocator;
    const deleted_len = end_offset - start_offset;
    const new_size = content.len - deleted_len;
    const new_content = gpa.alloc(u8, new_size) catch return;
    defer gpa.free(new_content);

    @memcpy(new_content[0..start_offset], content[0..start_offset]);
    @memcpy(new_content[start_offset..], content[end_offset..]);

    write_document_content_and_remap(new_content) catch {};
}

pub export fn zig_replace_range(start: Position, end: Position, text: [*:0]const u8) callconv(.c) void {
    const text_slice = std.mem.span(text);
    const content = if (active_mmap_ptr) |ptr| ptr[0..active_mmap_size] else "";
    const start_offset = positionToOffset(start);
    const end_offset = positionToOffset(end);

    if (start_offset > end_offset) return;

    const gpa = std.heap.page_allocator;
    const deleted_len = end_offset - start_offset;
    const new_size = content.len - deleted_len + text_slice.len;
    const new_content = gpa.alloc(u8, new_size) catch return;
    defer gpa.free(new_content);

    @memcpy(new_content[0..start_offset], content[0..start_offset]);
    @memcpy(new_content[start_offset .. start_offset + text_slice.len], text_slice);
    @memcpy(new_content[start_offset + text_slice.len ..], content[end_offset..]);

    write_document_content_and_remap(new_content) catch {};
}

pub export fn zig_save_document() callconv(.c) c_int {
    if (active_file_path_len == 0) return 1;
    const path = active_file_path[0..active_file_path_len];
    if (std.mem.eql(u8, path, "Untitled")) return 0;
    const gpa = std.heap.page_allocator;

    const content = if (active_mmap_ptr) |ptr| ptr[0..active_mmap_size] else "";

    var file = Io.Dir.cwd().createFile(global_io, path, .{}) catch return 1;
    defer file.close(global_io);

    if (active_file_is_encrypted) {
        const key = active_master_key orelse return 1;
        const enc_blob = encryptWithMasterKey(gpa, key, content) catch return 1;
        defer gpa.free(enc_blob);
        file.writeStreamingAll(global_io, enc_blob) catch return 1;
    } else {
        file.writeStreamingAll(global_io, content) catch return 1;
    }
    return 0;
}

pub export fn zig_get_document_text() ?[*:0]const u8 {
    const content = if (active_mmap_ptr) |ptr| ptr[0..active_mmap_size] else "";
    const gpa = std.heap.page_allocator;
    const content_z = gpa.dupeZ(u8, content) catch return null;
    return content_z.ptr;
}

pub export fn zig_free_document_text(ptr: ?[*:0]const u8) void {
    if (ptr) |p| {
        const len = std.mem.len(p);
        const slice = p[0..len + 1];
        std.heap.page_allocator.free(slice);
    }
}

pub fn alert_file_updated() void {
    gui_run_on_main_thread(&reload_file_callback, null);
}

test "file encryption/decryption" {
    var key: [32]u8 = undefined;
    try fillRandomBytes(&key);

    var nonce: [12]u8 = undefined;
    try fillRandomBytes(&nonce);

    const pt = "hello world";
    const cipher = std.crypto.aead.chacha_poly.ChaCha20Poly1305;

    var ct: [11]u8 = undefined;
    var tag: [16]u8 = undefined;
    cipher.encrypt(&ct, &tag, pt, "", nonce, key);

    var pt_out: [11]u8 = undefined;
    try cipher.decrypt(&pt_out, &ct, tag, "", nonce, key);
    try std.testing.expect(std.mem.eql(u8, pt, &pt_out));
}

test "system_keys schema is created with expected columns" {
    var db: ?*c.sqlite3 = null;
    try std.testing.expectEqual(c.SQLITE_OK, c.sqlite3_open(":memory:", &db));
    defer {
        if (db != null) _ = c.sqlite3_close(db.?);
    }

    ensureSystemKeysSchema(db.?);

    const sql = "PRAGMA table_info(system_keys);";
    var stmt: ?*c.sqlite3_stmt = null;
    try std.testing.expectEqual(c.SQLITE_OK, c.sqlite3_prepare_v2(db.?, sql.ptr, -1, &stmt, null));
    defer _ = c.sqlite3_finalize(stmt);

    var saw_machine_key = false;
    var saw_machine_notnull = false;
    var saw_passphrase_key = false;
    var saw_salt_key = false;

    while (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
        const name_c = c.sqlite3_column_text(stmt, 1);
        const type_c = c.sqlite3_column_text(stmt, 2);
        const notnull = c.sqlite3_column_int(stmt, 3);

        if (name_c == null or type_c == null) continue;
        const name = std.mem.span(name_c);
        const col_type = std.mem.span(type_c);

        if (std.mem.eql(u8, name, "encrypted_master_key_machine")) {
            saw_machine_key = std.mem.eql(u8, col_type, "TEXT");
            saw_machine_notnull = notnull == 1;
        } else if (std.mem.eql(u8, name, "encrypted_master_key_passphrase")) {
            saw_passphrase_key = std.mem.eql(u8, col_type, "TEXT");
        } else if (std.mem.eql(u8, name, "passphrase_salt")) {
            saw_salt_key = std.mem.eql(u8, col_type, "TEXT");
        }
    }

    try std.testing.expect(saw_machine_key);
    try std.testing.expect(saw_machine_notnull);
    try std.testing.expect(saw_passphrase_key);
    try std.testing.expect(saw_salt_key);
}

test "master key file blob round-trip" {
    const key: [32]u8 = .{
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    };

    const plaintext = "phase three master key test";
    const blob = try encryptWithMasterKey(std.testing.allocator, key, plaintext);
    defer std.testing.allocator.free(blob);

    const decrypted = try decryptWithMasterKey(std.testing.allocator, key, blob);
    defer std.testing.allocator.free(decrypted);

    try std.testing.expectEqualStrings(plaintext, decrypted);
}
