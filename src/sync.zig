const std = @import("std");
const Io = std.Io;

const c = @cImport({
    @cInclude("sqlite3.h");
    @cInclude("sys/stat.h");
    @cInclude("sys/mman.h");
    @cInclude("unistd.h");
    @cInclude("fcntl.h");
    @cInclude("time.h");
    @cInclude("dirent.h");
    @cInclude("stdlib.h");
});

extern fn gui_update_sync_status(connected: c_int, status_text: [*:0]const u8) void;
extern fn gui_update_dropbox_status(connected: c_int, status_text: [*:0]const u8) void;
extern fn gui_update_github_status(connected: c_int, status_text: [*:0]const u8) void;
extern fn gui_update_local_sync_status(connected: c_int, status_text: [*:0]const u8) void;
extern fn gui_refresh_explorer() void;
extern fn gui_index_file(filename: [*:0]const u8) void;
extern fn gui_trigger_autosave() void;

// Active IO reference from main.zig
const main = @import("main.zig");

fn dbp() [*:0]const u8 {
    return main.dbPathZ();
}

const TokenResponse = struct {
    access_token: []const u8,
    refresh_token: ?[]const u8 = null,
    expires_in: i64,
    token_type: []const u8,
};

const RefreshResponse = struct {
    access_token: []const u8,
    expires_in: i64,
    token_type: []const u8,
};

const DriveSearchResponse = struct {
    files: []struct {
        id: []const u8,
        name: []const u8,
        modifiedTime: []const u8,
    },
};

const DriveUploadResponse = struct {
    id: []const u8,
};

// Local credentials struct
const SyncCredentials = struct {
    client_id: []const u8,
    client_secret: []const u8,
    access_token: ?[]const u8,
    refresh_token: ?[]const u8,
    expiry_time: i64,
};

fn get_credentials(allocator: std.mem.Allocator) !SyncCredentials {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(dbp(), &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return error.DbOpenFailed;
    }
    defer _ = c.sqlite3_close(db);
    _ = c.sqlite3_busy_timeout(db, 5000);

    const sql = "SELECT client_id, client_secret, access_token, refresh_token, expiry_time FROM sync_tokens WHERE id = 1;";
    var stmt: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) != c.SQLITE_OK) return error.DbPrepareFailed;
    defer _ = c.sqlite3_finalize(stmt);

    if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
        const c_id = c.sqlite3_column_text(stmt, 0);
        const c_sec = c.sqlite3_column_text(stmt, 1);
        const acc_tok = c.sqlite3_column_text(stmt, 2);
        const ref_tok = c.sqlite3_column_text(stmt, 3);
        const exp_time = c.sqlite3_column_int64(stmt, 4);

        if (c_id == null or c_sec == null) return error.CredentialsNotFound;

        const secret_raw = std.mem.span(c_sec);
        const client_secret = if (secret_raw.len == 0)
            try allocator.dupe(u8, "")
        else
            decryptToken(allocator, secret_raw) catch try allocator.dupe(u8, secret_raw);
        errdefer allocator.free(client_secret);

        const dec_access = if (acc_tok != null and std.mem.span(acc_tok).len > 0)
            decryptToken(allocator, std.mem.span(acc_tok)) catch null
        else null;
        const dec_refresh = if (ref_tok != null and std.mem.span(ref_tok).len > 0)
            decryptToken(allocator, std.mem.span(ref_tok)) catch null
        else null;

        return SyncCredentials{
            .client_id = try allocator.dupe(u8, std.mem.span(c_id)),
            .client_secret = client_secret,
            .access_token = dec_access,
            .refresh_token = dec_refresh,
            .expiry_time = exp_time,
        };
    }

    return error.CredentialsNotFound;
}

fn free_credentials(creds: *SyncCredentials, allocator: std.mem.Allocator) void {
    allocator.free(creds.client_id);
    allocator.free(creds.client_secret);
    if (creds.access_token) |t| {
        @memset(@constCast(t), 0);
        allocator.free(t);
    }
    if (creds.refresh_token) |t| {
        @memset(@constCast(t), 0);
        allocator.free(t);
    }
}

// FFI: Save credentials entered by user
pub export fn zig_save_sync_credentials(client_id_ptr: [*:0]const u8, client_secret_ptr: [*:0]const u8) callconv(.c) void {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(dbp(), &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return;
    }
    defer _ = c.sqlite3_close(db);
    _ = c.sqlite3_busy_timeout(db, 5000);

    const create_table_sql = 
        \\CREATE TABLE IF NOT EXISTS sync_tokens (
        \\    id INTEGER PRIMARY KEY CHECK (id = 1),
        \\    client_id TEXT NOT NULL,
        \\    client_secret TEXT NOT NULL,
        \\    access_token TEXT,
        \\    refresh_token TEXT,
        \\    expiry_time INTEGER DEFAULT 0
        \\);
    ;
    _ = c.sqlite3_exec(db, create_table_sql.ptr, null, null, null);

    const allocator = std.heap.page_allocator;
    const client_id = std.mem.span(client_id_ptr);
    const client_secret = std.mem.span(client_secret_ptr);
    const stored_secret = if (client_secret.len == 0)
        allocator.dupe(u8, "") catch return
    else
        encryptToken(allocator, client_secret) catch return;
    defer allocator.free(stored_secret);

    const sql = "INSERT OR REPLACE INTO sync_tokens (id, client_id, client_secret) VALUES (1, ?, ?);";
    var stmt: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
        const client_id_z = allocator.dupeZ(u8, client_id) catch return;
        defer allocator.free(client_id_z);
        const stored_secret_z = allocator.dupeZ(u8, stored_secret) catch return;
        defer allocator.free(stored_secret_z);
        _ = c.sqlite3_bind_text(stmt, 1, client_id_z.ptr, -1, null);
        _ = c.sqlite3_bind_text(stmt, 2, stored_secret_z.ptr, -1, null);
        _ = c.sqlite3_step(stmt);
        _ = c.sqlite3_finalize(stmt);
    }
}

// FFI: Spawns browser for Google Auth flow
pub export fn zig_sync_connect() callconv(.c) void {
    const allocator = std.heap.page_allocator;
    var creds = get_credentials(allocator) catch {
        var db: ?*c.sqlite3 = null;
        if (c.sqlite3_open(dbp(), &db) == c.SQLITE_OK) {
            _ = c.sqlite3_busy_timeout(db, 5000);
            const create_table_sql = 
                \\CREATE TABLE IF NOT EXISTS sync_tokens (
                \\    id INTEGER PRIMARY KEY CHECK (id = 1),
                \\    client_id TEXT NOT NULL,
                \\    client_secret TEXT NOT NULL,
                \\    access_token TEXT,
                \\    refresh_token TEXT,
                \\    expiry_time INTEGER DEFAULT 0
                \\);
            ;
            _ = c.sqlite3_exec(db, create_table_sql.ptr, null, null, null);
            _ = c.sqlite3_close(db);
        } else {
            if (db != null) _ = c.sqlite3_close(db);
        }
        gui_update_sync_status(0, "Configure Google client ID");
        return;
    };
    defer free_credentials(&creds, allocator);

    const url = std.fmt.allocPrint(allocator, 
       "https://accounts.google.com/o/oauth2/v2/auth?client_id={s}&redirect_uri=http://localhost:12345&response_type=code&scope=https://www.googleapis.com/auth/drive.appdata&access_type=offline&prompt=consent",
       .{creds.client_id}
     ) catch return;
    defer allocator.free(url);

    var child = std.process.spawn(main.global_io, .{
        .argv = &[_][]const u8{ "xdg-open", url },
    }) catch blk: {
        break :blk std.process.spawn(main.global_io, .{
            .argv = &[_][]const u8{ "gio", "open", url },
        }) catch return;
    };
    _ = child.wait(main.global_io) catch {};

    gui_update_sync_status(2, "Enter code below...");
}

// FFI: Submit the auth code and exchange for tokens
pub export fn zig_sync_submit_code(code_ptr: [*:0]const u8) callconv(.c) void {
    const code = std.mem.span(code_ptr);
    const allocator = std.heap.page_allocator;

    var creds = get_credentials(allocator) catch return;
    defer free_credentials(&creds, allocator);

    gui_update_sync_status(2, "Exchanging code...");

    const code_dup = allocator.dupe(u8, code) catch return;
    const client_id_dup = allocator.dupe(u8, creds.client_id) catch {
        allocator.free(code_dup);
        return;
    };
    const client_secret_dup = allocator.dupe(u8, creds.client_secret) catch {
        allocator.free(code_dup);
        allocator.free(client_id_dup);
        return;
    };

    _ = std.Thread.spawn(.{}, exchange_token_worker, .{
        allocator,
        code_dup,
        client_id_dup,
        client_secret_dup,
    }) catch {
        allocator.free(code_dup);
        allocator.free(client_id_dup);
        allocator.free(client_secret_dup);
        gui_update_sync_status(0, "Disconnected");
    };
}

fn exchange_token_worker(allocator: std.mem.Allocator, code: []const u8, client_id: []const u8, client_secret: []const u8) void {
    defer allocator.free(code);
    defer allocator.free(client_id);
    defer allocator.free(client_secret);

    exchange_token_impl(allocator, code, client_id, client_secret) catch |err| {
        std.debug.print("Token exchange failed: {}\n", .{err});
        var msg_buf: [128]u8 = undefined;
        gui_update_sync_status(sync_error_connection_state(err), sync_error_message_z(err, &msg_buf));
        return;
    };

    gui_update_sync_status(1, "Connected");
}

fn exchange_token_impl(allocator: std.mem.Allocator, code: []const u8, client_id: []const u8, client_secret: []const u8) !void {
    var client = std.http.Client{ .allocator = allocator, .io = main.global_io };
    defer client.deinit();

    const uri = try std.Uri.parse("https://oauth2.googleapis.com/token");
    
    var req = try client.request(.POST, uri, .{
        .extra_headers = &[_]std.http.Header{
            .{ .name = "Content-Type", .value = "application/x-www-form-urlencoded" },
        },
    });
    defer req.deinit();

    const body = if (client_secret.len > 0)
        try std.fmt.allocPrint(allocator,
            "code={s}&client_id={s}&client_secret={s}&redirect_uri=http://localhost:12345&grant_type=authorization_code",
            .{ code, client_id, client_secret },
        )
    else
        try std.fmt.allocPrint(allocator,
            "code={s}&client_id={s}&redirect_uri=http://localhost:12345&grant_type=authorization_code",
            .{ code, client_id },
        );
    defer allocator.free(body);

    try req.sendBodyComplete(body);

    var redirect_buf: [1024]u8 = undefined;
    var response = try req.receiveHead(&redirect_buf);
    
    if (response.head.status != .ok) return error.TokenExchangeFailed;

    var transfer_buf: [4096]u8 = undefined;
    const rdr = response.reader(&transfer_buf);

    // آلية القراءة الآمنة المتوافقة كلياً عبر تجميع دفق البيانات في ArrayList
    var token_json_list: std.ArrayList(u8) = .empty;
    defer token_json_list.deinit(allocator);

    while (true) {
        var chunk_buf: [4096]u8 = undefined;
        const read_bytes = rdr.readSliceShort(&chunk_buf) catch |err| {
            if (err == error.EndOfStream) break;
            return err;
        };
        if (read_bytes == 0) break;
        try token_json_list.appendSlice(allocator, chunk_buf[0..read_bytes]);
    }

    const parsed = try std.json.parseFromSlice(TokenResponse, allocator, token_json_list.items, .{ .ignore_unknown_fields = true });
    defer parsed.deinit();

    const expiry = @as(i64, @intCast(c.time(null))) + parsed.value.expires_in;

    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(dbp(), &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return error.DbOpenFailed;
    }
    defer _ = c.sqlite3_close(db);
    _ = c.sqlite3_busy_timeout(db, 5000);

    const sql = "UPDATE sync_tokens SET access_token = ?, refresh_token = ?, expiry_time = ? WHERE id = 1;";
    var stmt: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
        const enc_access = try encryptToken(allocator, parsed.value.access_token);
        defer allocator.free(enc_access);
        const access_token_z = try allocator.dupeZ(u8, enc_access);
        defer allocator.free(access_token_z);

        const enc_refresh = if (parsed.value.refresh_token) |rt| try encryptToken(allocator, rt) else null;
        defer if (enc_refresh) |er| allocator.free(er);
        const refresh_token_z = if (enc_refresh) |er| try allocator.dupeZ(u8, er) else null;
        defer if (refresh_token_z) |erz| allocator.free(erz);

        _ = c.sqlite3_bind_text(stmt, 1, access_token_z.ptr, -1, null);
        if (refresh_token_z) |erz| {
            _ = c.sqlite3_bind_text(stmt, 2, erz.ptr, -1, null);
        } else {
            _ = c.sqlite3_bind_text(stmt, 2, "", -1, null);
        }
        _ = c.sqlite3_bind_int64(stmt, 3, expiry);
        _ = c.sqlite3_step(stmt);
        _ = c.sqlite3_finalize(stmt);
    }
}

fn refresh_token_if_needed(allocator: std.mem.Allocator, creds: *SyncCredentials) ![]const u8 {
    const current_time = @as(i64, @intCast(c.time(null)));
    if (creds.access_token != null and creds.expiry_time > current_time + 300) {
        return try allocator.dupe(u8, creds.access_token.?);
    }

    if (creds.refresh_token == null or creds.refresh_token.?.len == 0) return error.NoRefreshToken;

    var client = std.http.Client{ .allocator = allocator, .io = main.global_io };
    defer client.deinit();

    const uri = try std.Uri.parse("https://oauth2.googleapis.com/token");
    var req = try client.request(.POST, uri, .{
        .extra_headers = &[_]std.http.Header{
            .{ .name = "Content-Type", .value = "application/x-www-form-urlencoded" },
        },
    });
    defer req.deinit();

    const body = if (creds.client_secret.len > 0)
        try std.fmt.allocPrint(allocator,
            "refresh_token={s}&client_id={s}&client_secret={s}&grant_type=refresh_token",
            .{ creds.refresh_token.?, creds.client_id, creds.client_secret },
        )
    else
        try std.fmt.allocPrint(allocator,
            "refresh_token={s}&client_id={s}&grant_type=refresh_token",
            .{ creds.refresh_token.?, creds.client_id },
        );
    defer allocator.free(body);

    try req.sendBodyComplete(body);

    var redirect_buf: [1024]u8 = undefined;
    var response = try req.receiveHead(&redirect_buf);
    
    if (response.head.status != .ok) {
        var transfer_buf: [4096]u8 = undefined;
        const rdr = response.reader(&transfer_buf);
        var read_buf: [4096]u8 = undefined;
        const read_bytes = rdr.readSliceShort(&read_buf) catch 0;
        const body_str = read_buf[0..read_bytes];
        
        std.debug.print("Token refresh failed with status {}: {s}\n", .{response.head.status, body_str});

        if (response.head.status == .bad_request or response.head.status == .unauthorized) {
            if (std.mem.indexOf(u8, body_str, "invalid_grant") != null) {
                zig_sync_disconnect();
                return error.AuthenticationExpired;
            }
        }
        return error.TokenRefreshFailed;
    }

    var transfer_buf: [4096]u8 = undefined;
    const rdr = response.reader(&transfer_buf);

    var read_buf: [4096]u8 = undefined;
    const read_bytes = try rdr.readSliceShort(&read_buf);

    const parsed = try std.json.parseFromSlice(RefreshResponse, allocator, read_buf[0..read_bytes], .{ .ignore_unknown_fields = true });
    defer parsed.deinit();

    const new_expiry = @as(i64, @intCast(c.time(null))) + parsed.value.expires_in;

    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(dbp(), &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return error.DbOpenFailed;
    }
    defer _ = c.sqlite3_close(db);
    _ = c.sqlite3_busy_timeout(db, 5000);

    const sql = "UPDATE sync_tokens SET access_token = ?, expiry_time = ? WHERE id = 1;";
    var stmt: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
        const enc_access = try encryptToken(allocator, parsed.value.access_token);
        defer allocator.free(enc_access);
        const access_token_z = try allocator.dupeZ(u8, enc_access);
        defer allocator.free(access_token_z);

        _ = c.sqlite3_bind_text(stmt, 1, access_token_z.ptr, -1, null);
        _ = c.sqlite3_bind_int64(stmt, 2, new_expiry);
        _ = c.sqlite3_step(stmt);
        _ = c.sqlite3_finalize(stmt);
    }

    return try allocator.dupe(u8, parsed.value.access_token);
}

fn is_leap_year(y: i32) bool {
    return (@mod(y, 4) == 0 and @mod(y, 100) != 0) or @mod(y, 400) == 0;
}

fn days_in_month(y: i32, m: i32) i32 {
    const days = [_]i32{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 and is_leap_year(y)) return 29;
    return days[@as(usize, @intCast(m - 1))];
}

fn tm_to_unix(year: i32, month: i32, day: i32, hour: i32, minute: i32, second: i32) i64 {
    var days: i64 = 0;
    var y: i32 = 1970;
    while (y < year) : (y += 1) {
        days += if (is_leap_year(y)) @as(i64, 366) else @as(i64, 365);
    }
    var m: i32 = 1;
    while (m < month) : (m += 1) {
        days += days_in_month(year, m);
    }
    days += (day - 1);

    const seconds = days * 86400 + @as(i64, hour) * 3600 + @as(i64, minute) * 60 + second;
    return seconds;
}

fn parse_iso8601_to_unix(iso_str: []const u8) !i64 {
    if (iso_str.len < 19) return error.InvalidDateFormat;
    const year = try std.fmt.parseInt(i32, iso_str[0..4], 10);
    const month = try std.fmt.parseInt(i32, iso_str[5..7], 10);
    const day = try std.fmt.parseInt(i32, iso_str[8..10], 10);
    const hour = try std.fmt.parseInt(i32, iso_str[11..13], 10);
    const minute = try std.fmt.parseInt(i32, iso_str[14..16], 10);
    const second = try std.fmt.parseInt(i32, iso_str[17..19], 10);

    return tm_to_unix(year, month, day, hour, minute, second);
}

fn is_syncable_file(filename: []const u8) bool {
    return std.mem.endsWith(u8, filename, ".md") or 
           std.mem.endsWith(u8, filename, ".txt") or 
           std.mem.endsWith(u8, filename, ".zig") or 
           std.mem.endsWith(u8, filename, ".zon") or
           std.mem.endsWith(u8, filename, ".c") or
           std.mem.endsWith(u8, filename, ".h");
}

fn make_conflict_filename(allocator: std.mem.Allocator, filename: []const u8) ![]const u8 {
    if (std.mem.lastIndexOfScalar(u8, filename, '.')) |dot_idx| {
        return try std.fmt.allocPrint(allocator, "{s}_conflict{s}", .{ filename[0..dot_idx], filename[dot_idx..] });
    } else {
        return try std.fmt.allocPrint(allocator, "{s}_conflict", .{filename});
    }
}

fn read_file_content(allocator: std.mem.Allocator, filename: []const u8) ![]const u8 {
    const filename_z = try allocator.dupeZ(u8, filename);
    defer allocator.free(filename_z);

    const fd = c.open(filename_z.ptr, c.O_RDONLY);
    if (fd < 0) return error.OpenFileFailed;
    defer _ = c.close(fd);

    var st: c.struct_stat = undefined;
    if (c.fstat(fd, &st) < 0) return error.StatFileFailed;
    const file_size = @as(usize, @intCast(st.st_size));

    if (file_size == 0) {
        return try allocator.alloc(u8, 0);
    }

    const ptr = c.mmap(null, file_size, c.PROT_READ, c.MAP_SHARED, fd, 0);
    if (ptr == c.MAP_FAILED) return error.MmapFailed;
    defer _ = c.munmap(ptr, file_size);

    const slice: [*]const u8 = @ptrCast(ptr);
    return try allocator.dupe(u8, slice[0..file_size]);
}

fn write_file_mmap(filename: []const u8, content: []const u8) !void {
    const gpa = std.heap.page_allocator;
    const filename_z = try gpa.dupeZ(u8, filename);
    defer gpa.free(filename_z);

    const fd = c.open(filename_z.ptr, c.O_RDWR | c.O_CREAT | c.O_TRUNC, @as(c.mode_t, 0o644));
    if (fd < 0) return error.CreateFileFailed;
    defer _ = c.close(fd);

    if (content.len == 0) {
        return;
    }

    if (c.ftruncate(fd, @as(c_long, @intCast(content.len))) < 0) {
        return error.FtruncateFailed;
    }

    const ptr = c.mmap(null, content.len, c.PROT_READ | c.PROT_WRITE, c.MAP_SHARED, fd, 0);
    if (ptr == c.MAP_FAILED) return error.MmapFailed;
    defer _ = c.munmap(ptr, content.len);

    const mmap_slice: [*]u8 = @ptrCast(ptr);
    @memcpy(mmap_slice[0..content.len], content);
}

fn is_transient_error(status: std.http.Status) bool {
    return status == .too_many_requests or 
           status == .internal_server_error or 
           status == .bad_gateway or 
           status == .service_unavailable or 
           status == .gateway_timeout;
}

fn sync_error_message(err: anyerror) []const u8 {
    return switch (err) {
        error.AuthenticationExpired => "Error: auth expired. reconnect.",
        error.NoRefreshToken => "Error: missing refresh token.",
        error.CredentialsNotFound => "Error: missing credentials.",
        error.DbOpenFailed => "Error: database unavailable.",
        error.DbPrepareFailed => "Error: database query failed.",
        error.OpenDirFailed => "Error: cannot open folder.",
        error.CreateSyncDirectoryFailed => "Error: cannot create sync folder.",
        error.SyncTargetIsNotDirectory => "Error: sync target is not a directory.",
        error.TokenExchangeFailed => "Error: token exchange failed.",
        error.TokenRefreshFailed => "Error: token refresh failed.",
        error.DriveDownloadFailed => "Error: Google Drive download failed.",
        error.DriveUpdateFailed => "Error: Google Drive update failed.",
        error.DriveCreateFailed => "Error: Google Drive upload failed.",
        error.DriveListFailed => "Error: Google Drive list failed.",
        error.DriveMetaFailed => "Error: Google Drive metadata failed.",
        error.ConnectionRefused => "Error: connection refused.",
        error.UnknownHostName => "Error: unknown host.",
        error.ConnectionReset => "Error: connection reset.",
        error.NetworkUnreachable => "Error: network unreachable.",
        error.HostLacksNetworkAddresses => "Error: no network addresses.",
        else => "Error: sync failed.",
    };
}

fn sync_error_message_z(err: anyerror, buf: []u8) [:0]const u8 {
    return std.fmt.bufPrintZ(buf, "{s}", .{sync_error_message(err)}) catch unreachable;
}

fn sync_error_connection_state(err: anyerror) c_int {
    return switch (err) {
        error.AuthenticationExpired, error.NoRefreshToken, error.CredentialsNotFound => 0,
        else => 1,
    };
}

fn download_file_from_drive(allocator: std.mem.Allocator, access_token: []const u8, file_id: []const u8) ![]const u8 {
    var attempt: u32 = 0;
    const max_attempts = 3;
    while (attempt < max_attempts) : (attempt += 1) {
        if (attempt > 0) {
            _ = c.usleep(attempt * 1000 * 1000);
        }

        var client = std.http.Client{ .allocator = allocator, .io = main.global_io };
        defer client.deinit();

        const download_url = std.fmt.allocPrint(allocator, "https://www.googleapis.com/drive/v3/files/{s}?alt=media", .{file_id}) catch |e| return e;
        defer allocator.free(download_url);

        const uri = std.Uri.parse(download_url) catch |e| return e;
        const auth_hdr = std.fmt.allocPrint(allocator, "Bearer {s}", .{access_token}) catch |e| return e;
        defer allocator.free(auth_hdr);

        var req = client.request(.GET, uri, .{
            .extra_headers = &[_]std.http.Header{
                .{ .name = "Authorization", .value = auth_hdr },
            },
        }) catch |err| {
            std.debug.print("Download attempt {} failed to send request: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };
        defer req.deinit();

        req.sendBodiless() catch |err| {
            std.debug.print("Download attempt {} failed to send body: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };

        var redirect_buf: [1024]u8 = undefined;
        var response = req.receiveHead(&redirect_buf) catch |err| {
            std.debug.print("Download attempt {} failed to receive head: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };

        if (response.head.status != .ok) {
            std.debug.print("Download attempt {} failed with HTTP status: {}\n", .{attempt + 1, response.head.status});
            if (response.head.status == .unauthorized or response.head.status == .forbidden) {
                zig_sync_disconnect();
                return error.AuthenticationExpired;
            }
            if (is_transient_error(response.head.status) and attempt + 1 < max_attempts) {
                continue;
            }
            return error.DriveDownloadFailed;
        }

        var content_list: std.ArrayList(u8) = .empty;
        defer content_list.deinit(allocator);

        var transfer_buf: [4096]u8 = undefined;
        const rdr = response.reader(&transfer_buf);
        var read_success = true;
        while (true) {
            var read_buf: [4096]u8 = undefined;
            const read_bytes = rdr.readSliceShort(&read_buf) catch |err| {
                if (err == error.EndOfStream) break;
                read_success = false;
                break;
            };
            if (read_bytes == 0) break;
            content_list.appendSlice(allocator, read_buf[0..read_bytes]) catch |err| {
                return err;
            };
        }

        if (!read_success) {
            std.debug.print("Download attempt {} failed to read stream\n", .{attempt + 1});
            if (attempt + 1 == max_attempts) return error.DriveDownloadFailed;
            continue;
        }

        return try content_list.toOwnedSlice(allocator);
    }
    return error.DriveDownloadFailed;
}

fn patch_file_on_drive(allocator: std.mem.Allocator, access_token: []const u8, file_id: []const u8, content: []const u8) !void {
    var attempt: u32 = 0;
    const max_attempts = 3;
    while (attempt < max_attempts) : (attempt += 1) {
        if (attempt > 0) {
            _ = c.usleep(attempt * 1000 * 1000);
        }

        var client = std.http.Client{ .allocator = allocator, .io = main.global_io };
        defer client.deinit();

        const update_url = std.fmt.allocPrint(allocator, "https://www.googleapis.com/upload/drive/v3/files/{s}?uploadType=media", .{file_id}) catch |e| return e;
        defer allocator.free(update_url);

        const uri = std.Uri.parse(update_url) catch |e| return e;
        const auth_hdr = std.fmt.allocPrint(allocator, "Bearer {s}", .{access_token}) catch |e| return e;
        defer allocator.free(auth_hdr);

        var req = client.request(.PATCH, uri, .{
            .extra_headers = &[_]std.http.Header{
                .{ .name = "Authorization", .value = auth_hdr },
                .{ .name = "Content-Type", .value = "text/markdown" },
            },
        }) catch |err| {
            std.debug.print("Patch attempt {} failed to send request: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };
        defer req.deinit();

        req.sendBodyComplete(@constCast(content)) catch |err| {
            std.debug.print("Patch attempt {} failed to send body: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };

        var redirect_buf: [1024]u8 = undefined;
        const response = req.receiveHead(&redirect_buf) catch |err| {
            std.debug.print("Patch attempt {} failed to receive head: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };

        if (response.head.status != .ok) {
            std.debug.print("Patch attempt {} failed with HTTP status: {}\n", .{attempt + 1, response.head.status});
            if (response.head.status == .unauthorized or response.head.status == .forbidden) {
                zig_sync_disconnect();
                return error.AuthenticationExpired;
            }
            if (is_transient_error(response.head.status) and attempt + 1 < max_attempts) {
                continue;
            }
            return error.DriveUpdateFailed;
        }
        return;
    }
    return error.DriveUpdateFailed;
}

fn upload_file_to_drive(allocator: std.mem.Allocator, access_token: []const u8, filename: []const u8, content: []const u8) ![]const u8 {
    var attempt: u32 = 0;
    const max_attempts = 3;
    while (attempt < max_attempts) : (attempt += 1) {
        if (attempt > 0) {
            _ = c.usleep(attempt * 1000 * 1000);
        }

        var client = std.http.Client{ .allocator = allocator, .io = main.global_io };
        defer client.deinit();

        const uri = std.Uri.parse("https://www.googleapis.com/upload/drive/v3/files?uploadType=multipart") catch |e| return e;
        const auth_hdr = std.fmt.allocPrint(allocator, "Bearer {s}", .{access_token}) catch |e| return e;
        defer allocator.free(auth_hdr);

        var req = client.request(.POST, uri, .{
            .extra_headers = &[_]std.http.Header{
                .{ .name = "Authorization", .value = auth_hdr },
                .{ .name = "Content-Type", .value = "multipart/related; boundary=foo" },
            },
        }) catch |err| {
            std.debug.print("Upload attempt {} failed to send request: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };
        defer req.deinit();

        const multipart_body = std.fmt.allocPrint(allocator,
            \\--foo
            \\Content-Type: application/json; charset=UTF-8
            \\
            \\{{"name": "{s}", "parents": ["appDataFolder"]}}
            \\
            \\--foo
            \\Content-Type: text/markdown
            \\
            \\{s}
            \\--foo--
            ,
            .{filename, content}
        ) catch |err| return err;
        defer allocator.free(multipart_body);

        req.sendBodyComplete(multipart_body) catch |err| {
            std.debug.print("Upload attempt {} failed to send body: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };

        var redirect_buf: [1024]u8 = undefined;
        var response = req.receiveHead(&redirect_buf) catch |err| {
            std.debug.print("Upload attempt {} failed to receive head: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };

        if (response.head.status != .ok and response.head.status != .created) {
            std.debug.print("Upload attempt {} failed with HTTP status: {}\n", .{attempt + 1, response.head.status});
            if (response.head.status == .unauthorized or response.head.status == .forbidden) {
                zig_sync_disconnect();
                return error.AuthenticationExpired;
            }
            if (is_transient_error(response.head.status) and attempt + 1 < max_attempts) {
                continue;
            }
            return error.DriveCreateFailed;
        }

        var transfer_buf: [4096]u8 = undefined;
        const rdr = response.reader(&transfer_buf);
        var read_buf: [4096]u8 = undefined;
        const read_bytes = rdr.readSliceShort(&read_buf) catch |err| {
            std.debug.print("Upload attempt {} failed to read response body: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };

        const parsed = std.json.parseFromSlice(DriveUploadResponse, allocator, read_buf[0..read_bytes], .{ .ignore_unknown_fields = true }) catch |err| {
            std.debug.print("Upload attempt {} failed to parse response JSON: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };
        defer parsed.deinit();

        return try allocator.dupe(u8, parsed.value.id);
    }
    return error.DriveCreateFailed;
}

fn get_file_modified_time_from_drive(allocator: std.mem.Allocator, access_token: []const u8, file_id: []const u8) !i64 {
    var attempt: u32 = 0;
    const max_attempts = 3;
    while (attempt < max_attempts) : (attempt += 1) {
        if (attempt > 0) {
            _ = c.usleep(attempt * 1000 * 1000);
        }

        var client = std.http.Client{ .allocator = allocator, .io = main.global_io };
        defer client.deinit();

        const url = std.fmt.allocPrint(allocator, "https://www.googleapis.com/drive/v3/files/{s}?fields=modifiedTime", .{file_id}) catch |e| return e;
        defer allocator.free(url);

        const uri = std.Uri.parse(url) catch |e| return e;
        const auth_hdr = std.fmt.allocPrint(allocator, "Bearer {s}", .{access_token}) catch |e| return e;
        defer allocator.free(auth_hdr);

        var req = client.request(.GET, uri, .{
            .extra_headers = &[_]std.http.Header{
                .{ .name = "Authorization", .value = auth_hdr },
            },
        }) catch |err| {
            std.debug.print("Get time attempt {} failed to send request: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };
        defer req.deinit();

        req.sendBodiless() catch |err| {
            std.debug.print("Get time attempt {} failed to send body: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };

        var redirect_buf: [1024]u8 = undefined;
        var response = req.receiveHead(&redirect_buf) catch |err| {
            std.debug.print("Get time attempt {} failed to receive head: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };

        if (response.head.status != .ok) {
            std.debug.print("Get time attempt {} failed with HTTP status: {}\n", .{attempt + 1, response.head.status});
            if (response.head.status == .unauthorized or response.head.status == .forbidden) {
                zig_sync_disconnect();
                return error.AuthenticationExpired;
            }
            if (is_transient_error(response.head.status) and attempt + 1 < max_attempts) {
                continue;
            }
            return error.DriveMetaFailed;
        }

        var transfer_buf: [4096]u8 = undefined;
        const rdr = response.reader(&transfer_buf);
        var read_buf: [4096]u8 = undefined;
        const read_bytes = rdr.readSliceShort(&read_buf) catch |err| {
            std.debug.print("Get time attempt {} failed to read response: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };

        const DriveMetaResponse = struct {
            modifiedTime: []const u8,
        };
        const parsed = std.json.parseFromSlice(DriveMetaResponse, allocator, read_buf[0..read_bytes], .{ .ignore_unknown_fields = true }) catch |err| {
            std.debug.print("Get time attempt {} failed to parse JSON: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };
        defer parsed.deinit();

        return try parse_iso8601_to_unix(parsed.value.modifiedTime);
    }
    return error.DriveMetaFailed;
}

fn update_local_metadata(allocator: std.mem.Allocator, filename: []const u8, drive_id: []const u8, last_modified: i64) !void {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(dbp(), &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return error.DbOpenFailed;
    }
    defer _ = c.sqlite3_close(db);
    _ = c.sqlite3_busy_timeout(db, 5000);

    const filename_z = try allocator.dupeZ(u8, filename);
    defer allocator.free(filename_z);
    const drive_id_z = try allocator.dupeZ(u8, drive_id);
    defer allocator.free(drive_id_z);

    const sql = "INSERT OR REPLACE INTO file_metadata (filepath, last_modified, drive_file_id) VALUES (?, ?, ?);";
    var stmt: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) != c.SQLITE_OK) return error.DbPrepareFailed;
    defer _ = c.sqlite3_finalize(stmt);

    _ = c.sqlite3_bind_text(stmt, 1, filename_z.ptr, -1, null);
    _ = c.sqlite3_bind_int64(stmt, 2, last_modified);
    _ = c.sqlite3_bind_text(stmt, 3, drive_id_z.ptr, -1, null);

    _ = c.sqlite3_step(stmt);
}

fn local_sync_dir_path(allocator: std.mem.Allocator) ![]u8 {
    if (c.getenv("QIRTAS_LOCAL_SYNC_DIR")) |env_path| {
        const path = std.mem.span(env_path);
        if (path.len > 0) return try allocator.dupe(u8, path);
    }

    if (c.getenv("HOME")) |home| {
        return try std.fmt.allocPrint(allocator, "{s}/QirtasSync", .{std.mem.span(home)});
    }

    return try allocator.dupe(u8, "/tmp/QirtasSync");
}

fn ensure_directory(path: []const u8) !void {
    const path_z = try std.heap.page_allocator.dupeZ(u8, path);
    defer std.heap.page_allocator.free(path_z);

    var st: c.struct_stat = undefined;
    if (c.stat(path_z.ptr, &st) == 0) {
        if ((st.st_mode & c.S_IFMT) == c.S_IFDIR) return;
        return error.SyncTargetIsNotDirectory;
    }

    if (c.mkdir(path_z.ptr, @as(c.mode_t, 0o700)) < 0) return error.CreateSyncDirectoryFailed;
}

fn join_path(allocator: std.mem.Allocator, dir: []const u8, filename: []const u8) ![]u8 {
    if (dir.len > 0 and dir[dir.len - 1] == '/') {
        return try std.fmt.allocPrint(allocator, "{s}{s}", .{ dir, filename });
    }
    return try std.fmt.allocPrint(allocator, "{s}/{s}", .{ dir, filename });
}

fn stat_path(path: []const u8) ?c.struct_stat {
    const path_z = std.heap.page_allocator.dupeZ(u8, path) catch return null;
    defer std.heap.page_allocator.free(path_z);

    var st: c.struct_stat = undefined;
    if (c.stat(path_z.ptr, &st) != 0) return null;
    return st;
}

fn copy_file_path(allocator: std.mem.Allocator, source_path: []const u8, dest_path: []const u8) !void {
    const content = try read_file_content(allocator, source_path);
    defer allocator.free(content);
    try write_file_mmap(dest_path, content);
}

pub export fn zig_local_sync_now() callconv(.c) void {
    gui_trigger_autosave();
    gui_update_local_sync_status(2, "Syncing...");
    _ = std.Thread.spawn(.{}, local_sync_worker, .{}) catch {
        gui_update_local_sync_status(0, "Error: local sync start failed.");
    };
}

fn local_sync_worker() void {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();

    local_sync_impl(arena.allocator()) catch |err| {
        std.debug.print("Local sync failed: {}\n", .{err});
        var msg_buf: [128]u8 = undefined;
        gui_update_local_sync_status(0, sync_error_message_z(err, &msg_buf));
        return;
    };

    gui_refresh_explorer();
    gui_update_local_sync_status(1, "~/QirtasSync");
}

fn local_sync_impl(allocator: std.mem.Allocator) !void {
    const target_dir = try local_sync_dir_path(allocator);
    try ensure_directory(target_dir);

    const local_dir = c.opendir(".");
    if (local_dir == null) return error.OpenDirFailed;
    defer _ = c.closedir(local_dir);

    while (true) {
        const entry = c.readdir(local_dir);
        if (entry == null) break;
        const name = std.mem.span(@as([*:0]const u8, @ptrCast(&entry.*.d_name)));
        if (!is_syncable_file(name)) continue;

        const target_path = try join_path(allocator, target_dir, name);
        const local_stat = stat_path(name) orelse continue;
        const target_stat = stat_path(target_path);

        if (target_stat) |remote_stat| {
            if (local_stat.st_mtim.tv_sec > remote_stat.st_mtim.tv_sec) {
                try copy_file_path(allocator, name, target_path);
            } else if (remote_stat.st_mtim.tv_sec > local_stat.st_mtim.tv_sec) {
                try copy_file_path(allocator, target_path, name);
                const name_z = try allocator.dupeZ(u8, name);
                gui_index_file(name_z.ptr);
            }
        } else {
            try copy_file_path(allocator, name, target_path);
        }
    }

    const remote_dir_z = try allocator.dupeZ(u8, target_dir);
    const remote_dir = c.opendir(remote_dir_z.ptr);
    if (remote_dir == null) return error.OpenDirFailed;
    defer _ = c.closedir(remote_dir);

    while (true) {
        const entry = c.readdir(remote_dir);
        if (entry == null) break;
        const name = std.mem.span(@as([*:0]const u8, @ptrCast(&entry.*.d_name)));
        if (!is_syncable_file(name)) continue;

        if (stat_path(name) == null) {
            const source_path = try join_path(allocator, target_dir, name);
            try copy_file_path(allocator, source_path, name);
            const name_z = try allocator.dupeZ(u8, name);
            gui_index_file(name_z.ptr);
        }
    }
}

// FFI: Trigger manual backup sync
pub export fn zig_sync_now() callconv(.c) void {
    gui_trigger_autosave();
    const allocator = std.heap.page_allocator;

    if (zig_sync_check_status() == 1) {
        gui_update_sync_status(2, "Syncing...");
        _ = std.Thread.spawn(.{}, sync_now_worker, .{allocator}) catch {
            gui_update_sync_status(1, "Connected");
        };
    }

    if (zig_dropbox_check_status() == 1) {
        zig_dropbox_now();
    }

    if (zig_github_check_status() == 1) {
        zig_github_now();
    }
}

fn sync_now_worker(allocator: std.mem.Allocator) void {
    _ = allocator;
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();

    sync_now_impl(arena.allocator()) catch |err| {
        std.debug.print("Sync failed: {}\n", .{err});
        var msg_buf: [128]u8 = undefined;
        gui_update_sync_status(sync_error_connection_state(err), sync_error_message_z(err, &msg_buf));
        return;
    };
    
    gui_refresh_explorer();
    gui_update_sync_status(1, "Connected");
}

fn sync_now_impl(allocator: std.mem.Allocator) anyerror!void {
    var creds = try get_credentials(allocator);
    defer free_credentials(&creds, allocator);

    const access_token = try refresh_token_if_needed(allocator, &creds);
    defer allocator.free(access_token);

    var body_list: std.ArrayList(u8) = .empty;
    defer body_list.deinit(allocator);

    var attempt: u32 = 0;
    const max_attempts = 3;
    var list_success = false;

    while (attempt < max_attempts) : (attempt += 1) {
        if (attempt > 0) {
            _ = c.usleep(attempt * 1000 * 1000);
        }

        var client = std.http.Client{ .allocator = allocator, .io = main.global_io };
        defer client.deinit();

        const uri_search = std.Uri.parse("https://www.googleapis.com/drive/v3/files?q=%27appDataFolder%27+in+parents+and+trashed+%3D+false&spaces=appDataFolder&fields=files(id,name,modifiedTime)") catch |e| return e;
        const auth_hdr = std.fmt.allocPrint(allocator, "Bearer {s}", .{access_token}) catch |e| return e;
        defer allocator.free(auth_hdr);

        var req_search = client.request(.GET, uri_search, .{
            .extra_headers = &[_]std.http.Header{
                .{ .name = "Authorization", .value = auth_hdr },
            },
        }) catch |err| {
            std.debug.print("Search attempt {} failed to send request: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };
        defer req_search.deinit();

        req_search.sendBodiless() catch |err| {
            std.debug.print("Search attempt {} failed to send body: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };

        var redirect_buf: [1024]u8 = undefined;
        var response_search = req_search.receiveHead(&redirect_buf) catch |err| {
            std.debug.print("Search attempt {} failed to receive head: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };

        if (response_search.head.status != .ok) {
            std.debug.print("Search attempt {} failed with HTTP status: {}\n", .{attempt + 1, response_search.head.status});
            if (response_search.head.status == .unauthorized or response_search.head.status == .forbidden) {
                zig_sync_disconnect();
                return error.AuthenticationExpired;
            }
            if (is_transient_error(response_search.head.status) and attempt + 1 < max_attempts) {
                continue;
            }
            return error.DriveListFailed;
        }

        body_list.clearRetainingCapacity();
        var transfer_buf: [4096]u8 = undefined;
        const rdr = response_search.reader(&transfer_buf);
        var read_success = true;
        while (true) {
            var read_buf: [4096]u8 = undefined;
            const read_bytes = rdr.readSliceShort(&read_buf) catch |err| {
                if (err == error.EndOfStream) break;
                read_success = false;
                break;
            };
            if (read_bytes == 0) break;
            body_list.appendSlice(allocator, read_buf[0..read_bytes]) catch |err| {
                return err;
            };
        }

        if (!read_success) {
            std.debug.print("Search attempt {} failed to read stream\n", .{attempt + 1});
            if (attempt + 1 == max_attempts) return error.DriveListFailed;
            continue;
        }

        list_success = true;
        break;
    }

    if (!list_success) return error.DriveListFailed;

    const parsed = try std.json.parseFromSlice(DriveSearchResponse, allocator, body_list.items, .{ .ignore_unknown_fields = true });
    defer parsed.deinit();

    const CloudFileMeta = struct {
        id: []const u8,
        modifiedTime: []const u8,
    };
    var cloud_map = std.StringHashMap(CloudFileMeta).init(allocator);
    defer cloud_map.deinit();

    for (parsed.value.files) |file| {
        try cloud_map.put(file.name, .{ .id = file.id, .modifiedTime = file.modifiedTime });
    }

    var local_map = std.StringHashMap(c.struct_stat).init(allocator);
    defer local_map.deinit();

    const dir_handle = c.opendir(".");
    if (dir_handle == null) return error.OpenDirFailed;
    defer _ = c.closedir(dir_handle);

    while (true) {
        const entry = c.readdir(dir_handle);
        if (entry == null) break;
        const name = std.mem.span(@as([*:0]const u8, @ptrCast(&entry.*.d_name)));
        if (is_syncable_file(name)) {
            const name_z = try allocator.dupeZ(u8, name);
            defer allocator.free(name_z);
            var st: c.struct_stat = undefined;
            if (c.stat(name_z.ptr, &st) == 0) {
                const key = try allocator.dupe(u8, name);
                try local_map.put(key, st);
            }
        }
    }

    var local_it = local_map.iterator();
    while (local_it.next()) |entry| {
        const filename = entry.key_ptr.*;
        const local_st = entry.value_ptr.*;
        const local_mtime = local_st.st_mtim.tv_sec;

        if (cloud_map.get(filename)) |cloud_meta| {
            var db_last_modified: i64 = 0;
            var saved_drive_id: ?[]const u8 = null;
            
            {
                var db: ?*c.sqlite3 = null;
                if (c.sqlite3_open(dbp(), &db) == c.SQLITE_OK) {
                    _ = c.sqlite3_busy_timeout(db, 5000);
                    defer _ = c.sqlite3_close(db);
                    const sql_check = "SELECT last_modified, drive_file_id FROM file_metadata WHERE filepath = ?;";
                    var stmt_check: ?*c.sqlite3_stmt = null;
                    const filename_z = try allocator.dupeZ(u8, filename);
                    defer allocator.free(filename_z);
                    if (c.sqlite3_prepare_v2(db, sql_check.ptr, -1, &stmt_check, null) == c.SQLITE_OK) {
                        _ = c.sqlite3_bind_text(stmt_check, 1, filename_z.ptr, -1, null);
                        if (c.sqlite3_step(stmt_check) == c.SQLITE_ROW) {
                            db_last_modified = c.sqlite3_column_int64(stmt_check, 0);
                            const c_id = c.sqlite3_column_text(stmt_check, 1);
                            if (c_id != null) {
                                saved_drive_id = try allocator.dupe(u8, std.mem.span(c_id));
                            }
                        }
                        _ = c.sqlite3_finalize(stmt_check);
                    }
                } else {
                    if (db != null) _ = c.sqlite3_close(db);
                }
            }
            defer if (saved_drive_id) |id| allocator.free(id);

            const cloud_time = parse_iso8601_to_unix(cloud_meta.modifiedTime) catch |err| blk: {
                std.debug.print("Failed to parse cloud time {s}: {}\n", .{cloud_meta.modifiedTime, err});
                break :blk @as(i64, 0);
            };

            const local_changed = (local_mtime != db_last_modified);
            const cloud_changed = (cloud_time != db_last_modified);

            if (local_changed and cloud_changed) {
                std.debug.print("Conflict: {s}\n", .{filename});
                const conflict_name = try make_conflict_filename(allocator, filename);
                defer allocator.free(conflict_name);
                
                const local_content = try read_file_content(allocator, filename);
                defer allocator.free(local_content);

                try write_file_mmap(conflict_name, local_content);
                const conflict_name_z = try allocator.dupeZ(u8, conflict_name);
                defer allocator.free(conflict_name_z);
                gui_index_file(conflict_name_z.ptr);

                const cloud_content = try download_file_from_drive(allocator, access_token, cloud_meta.id);
                defer allocator.free(cloud_content);
                try write_file_mmap(filename, cloud_content);

                try update_local_metadata(allocator, filename, cloud_meta.id, cloud_time);
                main.alert_file_updated();
            } else if (local_changed) {
                std.debug.print("Local changed: patching {s}\n", .{filename});
                const local_content = try read_file_content(allocator, filename);
                defer allocator.free(local_content);
                try patch_file_on_drive(allocator, access_token, cloud_meta.id, local_content);
                
                try update_local_metadata(allocator, filename, cloud_meta.id, local_mtime);
            } else if (cloud_changed) {
                std.debug.print("Cloud changed: downloading {s}\n", .{filename});
                const cloud_content = try download_file_from_drive(allocator, access_token, cloud_meta.id);
                defer allocator.free(cloud_content);
                try write_file_mmap(filename, cloud_content);

                try update_local_metadata(allocator, filename, cloud_meta.id, cloud_time);
                main.alert_file_updated();
            } else {
                if (saved_drive_id == null) {
                    try update_local_metadata(allocator, filename, cloud_meta.id, local_mtime);
                }
            }
        } else {
            std.debug.print("Local only: uploading {s}\n", .{filename});
            const local_content = try read_file_content(allocator, filename);
            defer allocator.free(local_content);
            const drive_id_new = try upload_file_to_drive(allocator, access_token, filename, local_content);
            defer allocator.free(drive_id_new);

            const new_cloud_time = try get_file_modified_time_from_drive(allocator, access_token, drive_id_new);
            try update_local_metadata(allocator, filename, drive_id_new, new_cloud_time);
        }
    }

    var cloud_it = cloud_map.iterator();
    while (cloud_it.next()) |entry| {
        const filename = entry.key_ptr.*;
        const cloud_meta = entry.value_ptr.*;

        if (!local_map.contains(filename)) {
            std.debug.print("Cloud only: downloading {s}\n", .{filename});
            const cloud_content = try download_file_from_drive(allocator, access_token, cloud_meta.id);
            defer allocator.free(cloud_content);

            try write_file_mmap(filename, cloud_content);

            const cloud_time = parse_iso8601_to_unix(cloud_meta.modifiedTime) catch @as(i64, 0);
            try update_local_metadata(allocator, filename, cloud_meta.id, cloud_time);

            const filename_z = try allocator.dupeZ(u8, filename);
            defer allocator.free(filename_z);
            gui_index_file(filename_z.ptr);
        }
    }
}

// FFI: Check dynamic status on startup
pub export fn zig_sync_check_status() callconv(.c) c_int {
    const allocator = std.heap.page_allocator;
    var creds = get_credentials(allocator) catch return 0;
    defer free_credentials(&creds, allocator);
    if (creds.access_token != null and creds.access_token.?.len > 0) return 1;
    return 0;
}

// FFI: Disconnect account
pub export fn zig_sync_disconnect() callconv(.c) void {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(dbp(), &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return;
    }
    defer _ = c.sqlite3_close(db);
    _ = c.sqlite3_busy_timeout(db, 5000);

    const sql = "UPDATE sync_tokens SET access_token = NULL, refresh_token = NULL, expiry_time = 0 WHERE id = 1;";
    _ = c.sqlite3_exec(db, sql.ptr, null, null, null);
    
    _ = c.sqlite3_exec(db, "UPDATE file_metadata SET drive_file_id = NULL;", null, null, null);

    gui_update_sync_status(0, "Disconnected");
}

// --- CRYPTOGRAPHIC ENCRYPTION HELPERS ---

fn charToDigit(char: u8) !u8 {
    return switch (char) {
        '0'...'9' => char - '0',
        'a'...'f' => char - 'a' + 10,
        'A'...'F' => char - 'A' + 10,
        else => error.InvalidHexChar,
    };
}

fn bytesToHexAlloc(allocator: std.mem.Allocator, bytes: []const u8) ![]u8 {
    const hex_chars = "0123456789abcdef";
    const out = try allocator.alloc(u8, bytes.len * 2);
    for (bytes, 0..) |b, i| {
        out[i * 2] = hex_chars[b >> 4];
        out[i * 2 + 1] = hex_chars[b & 0x0f];
    }
    return out;
}

fn hexToBytesAlloc(allocator: std.mem.Allocator, hex: []const u8) ![]u8 {
    if (hex.len % 2 != 0) return error.InvalidHexLength;
    const out = try allocator.alloc(u8, hex.len / 2);
    errdefer allocator.free(out);
    var i: usize = 0;
    while (i < hex.len) : (i += 2) {
        const h1 = try charToDigit(hex[i]);
        const h2 = try charToDigit(hex[i+1]);
        out[i / 2] = (h1 << 4) | h2;
    }
    return out;
}

fn deriveKey(key: *[32]u8) !void {
    const fd = c.open("/etc/machine-id", c.O_RDONLY);
    if (fd < 0) return error.OpenMachineIdFailed;
    defer _ = c.close(fd);
    
    var buf: [128]u8 = undefined;
    const n = c.read(fd, &buf, buf.len);
    if (n < 0) return error.ReadMachineIdFailed;
    
    const id = std.mem.trim(u8, buf[0..@intCast(n)], " \r\n");
    std.crypto.hash.sha2.Sha256.hash(id, key, .{});
}

pub fn encryptToken(allocator: std.mem.Allocator, raw_token: []const u8) ![]u8 {
    var key: [32]u8 = undefined;
    try deriveKey(&key);
    
    var nonce: [12]u8 = undefined;
    const fd = c.open("/dev/urandom", c.O_RDONLY);
    if (fd < 0) return error.OpenUrandomFailed;
    defer _ = c.close(fd);
    const n = c.read(fd, &nonce, nonce.len);
    if (n < @as(isize, @intCast(nonce.len))) return error.ReadUrandomFailed;
    
    const cipher = std.crypto.aead.chacha_poly.ChaCha20Poly1305;
    
    const ct_len = raw_token.len;
    const raw_len = 12 + ct_len + 16; // nonce + ciphertext + tag
    
    const raw_buf = try allocator.alloc(u8, raw_len);
    defer allocator.free(raw_buf);
    
    @memcpy(raw_buf[0..12], &nonce);
    
    var tag: [16]u8 = undefined;
    cipher.encrypt(raw_buf[12..12+ct_len], &tag, raw_token, "", nonce, key);
    @memcpy(raw_buf[12+ct_len..], &tag);
    
    return try bytesToHexAlloc(allocator, raw_buf);
}

pub fn decryptToken(allocator: std.mem.Allocator, hex_token: []const u8) ![]u8 {
    var key: [32]u8 = undefined;
    try deriveKey(&key);
    
    const raw_buf = try hexToBytesAlloc(allocator, hex_token);
    defer allocator.free(raw_buf);
    
    if (raw_buf.len < 28) return error.InvalidEncryptedTokenLength;
    
    var nonce: [12]u8 = undefined;
    @memcpy(&nonce, raw_buf[0..12]);
    
    var tag: [16]u8 = undefined;
    const ct_len = raw_buf.len - 12 - 16;
    @memcpy(&tag, raw_buf[12+ct_len..]);
    
    const cipher = std.crypto.aead.chacha_poly.ChaCha20Poly1305;
    
    const pt_buf = try allocator.alloc(u8, ct_len);
    errdefer allocator.free(pt_buf);
    
    try cipher.decrypt(pt_buf, raw_buf[12..12+ct_len], tag, "", nonce, key);
    
    return pt_buf;
}

pub export fn zig_save_dropbox_credentials(client_id_ptr: [*:0]const u8, client_secret_ptr: [*:0]const u8) callconv(.c) void {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(dbp(), &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return;
    }
    defer _ = c.sqlite3_close(db);
    _ = c.sqlite3_busy_timeout(db, 5000);

    const allocator = std.heap.page_allocator;
    const client_id = std.mem.span(client_id_ptr);
    const client_secret = std.mem.span(client_secret_ptr);

    const encrypted = if (client_secret.len == 0)
        allocator.dupe(u8, "") catch return
    else
        encryptToken(allocator, client_secret) catch return;
    defer allocator.free(encrypted);

    const sql = "INSERT OR REPLACE INTO dropbox_sync_tokens (id, client_id, client_secret) VALUES (1, ?, ?);";
    var stmt: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
        const encrypted_z = allocator.dupeZ(u8, encrypted) catch return;
        defer allocator.free(encrypted_z);
        const client_id_z = allocator.dupeZ(u8, client_id) catch return;
        defer allocator.free(client_id_z);

        _ = c.sqlite3_bind_text(stmt, 1, client_id_z.ptr, -1, null);
        _ = c.sqlite3_bind_text(stmt, 2, encrypted_z.ptr, -1, null);
        _ = c.sqlite3_step(stmt);
        _ = c.sqlite3_finalize(stmt);
    }
}

const DropboxCredentials = struct {
    client_id: []const u8,
    client_secret: []const u8,
    access_token: ?[]const u8 = null,
    refresh_token: ?[]const u8 = null,
    expiry_time: i64 = 0,
};

fn get_dropbox_credentials(allocator: std.mem.Allocator) !DropboxCredentials {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(dbp(), &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return error.DbOpenFailed;
    }
    defer _ = c.sqlite3_close(db);
    _ = c.sqlite3_busy_timeout(db, 5000);

    const sql = "SELECT client_id, client_secret, access_token, refresh_token, expiry_time FROM dropbox_sync_tokens WHERE id = 1;";
    var stmt: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) != c.SQLITE_OK) return error.DbPrepareFailed;
    defer _ = c.sqlite3_finalize(stmt);

    if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
        const c_id = c.sqlite3_column_text(stmt, 0);
        const c_sec = c.sqlite3_column_text(stmt, 1);
        const acc_tok = c.sqlite3_column_text(stmt, 2);
        const ref_tok = c.sqlite3_column_text(stmt, 3);
        const exp_time = c.sqlite3_column_int64(stmt, 4);

        if (c_id == null or c_sec == null) return error.CredentialsNotFound;

        const encrypted_sec = std.mem.span(c_sec);
        const decrypted_sec = if (encrypted_sec.len == 0)
            try allocator.dupe(u8, "")
        else
            try decryptToken(allocator, encrypted_sec);
        errdefer allocator.free(decrypted_sec);

        const decrypted_access = if (acc_tok != null and std.mem.span(acc_tok).len > 0)
            decryptToken(allocator, std.mem.span(acc_tok)) catch null
        else null;
        const decrypted_refresh = if (ref_tok != null and std.mem.span(ref_tok).len > 0)
            decryptToken(allocator, std.mem.span(ref_tok)) catch null
        else null;

        return DropboxCredentials{
            .client_id = try allocator.dupe(u8, std.mem.span(c_id)),
            .client_secret = decrypted_sec,
            .access_token = decrypted_access,
            .refresh_token = decrypted_refresh,
            .expiry_time = exp_time,
        };
    }

    return error.CredentialsNotFound;
}

fn free_dropbox_credentials(creds: *DropboxCredentials, allocator: std.mem.Allocator) void {
    allocator.free(creds.client_id);
    @memset(@constCast(creds.client_secret), 0);
    allocator.free(creds.client_secret);
    if (creds.access_token) |t| {
        @memset(@constCast(t), 0);
        allocator.free(t);
    }
    if (creds.refresh_token) |t| {
        @memset(@constCast(t), 0);
        allocator.free(t);
    }
}

pub export fn zig_dropbox_connect() callconv(.c) void {
    const allocator = std.heap.page_allocator;
    var creds = get_dropbox_credentials(allocator) catch {
        gui_update_dropbox_status(0, "Configure Dropbox app key");
        return;
    };
    defer free_dropbox_credentials(&creds, allocator);

    const url = std.fmt.allocPrint(allocator, 
       "https://www.dropbox.com/oauth2/authorize?client_id={s}&token_access_type=offline&response_type=code&redirect_uri=http://localhost:5173",
       .{creds.client_id}
     ) catch return;
    defer allocator.free(url);

    var child = std.process.spawn(main.global_io, .{
        .argv = &[_][]const u8{ "xdg-open", url },
    }) catch blk: {
        break :blk std.process.spawn(main.global_io, .{
            .argv = &[_][]const u8{ "gio", "open", url },
        }) catch return;
    };
    _ = child.wait(main.global_io) catch {};

    gui_update_dropbox_status(2, "Enter code below...");
}

pub export fn zig_dropbox_submit_code(code_ptr: [*:0]const u8) callconv(.c) void {
    const code = std.mem.span(code_ptr);
    const allocator = std.heap.page_allocator;

    var creds = get_dropbox_credentials(allocator) catch return;
    defer free_dropbox_credentials(&creds, allocator);

    gui_update_dropbox_status(2, "Exchanging code...");

    const code_dup = allocator.dupe(u8, code) catch return;
    const client_id_dup = allocator.dupe(u8, creds.client_id) catch {
        allocator.free(code_dup);
        return;
    };
    const client_secret_dup = allocator.dupe(u8, creds.client_secret) catch {
        allocator.free(code_dup);
        allocator.free(client_id_dup);
        return;
    };

    _ = std.Thread.spawn(.{}, exchange_dropbox_token_worker, .{
        allocator,
        code_dup,
        client_id_dup,
        client_secret_dup,
    }) catch {
        allocator.free(code_dup);
        allocator.free(client_id_dup);
        allocator.free(client_secret_dup);
        gui_update_dropbox_status(0, "Disconnected");
    };
}

fn exchange_dropbox_token_worker(allocator: std.mem.Allocator, code: []const u8, client_id: []const u8, client_secret: []const u8) void {
    defer allocator.free(code);
    defer allocator.free(client_id);
    defer allocator.free(client_secret);

    exchange_dropbox_token_impl(allocator, code, client_id, client_secret) catch |err| {
        std.debug.print("Dropbox Token exchange failed: {}\n", .{err});
        var msg_buf: [128]u8 = undefined;
        gui_update_dropbox_status(sync_error_connection_state(err), sync_error_message_z(err, &msg_buf));
        return;
    };

    gui_update_dropbox_status(1, "Connected");
}

fn exchange_dropbox_token_impl(allocator: std.mem.Allocator, code: []const u8, client_id: []const u8, client_secret: []const u8) !void {
    var client = std.http.Client{ .allocator = allocator, .io = main.global_io };
    defer client.deinit();

    const uri = try std.Uri.parse("https://api.dropboxapi.com/oauth2/token");
    
    var req = try client.request(.POST, uri, .{
        .extra_headers = &[_]std.http.Header{
            .{ .name = "Content-Type", .value = "application/x-www-form-urlencoded" },
        },
    });
    defer req.deinit();

    const body = if (client_secret.len > 0)
        try std.fmt.allocPrint(allocator,
            "code={s}&client_id={s}&client_secret={s}&redirect_uri=http://localhost:5173&grant_type=authorization_code",
            .{ code, client_id, client_secret },
        )
    else
        try std.fmt.allocPrint(allocator,
            "code={s}&client_id={s}&redirect_uri=http://localhost:5173&grant_type=authorization_code",
            .{ code, client_id },
        );
    defer allocator.free(body);

    try req.sendBodyComplete(body);

    var redirect_buf: [1024]u8 = undefined;
    var response = try req.receiveHead(&redirect_buf);
    
    if (response.head.status != .ok) return error.TokenExchangeFailed;

    var transfer_buf: [4096]u8 = undefined;
    const rdr = response.reader(&transfer_buf);

    var token_json_list: std.ArrayList(u8) = .empty;
    defer token_json_list.deinit(allocator);

    while (true) {
        var chunk_buf: [4096]u8 = undefined;
        const read_bytes = rdr.readSliceShort(&chunk_buf) catch |err| {
            if (err == error.EndOfStream) break;
            return err;
        };
        if (read_bytes == 0) break;
        try token_json_list.appendSlice(allocator, chunk_buf[0..read_bytes]);
    }

    const parsed = try std.json.parseFromSlice(TokenResponse, allocator, token_json_list.items, .{ .ignore_unknown_fields = true });
    defer parsed.deinit();

    const expiry = @as(i64, @intCast(c.time(null))) + parsed.value.expires_in;

    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(dbp(), &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return error.DbOpenFailed;
    }
    defer _ = c.sqlite3_close(db);
    _ = c.sqlite3_busy_timeout(db, 5000);

    const sql = "UPDATE dropbox_sync_tokens SET access_token = ?, refresh_token = ?, expiry_time = ? WHERE id = 1;";
    var stmt: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
        const enc_access = try encryptToken(allocator, parsed.value.access_token);
        defer allocator.free(enc_access);
        const access_token_z = try allocator.dupeZ(u8, enc_access);
        defer allocator.free(access_token_z);

        const enc_refresh = if (parsed.value.refresh_token) |rt| try encryptToken(allocator, rt) else null;
        defer if (enc_refresh) |er| allocator.free(er);
        const refresh_token_z = if (enc_refresh) |er| try allocator.dupeZ(u8, er) else null;
        defer if (refresh_token_z) |erz| allocator.free(erz);

        _ = c.sqlite3_bind_text(stmt, 1, access_token_z.ptr, -1, null);
        if (refresh_token_z) |erz| {
            _ = c.sqlite3_bind_text(stmt, 2, erz.ptr, -1, null);
        } else {
            _ = c.sqlite3_bind_text(stmt, 2, "", -1, null);
        }
        _ = c.sqlite3_bind_int64(stmt, 3, expiry);
        _ = c.sqlite3_step(stmt);
        _ = c.sqlite3_finalize(stmt);
    }
}

fn refresh_dropbox_token_if_needed(allocator: std.mem.Allocator, creds: *DropboxCredentials) ![]const u8 {
    const current_time = @as(i64, @intCast(c.time(null)));
    if (creds.access_token != null and creds.expiry_time > current_time + 300) {
        return try allocator.dupe(u8, creds.access_token.?);
    }

    if (creds.refresh_token == null or creds.refresh_token.?.len == 0) return error.NoRefreshToken;

    var client = std.http.Client{ .allocator = allocator, .io = main.global_io };
    defer client.deinit();

    const uri = try std.Uri.parse("https://api.dropboxapi.com/oauth2/token");
    var req = try client.request(.POST, uri, .{
        .extra_headers = &[_]std.http.Header{
            .{ .name = "Content-Type", .value = "application/x-www-form-urlencoded" },
        },
    });
    defer req.deinit();

    const body = if (creds.client_secret.len > 0)
        try std.fmt.allocPrint(allocator,
            "refresh_token={s}&client_id={s}&client_secret={s}&grant_type=refresh_token",
            .{ creds.refresh_token.?, creds.client_id, creds.client_secret },
        )
    else
        try std.fmt.allocPrint(allocator,
            "refresh_token={s}&client_id={s}&grant_type=refresh_token",
            .{ creds.refresh_token.?, creds.client_id },
        );
    defer allocator.free(body);

    try req.sendBodyComplete(body);

    var redirect_buf: [1024]u8 = undefined;
    var response = try req.receiveHead(&redirect_buf);

    if (response.head.status != .ok) return error.TokenRefreshFailed;

    var transfer_buf: [4096]u8 = undefined;
    const rdr = response.reader(&transfer_buf);

    var refresh_json_list: std.ArrayList(u8) = .empty;
    defer refresh_json_list.deinit(allocator);

    while (true) {
        var chunk_buf: [4096]u8 = undefined;
        const read_bytes = rdr.readSliceShort(&chunk_buf) catch |err| {
            if (err == error.EndOfStream) break;
            return err;
        };
        if (read_bytes == 0) break;
        try refresh_json_list.appendSlice(allocator, chunk_buf[0..read_bytes]);
    }

    const parsed = try std.json.parseFromSlice(RefreshResponse, allocator, refresh_json_list.items, .{ .ignore_unknown_fields = true });
    defer parsed.deinit();

    const expiry = @as(i64, @intCast(c.time(null))) + parsed.value.expires_in;

    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(dbp(), &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return error.DbOpenFailed;
    }
    defer _ = c.sqlite3_close(db);
    _ = c.sqlite3_busy_timeout(db, 5000);

    const sql = "UPDATE dropbox_sync_tokens SET access_token = ?, expiry_time = ? WHERE id = 1;";
    var stmt: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
        const enc_access = try encryptToken(allocator, parsed.value.access_token);
        defer allocator.free(enc_access);
        const access_token_z = try allocator.dupeZ(u8, enc_access);
        defer allocator.free(access_token_z);

        _ = c.sqlite3_bind_text(stmt, 1, access_token_z.ptr, -1, null);
        _ = c.sqlite3_bind_int64(stmt, 2, expiry);
        _ = c.sqlite3_step(stmt);
        _ = c.sqlite3_finalize(stmt);
    }

    return try allocator.dupe(u8, parsed.value.access_token);
}

pub export fn zig_dropbox_check_status() callconv(.c) c_int {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(dbp(), &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return 0;
    }
    defer _ = c.sqlite3_close(db);
    _ = c.sqlite3_busy_timeout(db, 5000);

    const sql = "SELECT access_token FROM dropbox_sync_tokens WHERE id = 1;";
    var stmt: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
        defer _ = c.sqlite3_finalize(stmt);
        if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
            const acc_tok = c.sqlite3_column_text(stmt, 0);
            if (acc_tok != null and std.mem.span(acc_tok).len > 0) return 1;
        }
    }
    return 0;
}

pub export fn zig_dropbox_disconnect() callconv(.c) void {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(dbp(), &db) == c.SQLITE_OK) {
        defer _ = c.sqlite3_close(db);
        _ = c.sqlite3_busy_timeout(db, 5000);
        _ = c.sqlite3_exec(db, "UPDATE dropbox_sync_tokens SET access_token = NULL, refresh_token = NULL, expiry_time = 0 WHERE id = 1;", null, null, null);
    }
    gui_update_dropbox_status(0, "Disconnected");
}

pub export fn zig_dropbox_now() callconv(.c) void {
    gui_trigger_autosave();
    gui_update_dropbox_status(2, "Syncing...");
    _ = std.Thread.spawn(.{}, dropbox_sync_worker, .{}) catch {
        gui_update_dropbox_status(1, "Connected");
    };
}

fn dropbox_sync_worker() void {
    const allocator = std.heap.page_allocator;
    var creds = get_dropbox_credentials(allocator) catch {
        gui_update_dropbox_status(0, "Error: missing credentials.");
        return;
    };
    defer free_dropbox_credentials(&creds, allocator);

    const token = refresh_dropbox_token_if_needed(allocator, &creds) catch {
        gui_update_dropbox_status(0, "Error: token refresh failed.");
        return;
    };
    defer {
        @memset(@constCast(token), 0);
        allocator.free(token);
    }

    var script_buf: [600]u8 = undefined;
    const script = std.fmt.bufPrintZ(&script_buf, "{s}/dropbox_sync.sh", .{main.configDir()}) catch {
        gui_update_dropbox_status(1, "Error: Dropbox sync start failed.");
        return;
    };
    if (c.access(script.ptr, c.X_OK) != 0) {
        gui_update_dropbox_status(1, "Error: dropbox_sync.sh missing.");
        return;
    }

    var child = std.process.spawn(main.global_io, .{
        .argv = &[_][]const u8{ script, token, "." },
    }) catch {
        gui_update_dropbox_status(1, "Error: Dropbox sync start failed.");
        return;
    };

    const term = child.wait(main.global_io) catch {
        gui_update_dropbox_status(1, "Error: Dropbox sync wait failed.");
        return;
    };

    @memset(@constCast(token), 0);

    switch (term) {
        .exited => |code| {
            if (code == 0) {
                gui_update_dropbox_status(1, "Synced ✓");
            } else {
                gui_update_dropbox_status(1, "Error: Dropbox sync failed.");
            }
        },
        else => {
            gui_update_dropbox_status(1, "Error: Dropbox sync failed.");
        },
    }
}

fn get_github_credentials(allocator: std.mem.Allocator) !struct { token: []const u8, repo: []const u8 } {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(dbp(), &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return error.DbOpenFailed;
    }
    defer _ = c.sqlite3_close(db);
    _ = c.sqlite3_busy_timeout(db, 5000);

    const sql = "SELECT personal_token, repo_name FROM github_sync_tokens WHERE id = 1;";
    var stmt: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) != c.SQLITE_OK) return error.DbPrepareFailed;
    defer _ = c.sqlite3_finalize(stmt);

    if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
        const tok = c.sqlite3_column_text(stmt, 0);
        const rep = c.sqlite3_column_text(stmt, 1);

        if (tok == null or rep == null) return error.CredentialsNotFound;

        const encrypted = std.mem.span(tok);
        const decrypted = try decryptToken(allocator, encrypted);
        errdefer allocator.free(decrypted);

        return .{
            .token = decrypted,
            .repo = try allocator.dupe(u8, std.mem.span(rep)),
        };
    }

    return error.CredentialsNotFound;
}

pub export fn zig_save_github_credentials(token_ptr: [*:0]const u8, repo_ptr: [*:0]const u8) callconv(.c) void {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(dbp(), &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return;
    }
    defer _ = c.sqlite3_close(db);
    _ = c.sqlite3_busy_timeout(db, 5000);

    const allocator = std.heap.page_allocator;
    const token = std.mem.span(token_ptr);
    const repo = std.mem.span(repo_ptr);

    const encrypted = encryptToken(allocator, token) catch return;
    defer allocator.free(encrypted);

    const sql = "INSERT OR REPLACE INTO github_sync_tokens (id, personal_token, repo_name) VALUES (1, ?, ?);";
    var stmt: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
        const encrypted_z = allocator.dupeZ(u8, encrypted) catch return;
        defer allocator.free(encrypted_z);
        const repo_z = allocator.dupeZ(u8, repo) catch return;
        defer allocator.free(repo_z);

        _ = c.sqlite3_bind_text(stmt, 1, encrypted_z.ptr, -1, null);
        _ = c.sqlite3_bind_text(stmt, 2, repo_z.ptr, -1, null);
        _ = c.sqlite3_step(stmt);
        _ = c.sqlite3_finalize(stmt);
    }
}

pub export fn zig_github_connect() callconv(.c) void {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(dbp(), &db) == c.SQLITE_OK) {
        defer _ = c.sqlite3_close(db);
        _ = c.sqlite3_busy_timeout(db, 5000);
        _ = c.sqlite3_exec(db, "UPDATE github_sync_tokens SET access_token = 'mock-github-token' WHERE id = 1;", null, null, null);
    }
    gui_update_github_status(1, "Connected");
    
    // Automatically trigger sync now to create the repo and push initial files
    // zig_github_now();
}

pub export fn zig_github_check_status() callconv(.c) c_int {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(dbp(), &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return 0;
    }
    defer _ = c.sqlite3_close(db);
    _ = c.sqlite3_busy_timeout(db, 5000);

    const sql = "SELECT personal_token FROM github_sync_tokens WHERE id = 1;";
    var stmt: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) == c.SQLITE_OK) {
        defer _ = c.sqlite3_finalize(stmt);
        if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
            const acc_tok = c.sqlite3_column_text(stmt, 0);
            if (acc_tok != null and std.mem.span(acc_tok).len > 0) return 1;
        }
    }
    return 0;
}

pub export fn zig_github_disconnect() callconv(.c) void {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(dbp(), &db) == c.SQLITE_OK) {
        defer _ = c.sqlite3_close(db);
        _ = c.sqlite3_busy_timeout(db, 5000);
        _ = c.sqlite3_exec(db, "UPDATE github_sync_tokens SET personal_token = NULL WHERE id = 1;", null, null, null);
    }
    gui_update_github_status(0, "Disconnected");
}

pub export fn zig_github_now() callconv(.c) void {
    gui_trigger_autosave();
    gui_update_github_status(2, "Syncing...");
    _ = std.Thread.spawn(.{}, github_sync_worker, .{}) catch {
        gui_update_github_status(1, "Connected");
    };
}

fn github_sync_worker() void {
    const allocator = std.heap.page_allocator;
    const creds = get_github_credentials(allocator) catch {
        gui_update_github_status(0, "Error: missing credentials.");
        return;
    };
    defer {
        @memset(@constCast(creds.token), 0);
        allocator.free(creds.token);
        allocator.free(creds.repo);
    }

    var script_buf: [600]u8 = undefined;
    const script = std.fmt.bufPrintZ(&script_buf, "{s}/github_sync.sh", .{main.configDir()}) catch {
        gui_update_github_status(1, "Error: GitHub sync start failed.");
        return;
    };
    if (c.access(script.ptr, c.X_OK) != 0) {
        gui_update_github_status(1, "Error: github_sync.sh missing.");
        return;
    }

    var child = std.process.spawn(main.global_io, .{
        .argv = &[_][]const u8{ script, creds.token, creds.repo, "." },
    }) catch {
        gui_update_github_status(1, "Error: GitHub sync start failed.");
        return;
    };

    const term = child.wait(main.global_io) catch {
        gui_update_github_status(1, "Error: GitHub sync wait failed.");
        return;
    };

    @memset(@constCast(creds.token), 0);

    switch (term) {
        .exited => |code| {
            if (code == 0) {
                gui_update_github_status(1, "Synced ✓");
            } else {
                gui_update_github_status(1, "Error: GitHub sync failed.");
            }
        },
        else => {
            gui_update_github_status(1, "Error: GitHub sync failed.");
        },
    }
}

pub export fn zig_get_github_credentials_decrypted(token_buf: [*]u8, token_buf_max: usize, repo_buf: [*]u8, repo_buf_max: usize) callconv(.c) c_int {
    const allocator = std.heap.page_allocator;
    const creds = get_github_credentials(allocator) catch return 0;
    defer {
        @memset(@constCast(creds.token), 0);
        allocator.free(creds.token);
        allocator.free(creds.repo);
    }

    if (creds.token.len >= token_buf_max or creds.repo.len >= repo_buf_max) return 0;

    @memcpy(token_buf[0..creds.token.len], creds.token);
    token_buf[creds.token.len] = 0;

    @memcpy(repo_buf[0..creds.repo.len], creds.repo);
    repo_buf[creds.repo.len] = 0;

    return 1;
}

pub export fn zig_get_dropbox_credentials_decrypted(client_id_buf: [*]u8, client_id_max: usize, client_secret_buf: [*]u8, client_secret_max: usize) callconv(.c) c_int {
    const allocator = std.heap.page_allocator;
    var creds = get_dropbox_credentials(allocator) catch return 0;
    defer free_dropbox_credentials(&creds, allocator);

    if (creds.client_id.len >= client_id_max or creds.client_secret.len >= client_secret_max) return 0;

    @memcpy(client_id_buf[0..creds.client_id.len], creds.client_id);
    client_id_buf[creds.client_id.len] = 0;

    @memcpy(client_secret_buf[0..creds.client_secret.len], creds.client_secret);
    client_secret_buf[creds.client_secret.len] = 0;

    return 1;
}

// ─────────────────────────────────────────────────────────────
// Tests (run with: zig build test)
// ─────────────────────────────────────────────────────────────

test "encryptToken/decryptToken round-trip" {
    const allocator = std.testing.allocator;
    const secret = "ya29.a0AfH6-test-token-with-arabic-قرطاس";
    const enc = try encryptToken(allocator, secret);
    defer allocator.free(enc);
    // ciphertext is hex and never equals plaintext
    try std.testing.expect(std.mem.indexOf(u8, enc, secret) == null);
    const dec = try decryptToken(allocator, enc);
    defer allocator.free(dec);
    try std.testing.expectEqualStrings(secret, dec);
}

test "decryptToken rejects tampered ciphertext" {
    const allocator = std.testing.allocator;
    const enc = try encryptToken(allocator, "sensitive");
    defer allocator.free(enc);
    const tampered = try allocator.dupe(u8, enc);
    defer allocator.free(tampered);
    // flip one hex nibble inside the ciphertext region (past the 24-char nonce)
    const i = 26;
    tampered[i] = if (tampered[i] == 'a') 'b' else 'a';
    try std.testing.expectError(error.AuthenticationFailed, decryptToken(allocator, tampered));
}

test "decryptToken rejects truncated input" {
    const allocator = std.testing.allocator;
    try std.testing.expectError(error.InvalidEncryptedTokenLength, decryptToken(allocator, "deadbeef"));
}

test "parse_iso8601_to_unix known timestamps" {
    try std.testing.expectEqual(@as(i64, 0), try parse_iso8601_to_unix("1970-01-01T00:00:00Z"));
    // 2026-06-12 00:00:00 UTC
    try std.testing.expectEqual(@as(i64, 1781222400), try parse_iso8601_to_unix("2026-06-12T00:00:00.000Z"));
    // leap day handled
    try std.testing.expectEqual(@as(i64, 1709164800), try parse_iso8601_to_unix("2024-02-29T00:00:00Z"));
    try std.testing.expectError(error.InvalidDateFormat, parse_iso8601_to_unix("2024-02-29"));
}

test "make_conflict_filename" {
    const allocator = std.testing.allocator;
    const a = try make_conflict_filename(allocator, "notes.md");
    defer allocator.free(a);
    try std.testing.expectEqualStrings("notes_conflict.md", a);
    const b = try make_conflict_filename(allocator, "README");
    defer allocator.free(b);
    try std.testing.expectEqualStrings("README_conflict", b);
}

test "is_syncable_file filter" {
    try std.testing.expect(is_syncable_file("a.md"));
    try std.testing.expect(is_syncable_file("a.txt"));
    try std.testing.expect(!is_syncable_file("vault.db"));
    try std.testing.expect(!is_syncable_file("image.png"));
    try std.testing.expect(!is_syncable_file(".md.swp"));
}

test "config dir resolves under qirtas" {
    const dir = main.configDir();
    try std.testing.expect(std.mem.endsWith(u8, dir, "/qirtas"));
    const db = std.mem.span(main.dbPathZ());
    try std.testing.expect(std.mem.endsWith(u8, db, "/qirtas/vault.db"));
}
