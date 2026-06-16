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
        md5Checksum: []const u8 = "",
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

// ── PKCE (RFC 7636) for the Drive/Dropbox loopback OAuth flows ──
// Desktop OAuth clients have no usable secret, so the auth server can't tell a
// real exchange from a stolen code. PKCE fixes this: at connect we generate a
// random `verifier`, send code_challenge = base64url(sha256(verifier)) in the
// auth URL, and prove possession by sending the `verifier` in the token
// exchange. Without it Google/Dropbox reject the exchange (invalid_request).
// Only one connect flow runs at a time, so a single slot suffices.
var pkce_verifier: [64]u8 = undefined;
var pkce_verifier_len: usize = 0;

const PKCE_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";

fn pkce_fill_random(buf: []u8) void {
    var i: usize = 0;
    while (i < buf.len) {
        const rc = std.os.linux.getrandom(buf[i..].ptr, buf.len - i, 0);
        switch (std.posix.errno(rc)) {
            .SUCCESS => i += @intCast(rc),
            .INTR => continue,
            else => break,
        }
    }
    // Extremely rare: top up any shortfall so we never emit a zero verifier.
    if (i < buf.len) {
        const seed: usize = @intCast(c.time(null));
        for (buf[i..], 0..) |*b, j| b.* = @truncate(seed +% j +% i);
    }
}

/// Generate a fresh verifier, store it, and write the S256 challenge (base64url,
/// no padding) into `out`. Returns challenge length (43), or 0 if out too small.
pub export fn zig_pkce_challenge(out: [*]u8, out_max: usize) callconv(.c) usize {
    var raw: [pkce_verifier.len]u8 = undefined;
    pkce_fill_random(&raw);
    for (&pkce_verifier, raw) |*v, r| v.* = PKCE_CHARS[r % PKCE_CHARS.len];
    pkce_verifier_len = pkce_verifier.len;

    var digest: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(pkce_verifier[0..], &digest, .{});

    const Enc = std.base64.url_safe_no_pad.Encoder;
    const need = Enc.calcSize(digest.len);
    if (need > out_max) return 0;
    _ = Enc.encode(out[0..need], &digest);
    return need;
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
        .headers = .{ .accept_encoding = .omit },
        .extra_headers = &[_]std.http.Header{
            .{ .name = "Content-Type", .value = "application/x-www-form-urlencoded" },
        },
    });
    defer req.deinit();

    // PKCE: prove possession of the verifier generated at connect time.
    const verifier = pkce_verifier[0..pkce_verifier_len];
    const body = if (client_secret.len > 0)
        try std.fmt.allocPrint(allocator,
            "code={s}&client_id={s}&client_secret={s}&redirect_uri=http://localhost:12345&grant_type=authorization_code&code_verifier={s}",
            .{ code, client_id, client_secret, verifier },
        )
    else
        try std.fmt.allocPrint(allocator,
            "code={s}&client_id={s}&redirect_uri=http://localhost:12345&grant_type=authorization_code&code_verifier={s}",
            .{ code, client_id, verifier },
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
        .headers = .{ .accept_encoding = .omit },
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
    // Despite the legacy name this is now an ATOMIC write (tmp + fsync +
    // rename via main.atomicWriteFile) — sync downloads must never be able
    // to leave a half-written note on crash/power loss either.
    try main.atomicWriteFile(filename, content);
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
        error.OpenFileFailed => "Error: cannot read local file.",
        error.StatFileFailed => "Error: cannot stat local file.",
        error.MmapFailed => "Error: cannot mmap local file.",
        error.CreateSyncDirectoryFailed => "Error: cannot create sync folder.",
        error.SyncTargetIsNotDirectory => "Error: sync target is not a directory.",
        error.TokenExchangeFailed => "Error: token exchange failed.",
        error.TokenRefreshFailed => "Error: token refresh failed.",
        error.DriveDownloadFailed => "Error: Google Drive download failed.",
        error.DriveUpdateFailed => "Error: Google Drive update failed.",
        error.DriveCreateFailed => "Error: Google Drive upload failed.",
        error.DriveListFailed => "Error: Google Drive list failed.",
        error.DriveMetaFailed => "Error: Google Drive metadata failed.",
        error.GithubUserFailed => "Error: GitHub auth check failed.",
        error.GithubListFailed => "Error: GitHub list failed.",
        error.GithubDownloadFailed => "Error: GitHub download failed.",
        error.GithubUploadFailed => "Error: GitHub upload failed.",
        error.GithubShaConflict => "Error: GitHub upload conflict, resync.",
        error.GithubRepoNotFound => "Repo not found, or token lacks 'repo' access.",
        error.GithubRepoCreateForbidden => "Cannot create repo. Create it on GitHub manually.",
        error.DropboxListFailed => "Error: Dropbox list failed.",
        error.DropboxDownloadFailed => "Error: Dropbox download failed.",
        error.DropboxUploadFailed => "Error: Dropbox upload failed.",
        error.ConnectionRefused => "Error: connection refused.",
        error.UnknownHostName => "Error: unknown host.",
        error.ConnectionReset => "Error: connection reset.",
        error.NetworkUnreachable => "Error: network unreachable.",
        error.HostLacksNetworkAddresses => "Error: no network addresses.",
        else => "",
    };
}

fn sync_error_message_z(err: anyerror, buf: []u8) [:0]const u8 {
    const known = sync_error_message(err);
    if (known.len > 0) return std.fmt.bufPrintZ(buf, "{s}", .{known}) catch unreachable;
    // Unknown error — show the name so the user can report it
    return std.fmt.bufPrintZ(buf, "Error: {s}", .{@errorName(err)}) catch
        std.fmt.bufPrintZ(buf, "Error: sync failed.", .{}) catch unreachable;
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
            .headers = .{ .accept_encoding = .omit },
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
            .headers = .{ .accept_encoding = .omit },
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

/// Metadata-only PATCH (no /upload/ prefix, JSON body) to rename a Drive file
/// in place. Used by rename detection so a local rename becomes a Drive
/// rename instead of a new upload + the old name being resurrected.
fn rename_file_on_drive(allocator: std.mem.Allocator, access_token: []const u8, file_id: []const u8, new_name: []const u8) !void {
    var attempt: u32 = 0;
    const max_attempts = 3;
    while (attempt < max_attempts) : (attempt += 1) {
        if (attempt > 0) {
            _ = c.usleep(attempt * 1000 * 1000);
        }

        var client = std.http.Client{ .allocator = allocator, .io = main.global_io };
        defer client.deinit();

        const update_url = std.fmt.allocPrint(allocator, "https://www.googleapis.com/drive/v3/files/{s}?fields=id", .{file_id}) catch |e| return e;
        defer allocator.free(update_url);

        const uri = std.Uri.parse(update_url) catch |e| return e;
        const auth_hdr = std.fmt.allocPrint(allocator, "Bearer {s}", .{access_token}) catch |e| return e;
        defer allocator.free(auth_hdr);

        const name_json = json_escape_header(allocator, new_name) catch |e| return e;
        defer allocator.free(name_json);
        const body = std.fmt.allocPrint(allocator, "{{\"name\": \"{s}\"}}", .{name_json}) catch |e| return e;
        defer allocator.free(body);

        var req = client.request(.PATCH, uri, .{
            .headers = .{ .accept_encoding = .omit },
            .extra_headers = &[_]std.http.Header{
                .{ .name = "Authorization", .value = auth_hdr },
                .{ .name = "Content-Type", .value = "application/json" },
            },
        }) catch |err| {
            std.debug.print("Rename attempt {} failed to send request: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };
        defer req.deinit();

        req.sendBodyComplete(@constCast(body)) catch |err| {
            std.debug.print("Rename attempt {} failed to send body: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };

        var redirect_buf: [1024]u8 = undefined;
        const response = req.receiveHead(&redirect_buf) catch |err| {
            std.debug.print("Rename attempt {} failed to receive head: {}\n", .{attempt + 1, err});
            if (attempt + 1 == max_attempts) return err;
            continue;
        };

        if (response.head.status != .ok) {
            std.debug.print("Rename attempt {} failed with HTTP status: {}\n", .{attempt + 1, response.head.status});
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

/// Finds (or creates) the per-vault folder named `vault_id` inside
/// appDataFolder and returns its Drive file ID. All sync listing/uploads
/// are scoped to this folder, so files from other vaults never appear
/// in this vault's appDataFolder listing.
fn drive_ensure_vault_folder(allocator: std.mem.Allocator, access_token: []const u8, vault_id: []const u8) ![]u8 {
    const auth_hdr = try std.fmt.allocPrint(allocator, "Bearer {s}", .{access_token});
    defer allocator.free(auth_hdr);

    const search_url = try std.fmt.allocPrint(allocator,
        "https://www.googleapis.com/drive/v3/files?q=name+%3D+%27{s}%27+and+%27appDataFolder%27+in+parents+and+trashed+%3D+false&spaces=appDataFolder&fields=files(id,name)",
        .{vault_id});
    defer allocator.free(search_url);

    const search_res = try http_fetch(allocator, .GET, search_url, &[_]std.http.Header{
        .{ .name = "Authorization", .value = auth_hdr },
    }, null);
    defer allocator.free(search_res.body);

    if (search_res.status == .unauthorized or search_res.status == .forbidden) {
        zig_sync_disconnect();
        return error.AuthenticationExpired;
    }
    if (search_res.status == .ok) {
        const FolderSearch = struct {
            files: []struct { id: []const u8, name: []const u8 },
        };
        if (std.json.parseFromSlice(FolderSearch, allocator, search_res.body, .{ .ignore_unknown_fields = true })) |parsed| {
            defer parsed.deinit();
            if (parsed.value.files.len > 0) {
                return try allocator.dupe(u8, parsed.value.files[0].id);
            }
        } else |_| {}
    }

    const create_body = try std.fmt.allocPrint(allocator,
        "{{\"name\": \"{s}\", \"mimeType\": \"application/vnd.google-apps.folder\", \"parents\": [\"appDataFolder\"]}}",
        .{vault_id});
    defer allocator.free(create_body);

    const create_res = try http_fetch(allocator, .POST, "https://www.googleapis.com/drive/v3/files", &[_]std.http.Header{
        .{ .name = "Authorization", .value = auth_hdr },
        .{ .name = "Content-Type", .value = "application/json" },
    }, create_body);
    defer allocator.free(create_res.body);

    if (create_res.status == .unauthorized or create_res.status == .forbidden) {
        zig_sync_disconnect();
        return error.AuthenticationExpired;
    }
    if (create_res.status != .ok and create_res.status != .created) return error.DriveCreateFolderFailed;

    const parsed = try std.json.parseFromSlice(DriveUploadResponse, allocator, create_res.body, .{ .ignore_unknown_fields = true });
    defer parsed.deinit();
    return try allocator.dupe(u8, parsed.value.id);
}

fn upload_file_to_drive(allocator: std.mem.Allocator, access_token: []const u8, parent_folder_id: []const u8, filename: []const u8, content: []const u8) ![]const u8 {
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
            .headers = .{ .accept_encoding = .omit },
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
            \\{{"name": "{s}", "parents": ["{s}"]}}
            \\
            \\--foo
            \\Content-Type: text/markdown
            \\
            \\{s}
            \\--foo--
            ,
            .{filename, parent_folder_id, content}
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
            .headers = .{ .accept_encoding = .omit },
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

/// Returns "<base>/<vault_id>" so each vault syncs to its own subfolder
/// instead of dumping every vault's files into one shared directory.
fn local_sync_dir_path(allocator: std.mem.Allocator, vault_id: []const u8) ![]u8 {
    if (c.getenv("QIRTAS_LOCAL_SYNC_DIR")) |env_path| {
        const path = std.mem.span(env_path);
        if (path.len > 0) return try std.fmt.allocPrint(allocator, "{s}/{s}", .{ path, vault_id });
    }

    if (c.getenv("HOME")) |home| {
        return try std.fmt.allocPrint(allocator, "{s}/QirtasSync/{s}", .{ std.mem.span(home), vault_id });
    }

    return try std.fmt.allocPrint(allocator, "/tmp/QirtasSync/{s}", .{vault_id});
}

/// Creates `path` and any missing parent directories.
fn ensure_directory(path: []const u8) !void {
    const path_z = try std.heap.page_allocator.dupeZ(u8, path);
    defer std.heap.page_allocator.free(path_z);

    var st: c.struct_stat = undefined;
    if (c.stat(path_z.ptr, &st) == 0) {
        if ((st.st_mode & c.S_IFMT) == c.S_IFDIR) return;
        return error.SyncTargetIsNotDirectory;
    }

    if (std.fs.path.dirname(path)) |parent| {
        if (parent.len > 0) try ensure_directory(parent);
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

    const conflicts = local_sync_impl(arena.allocator()) catch |err| {
        std.debug.print("Local sync failed: {}\n", .{err});
        var msg_buf: [128]u8 = undefined;
        gui_update_local_sync_status(0, sync_error_message_z(err, &msg_buf));
        return;
    };

    gui_refresh_explorer();
    if (conflicts > 0) {
        var buf: [64]u8 = undefined;
        const msg = std.fmt.bufPrintZ(&buf, "Synced ({d} conflict{s} saved)", .{ conflicts, if (conflicts == 1) "" else "s" }) catch "~/QirtasSync";
        gui_update_local_sync_status(1, msg);
    } else {
        gui_update_local_sync_status(1, "~/QirtasSync");
    }
}

/// Local folder sync, 3-way like the cloud backends: per-file last-synced
/// mtimes in local_sync_meta. Both-changed → local saved as _conflict copy,
/// the sync-folder version wins the original name (consistent with Drive:
/// "remote" wins). Content is compared first so equal files never produce
/// a false conflict.
fn local_sync_impl(allocator: std.mem.Allocator) !u32 {
    var conflicts: u32 = 0;
    var num_buf: [24]u8 = undefined;

    var vid_buf: [256]u8 = undefined;
    const vault_id = vault_id_hex(&vid_buf);

    const target_dir = try local_sync_dir_path(allocator, vault_id);
    try ensure_directory(target_dir);

    var local_map = try collect_local_files(allocator);
    defer local_map.deinit();

    var local_it = local_map.iterator();
    while (local_it.next()) |entry| {
        const name = entry.key_ptr.*;
        const local_mtime = entry.value_ptr.*;
        const target_path = try join_path(allocator, target_dir, name);
        defer allocator.free(target_path);
        const scoped_name = try scoped_key(allocator, vault_id, name);
        defer allocator.free(scoped_name);

        if (stat_path(target_path)) |remote_stat| {
            const remote_mtime = remote_stat.st_mtim.tv_sec;
            const meta = sync_meta_get("local_sync_meta", allocator, scoped_name);
            defer if (meta) |m| allocator.free(m.remote_marker);

            const stored_local: i64 = if (meta) |m| m.local_mtime else -1;
            const stored_remote: i64 = if (meta) |m|
                std.fmt.parseInt(i64, m.remote_marker, 10) catch -1
            else
                -1;
            const local_changed = local_mtime != stored_local;
            const remote_changed = remote_mtime != stored_remote;

            if (local_changed and remote_changed) {
                const local_content = try read_file_content(allocator, name);
                defer allocator.free(local_content);
                const remote_content = try read_file_content(allocator, target_path);
                defer allocator.free(remote_content);

                if (!std.mem.eql(u8, local_content, remote_content)) {
                    try preserve_local_as_conflict(allocator, name);
                    conflicts += 1;
                    try write_file_mmap(name, remote_content);
                    const name_z = try allocator.dupeZ(u8, name);
                    defer allocator.free(name_z);
                    gui_index_file(name_z.ptr);
                    main.alert_file_updated();
                }
                const new_st = stat_path(name) orelse continue;
                sync_meta_set("local_sync_meta", allocator, scoped_name, new_st.st_mtim.tv_sec, i64_to_text(&num_buf, remote_mtime));
            } else if (local_changed) {
                try copy_file_path(allocator, name, target_path);
                const new_remote = stat_path(target_path) orelse continue;
                sync_meta_set("local_sync_meta", allocator, scoped_name, local_mtime, i64_to_text(&num_buf, new_remote.st_mtim.tv_sec));
            } else if (remote_changed) {
                try copy_file_path(allocator, target_path, name);
                const name_z = try allocator.dupeZ(u8, name);
                defer allocator.free(name_z);
                gui_index_file(name_z.ptr);
                main.alert_file_updated();
                const new_st = stat_path(name) orelse continue;
                sync_meta_set("local_sync_meta", allocator, scoped_name, new_st.st_mtim.tv_sec, i64_to_text(&num_buf, remote_mtime));
            } else if (meta == null) {
                sync_meta_set("local_sync_meta", allocator, scoped_name, local_mtime, i64_to_text(&num_buf, remote_mtime));
            }
        } else {
            try copy_file_path(allocator, name, target_path);
            const new_remote = stat_path(target_path) orelse continue;
            sync_meta_set("local_sync_meta", allocator, scoped_name, local_mtime, i64_to_text(&num_buf, new_remote.st_mtim.tv_sec));
        }
    }

    // Sync-folder-only files → copy in
    const remote_dir_z = try allocator.dupeZ(u8, target_dir);
    defer allocator.free(remote_dir_z);
    const remote_dir = c.opendir(remote_dir_z.ptr);
    if (remote_dir == null) return error.OpenDirFailed;
    defer _ = c.closedir(remote_dir);

    while (true) {
        const entry = c.readdir(remote_dir);
        if (entry == null) break;
        const name = std.mem.span(@as([*:0]const u8, @ptrCast(&entry.*.d_name)));
        if (!is_syncable_file(name)) continue;
        if (local_map.contains(name)) continue;

        if (stat_path(name) == null) {
            const source_path = try join_path(allocator, target_dir, name);
            defer allocator.free(source_path);
            try copy_file_path(allocator, source_path, name);
            const name_z = try allocator.dupeZ(u8, name);
            defer allocator.free(name_z);
            gui_index_file(name_z.ptr);
            const new_st = stat_path(name) orelse continue;
            const rs = stat_path(source_path) orelse continue;
            const scoped_name = try scoped_key(allocator, vault_id, name);
            defer allocator.free(scoped_name);
            sync_meta_set("local_sync_meta", allocator, scoped_name, new_st.st_mtim.tv_sec, i64_to_text(&num_buf, rs.st_mtim.tv_sec));
        }
    }

    return conflicts;
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

    var vid_buf: [256]u8 = undefined;
    const vault_id = vault_id_hex(&vid_buf);

    const vault_folder_id = try drive_ensure_vault_folder(allocator, access_token, vault_id);
    defer allocator.free(vault_folder_id);

    const search_url = try std.fmt.allocPrint(allocator,
        "https://www.googleapis.com/drive/v3/files?q=%27{s}%27+in+parents+and+trashed+%3D+false&spaces=appDataFolder&fields=files(id,name,modifiedTime,md5Checksum)",
        .{vault_folder_id});
    defer allocator.free(search_url);

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

        const uri_search = std.Uri.parse(search_url) catch |e| return e;
        const auth_hdr = std.fmt.allocPrint(allocator, "Bearer {s}", .{access_token}) catch |e| return e;
        defer allocator.free(auth_hdr);

        var req_search = client.request(.GET, uri_search, .{
            .headers = .{ .accept_encoding = .omit },
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
        md5: []const u8,
    };
    var cloud_map = std.StringHashMap(CloudFileMeta).init(allocator);
    defer cloud_map.deinit();

    for (parsed.value.files) |file| {
        try cloud_map.put(file.name, .{ .id = file.id, .modifiedTime = file.modifiedTime, .md5 = file.md5Checksum });
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

    // ── Rename detection (content-hash match) ──────────────────────────
    // A local rename looks like "new local file" + "cloud file whose local
    // counterpart vanished". Without this, the new name gets uploaded as a
    // brand-new Drive file AND the old cloud name gets downloaded back,
    // resurrecting the renamed-away file. If a cloud file we previously
    // tracked under name X has no local counterpart, and some untracked local
    // file's MD5 matches its md5Checksum, treat it as X having been renamed
    // to that file: rename the Drive file in place instead of duplicating it.
    var handled_local = std.StringHashMap(void).init(allocator);
    defer handled_local.deinit();
    var handled_cloud = std.StringHashMap(void).init(allocator);
    defer handled_cloud.deinit();
    {
        var rename_db: ?*c.sqlite3 = null;
        if (c.sqlite3_open(dbp(), &rename_db) == c.SQLITE_OK) {
            defer _ = c.sqlite3_close(rename_db);
            _ = c.sqlite3_busy_timeout(rename_db, 5000);

            var cloud_it_r = cloud_map.iterator();
            while (cloud_it_r.next()) |centry| {
                const cloud_name = centry.key_ptr.*;
                const cloud_meta = centry.value_ptr.*;
                if (local_map.contains(cloud_name)) continue;
                if (cloud_meta.md5.len == 0) continue;

                var was_tracked = false;
                {
                    const sql_chk = "SELECT drive_file_id FROM file_metadata WHERE filepath = ?;";
                    var stmt_chk: ?*c.sqlite3_stmt = null;
                    if (c.sqlite3_prepare_v2(rename_db, sql_chk.ptr, -1, &stmt_chk, null) == c.SQLITE_OK) {
                        defer _ = c.sqlite3_finalize(stmt_chk);
                        const cn_z = try allocator.dupeZ(u8, cloud_name);
                        defer allocator.free(cn_z);
                        _ = c.sqlite3_bind_text(stmt_chk, 1, cn_z.ptr, -1, null);
                        if (c.sqlite3_step(stmt_chk) == c.SQLITE_ROW) {
                            const id_c = c.sqlite3_column_text(stmt_chk, 0);
                            if (id_c != null and std.mem.eql(u8, std.mem.span(id_c), cloud_meta.id)) {
                                was_tracked = true;
                            }
                        }
                    }
                }
                if (!was_tracked) continue;

                var local_it_r = local_map.iterator();
                while (local_it_r.next()) |lentry| {
                    const local_name = lentry.key_ptr.*;
                    if (cloud_map.contains(local_name)) continue;
                    if (handled_local.contains(local_name)) continue;

                    const content = read_file_content(allocator, local_name) catch continue;
                    defer allocator.free(content);
                    const hash = md5_hex(allocator, content) catch continue;
                    defer allocator.free(hash);
                    if (!std.mem.eql(u8, hash, cloud_meta.md5)) continue;

                    std.debug.print("Detected rename: {s} -> {s} (Drive id {s})\n", .{cloud_name, local_name, cloud_meta.id});
                    rename_file_on_drive(allocator, access_token, cloud_meta.id, local_name) catch |err| {
                        std.debug.print("Rename failed for {s}: {}\n", .{cloud_name, err});
                        continue;
                    };

                    {
                        const sql_del = "DELETE FROM file_metadata WHERE filepath = ?;";
                        var stmt_del: ?*c.sqlite3_stmt = null;
                        if (c.sqlite3_prepare_v2(rename_db, sql_del.ptr, -1, &stmt_del, null) == c.SQLITE_OK) {
                            defer _ = c.sqlite3_finalize(stmt_del);
                            const cn_z = try allocator.dupeZ(u8, cloud_name);
                            defer allocator.free(cn_z);
                            _ = c.sqlite3_bind_text(stmt_del, 1, cn_z.ptr, -1, null);
                            _ = c.sqlite3_step(stmt_del);
                        }
                    }

                    try update_local_metadata(allocator, local_name, cloud_meta.id, lentry.value_ptr.*.st_mtim.tv_sec);

                    try handled_local.put(local_name, {});
                    try handled_cloud.put(cloud_name, {});
                    break;
                }
            }
        }
    }

    var local_it = local_map.iterator();
    while (local_it.next()) |entry| {
        const filename = entry.key_ptr.*;
        if (handled_local.contains(filename)) continue;
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
            const drive_id_new = try upload_file_to_drive(allocator, access_token, vault_folder_id, filename, local_content);
            defer allocator.free(drive_id_new);

            const new_cloud_time = try get_file_modified_time_from_drive(allocator, access_token, drive_id_new);
            try update_local_metadata(allocator, filename, drive_id_new, new_cloud_time);
        }
    }

    var cloud_it = cloud_map.iterator();
    while (cloud_it.next()) |entry| {
        const filename = entry.key_ptr.*;
        const cloud_meta = entry.value_ptr.*;
        if (handled_cloud.contains(filename)) continue;

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

/// Lowercase hex MD5 of `content`, in the same format as Drive's md5Checksum
/// field — lets a rename be detected by comparing hashes without downloading.
fn md5_hex(allocator: std.mem.Allocator, content: []const u8) ![]u8 {
    var digest: [16]u8 = undefined;
    std.crypto.hash.Md5.hash(content, &digest, .{});
    return try bytesToHexAlloc(allocator, &digest);
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

/// Stable per-vault identifier: first 8 bytes of SHA256(cwd) as 16 lowercase
/// hex chars. zig_open_vault() chdir()s into the vault directory, so cwd
/// uniquely identifies the currently open vault for namespacing remote
/// sync folders and per-file sync state.
/// Per-vault remote folder name: a readable slug from the vault directory's
/// basename plus a 4-hex fingerprint of its full path, e.g. "my-notes-7c53".
/// The slug is for humans; the suffix keeps two same-named folders (on
/// different machines) from colliding in the same repo/account.
fn vault_id_hex(out: *[256]u8) []const u8 {
    var cwd_buf: [4096]u8 = undefined;
    const cwd_ptr = c.getcwd(&cwd_buf, cwd_buf.len);
    const cwd: []const u8 = if (cwd_ptr != null) std.mem.span(cwd_ptr) else "/";

    var digest: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(cwd, &digest, .{});
    const hex_chars = "0123456789abcdef";

    // Readable part: the folder's basename, sanitized to [A-Za-z0-9._-].
    const base = std.fs.path.basename(cwd);
    var n: usize = 0;
    for (base) |ch| {
        if (n + 6 >= out.len) break; // reserve room for "-XXXX"
        const ok = (ch >= 'A' and ch <= 'Z') or (ch >= 'a' and ch <= 'z') or
            (ch >= '0' and ch <= '9') or ch == '.' or ch == '_' or ch == '-';
        if (ok) {
            out[n] = ch;
            n += 1;
        } else if (ch == ' ') {
            out[n] = '-';
            n += 1;
        }
    }
    if (n == 0) {
        const fallback = "vault";
        @memcpy(out[0..fallback.len], fallback);
        n = fallback.len;
    }

    // Fingerprint suffix: "-" + first 2 bytes of the path hash as 4 hex chars.
    out[n] = '-';
    n += 1;
    out[n] = hex_chars[digest[0] >> 4];
    n += 1;
    out[n] = hex_chars[digest[0] & 0x0f];
    n += 1;
    out[n] = hex_chars[digest[1] >> 4];
    n += 1;
    out[n] = hex_chars[digest[1] & 0x0f];
    n += 1;
    return out[0..n];
}

/// Vault-scoped key for the *_sync_meta tables, so two vaults that both
/// have e.g. "notes/todo.md" don't clobber each other's sync state.
fn scoped_key(allocator: std.mem.Allocator, vault_id: []const u8, filename: []const u8) ![]u8 {
    return std.fmt.allocPrint(allocator, "{s}/{s}", .{ vault_id, filename });
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


// ─────────────────────────────────────────────────────────────
// Shared sync infrastructure: HTTP fetch, per-file sync metadata,
// encoders. Used by the native Dropbox / GitHub / Local backends.
// ─────────────────────────────────────────────────────────────

const HttpResult = struct {
    status: std.http.Status,
    body: []u8,
};

/// One-shot HTTP request collecting the whole response body.
/// Caller frees result.body.
fn http_fetch(
    allocator: std.mem.Allocator,
    method: std.http.Method,
    url: []const u8,
    headers: []const std.http.Header,
    body: ?[]const u8,
) !HttpResult {
    var client = std.http.Client{ .allocator = allocator, .io = main.global_io };
    defer client.deinit();

    const uri = try std.Uri.parse(url);
    var req = try client.request(method, uri, .{
        .extra_headers = headers,
        .headers = .{ .accept_encoding = .omit },
    });
    defer req.deinit();

    if (body) |b| {
        const buf = try allocator.dupe(u8, b);
        defer allocator.free(buf);
        try req.sendBodyComplete(buf);
    } else {
        try req.sendBodiless();
    }

    var redirect_buf: [2048]u8 = undefined;
    var response = try req.receiveHead(&redirect_buf);
    const status = response.head.status;

    var transfer_buf: [4096]u8 = undefined;
    const rdr = response.reader(&transfer_buf);
    var list: std.ArrayList(u8) = .empty;
    errdefer list.deinit(allocator);
    while (true) {
        var chunk: [4096]u8 = undefined;
        const n = rdr.readSliceShort(&chunk) catch |err| {
            if (err == error.EndOfStream) break;
            return err;
        };
        if (n == 0) break;
        try list.appendSlice(allocator, chunk[0..n]);
    }
    return .{ .status = status, .body = try list.toOwnedSlice(allocator) };
}

/// Per-file last-synced state. `remote_marker` is backend-defined: a decimal
/// mtime for Dropbox/Local, a git blob sha for GitHub. The 3-way decision is
/// always: local_changed = (local mtime != stored), remote_changed = (remote
/// marker != stored). Both changed → conflict copy, remote wins original.
const SyncMeta = struct {
    local_mtime: i64,
    remote_marker: []u8,
};

fn sync_meta_ensure(comptime table: []const u8, db: *c.sqlite3) void {
    _ = c.sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS " ++ table ++
            " (filepath TEXT PRIMARY KEY, local_mtime INTEGER, remote_marker TEXT);",
        null, null, null);
}

fn sync_meta_get(comptime table: []const u8, allocator: std.mem.Allocator, filepath: []const u8) ?SyncMeta {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(dbp(), &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return null;
    }
    defer _ = c.sqlite3_close(db);
    _ = c.sqlite3_busy_timeout(db, 5000);
    sync_meta_ensure(table, db.?);

    const sql = "SELECT local_mtime, remote_marker FROM " ++ table ++ " WHERE filepath = ?;";
    var stmt: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) != c.SQLITE_OK) return null;
    defer _ = c.sqlite3_finalize(stmt);

    const fp_z = allocator.dupeZ(u8, filepath) catch return null;
    defer allocator.free(fp_z);
    _ = c.sqlite3_bind_text(stmt, 1, fp_z.ptr, -1, null);

    if (c.sqlite3_step(stmt) == c.SQLITE_ROW) {
        const lm = c.sqlite3_column_int64(stmt, 0);
        const rm = c.sqlite3_column_text(stmt, 1);
        const rm_dup = allocator.dupe(u8, if (rm != null) std.mem.span(rm) else "") catch return null;
        return SyncMeta{ .local_mtime = lm, .remote_marker = rm_dup };
    }
    return null;
}

fn sync_meta_set(comptime table: []const u8, allocator: std.mem.Allocator, filepath: []const u8, local_mtime: i64, remote_marker: []const u8) void {
    var db: ?*c.sqlite3 = null;
    if (c.sqlite3_open(dbp(), &db) != c.SQLITE_OK) {
        if (db != null) _ = c.sqlite3_close(db);
        return;
    }
    defer _ = c.sqlite3_close(db);
    _ = c.sqlite3_busy_timeout(db, 5000);
    sync_meta_ensure(table, db.?);

    const sql = "INSERT OR REPLACE INTO " ++ table ++ " (filepath, local_mtime, remote_marker) VALUES (?, ?, ?);";
    var stmt: ?*c.sqlite3_stmt = null;
    if (c.sqlite3_prepare_v2(db, sql.ptr, -1, &stmt, null) != c.SQLITE_OK) return;
    defer _ = c.sqlite3_finalize(stmt);

    const fp_z = allocator.dupeZ(u8, filepath) catch return;
    defer allocator.free(fp_z);
    const rm_z = allocator.dupeZ(u8, remote_marker) catch return;
    defer allocator.free(rm_z);

    _ = c.sqlite3_bind_text(stmt, 1, fp_z.ptr, -1, null);
    _ = c.sqlite3_bind_int64(stmt, 2, local_mtime);
    _ = c.sqlite3_bind_text(stmt, 3, rm_z.ptr, -1, null);
    _ = c.sqlite3_step(stmt);
}

fn i64_to_text(buf: []u8, v: i64) []const u8 {
    return std.fmt.bufPrint(buf, "{d}", .{v}) catch "0";
}

/// Percent-encode a path segment (everything except unreserved chars).
fn percent_encode(allocator: std.mem.Allocator, s: []const u8) ![]u8 {
    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(allocator);
    for (s) |ch| {
        const unreserved = (ch >= 'A' and ch <= 'Z') or (ch >= 'a' and ch <= 'z') or
            (ch >= '0' and ch <= '9') or ch == '-' or ch == '.' or ch == '_' or ch == '~';
        if (unreserved) {
            try out.append(allocator, ch);
        } else {
            var hex: [3]u8 = undefined;
            _ = std.fmt.bufPrint(&hex, "%{X:0>2}", .{ch}) catch unreachable;
            try out.appendSlice(allocator, &hex);
        }
    }
    return out.toOwnedSlice(allocator);
}

/// Escape a string for use inside HTTP-header JSON (Dropbox-API-Arg):
/// quotes/backslash/control escaped, all non-ASCII as \uXXXX (UTF-16,
/// surrogate pairs for astral chars) — headers must stay ASCII.
fn json_escape_header(allocator: std.mem.Allocator, s: []const u8) ![]u8 {
    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(allocator);
    var it = (std.unicode.Utf8View.init(s) catch return error.InvalidUtf8).iterator();
    while (it.nextCodepoint()) |cp| {
        if (cp == '"' or cp == '\\') {
            try out.append(allocator, '\\');
            try out.append(allocator, @intCast(cp));
        } else if (cp < 0x20) {
            var esc: [6]u8 = undefined;
            _ = std.fmt.bufPrint(&esc, "\\u{X:0>4}", .{cp}) catch unreachable;
            try out.appendSlice(allocator, &esc);
        } else if (cp < 0x80) {
            try out.append(allocator, @intCast(cp));
        } else if (cp <= 0xFFFF) {
            var esc: [6]u8 = undefined;
            _ = std.fmt.bufPrint(&esc, "\\u{X:0>4}", .{cp}) catch unreachable;
            try out.appendSlice(allocator, &esc);
        } else {
            const v = cp - 0x10000;
            const hi = 0xD800 + (v >> 10);
            const lo = 0xDC00 + (v & 0x3FF);
            var esc: [12]u8 = undefined;
            const w = std.fmt.bufPrint(&esc, "\\u{X:0>4}\\u{X:0>4}", .{ hi, lo }) catch unreachable;
            try out.appendSlice(allocator, w);
        }
    }
    return out.toOwnedSlice(allocator);
}

/// Scan the workspace for syncable files → name→mtime map. Keys are
/// allocator-owned.
fn collect_local_files(allocator: std.mem.Allocator) !std.StringHashMap(i64) {
    var map = std.StringHashMap(i64).init(allocator);
    errdefer map.deinit();
    const dir_handle = c.opendir(".");
    if (dir_handle == null) return error.OpenDirFailed;
    defer _ = c.closedir(dir_handle);
    while (true) {
        const entry = c.readdir(dir_handle);
        if (entry == null) break;
        const name = std.mem.span(@as([*:0]const u8, @ptrCast(&entry.*.d_name)));
        if (!is_syncable_file(name)) continue;
        if (stat_path(name)) |st| {
            const key = try allocator.dupe(u8, name);
            try map.put(key, st.st_mtim.tv_sec);
        }
    }
    return map;
}

/// Save the local version under <name>_conflict.<ext> before the remote
/// version takes the original filename.
fn preserve_local_as_conflict(allocator: std.mem.Allocator, filename: []const u8) !void {
    const conflict_name = try make_conflict_filename(allocator, filename);
    defer allocator.free(conflict_name);
    const local_content = try read_file_content(allocator, filename);
    defer allocator.free(local_content);
    try write_file_mmap(conflict_name, local_content);
    const z = try allocator.dupeZ(u8, conflict_name);
    defer allocator.free(z);
    gui_index_file(z.ptr);
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
        .headers = .{ .accept_encoding = .omit },
        .extra_headers = &[_]std.http.Header{
            .{ .name = "Content-Type", .value = "application/x-www-form-urlencoded" },
        },
    });
    defer req.deinit();

    // PKCE: prove possession of the verifier generated at connect time.
    const verifier = pkce_verifier[0..pkce_verifier_len];
    const body = if (client_secret.len > 0)
        try std.fmt.allocPrint(allocator,
            "code={s}&client_id={s}&client_secret={s}&redirect_uri=http://localhost:5173&grant_type=authorization_code&code_verifier={s}",
            .{ code, client_id, client_secret, verifier },
        )
    else
        try std.fmt.allocPrint(allocator,
            "code={s}&client_id={s}&redirect_uri=http://localhost:5173&grant_type=authorization_code&code_verifier={s}",
            .{ code, client_id, verifier },
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
        .headers = .{ .accept_encoding = .omit },
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

// ─────────────────────────────────────────────────────────────
// Native Dropbox sync (no shell script). 3-way conflict detection
// matching the Google Drive backend: per-file last-synced state in
// dropbox_sync_meta; both-changed → local saved as _conflict copy,
// remote wins the original name.
// ─────────────────────────────────────────────────────────────

const DBX_FOLDER = "/qirtas";

const DbxListResp = struct {
    entries: []struct {
        @".tag": []const u8 = "",
        name: []const u8 = "",
        server_modified: []const u8 = "",
    },
    cursor: []const u8 = "",
    has_more: bool = false,
};

const DbxFileMeta = struct {
    server_modified: []const u8 = "",
};

/// Per-vault subfolder under the app's Dropbox folder, e.g. "/qirtas/<vault_id>".
fn dbx_vault_folder(allocator: std.mem.Allocator, vault_id: []const u8) ![]u8 {
    return std.fmt.allocPrint(allocator, "{s}/{s}", .{ DBX_FOLDER, vault_id });
}

fn dropbox_remote_list(allocator: std.mem.Allocator, token: []const u8, vault_id: []const u8) !std.StringHashMap(i64) {
    var map = std.StringHashMap(i64).init(allocator);
    errdefer map.deinit();

    const auth = try std.fmt.allocPrint(allocator, "Bearer {s}", .{token});
    defer allocator.free(auth);

    var first = true;
    var cursor: []u8 = try allocator.dupe(u8, "");
    defer allocator.free(cursor);

    while (true) {
        const url = if (first)
            "https://api.dropboxapi.com/2/files/list_folder"
        else
            "https://api.dropboxapi.com/2/files/list_folder/continue";
        const body = if (first) blk: {
            const folder = try dbx_vault_folder(allocator, vault_id);
            defer allocator.free(folder);
            break :blk try std.fmt.allocPrint(allocator, "{{\"path\": \"{s}\"}}", .{folder});
        } else try std.fmt.allocPrint(allocator, "{{\"cursor\": \"{s}\"}}", .{cursor});
        defer allocator.free(body);

        const res = try http_fetch(allocator, .POST, url, &[_]std.http.Header{
            .{ .name = "Authorization", .value = auth },
            .{ .name = "Content-Type", .value = "application/json" },
        }, body);
        defer allocator.free(res.body);

        if (res.status != .ok) {
            // path/not_found = empty folder on first sync
            if (first and std.mem.indexOf(u8, res.body, "not_found") != null) return map;
            if (res.status == .unauthorized) return error.AuthenticationExpired;
            std.debug.print("Dropbox list failed {}: {s}\n", .{ res.status, res.body });
            return error.DropboxListFailed;
        }

        const parsed = try std.json.parseFromSlice(DbxListResp, allocator, res.body, .{ .ignore_unknown_fields = true });
        defer parsed.deinit();

        for (parsed.value.entries) |e| {
            if (!std.mem.eql(u8, e.@".tag", "file")) continue;
            if (!is_syncable_file(e.name)) continue;
            const t = parse_iso8601_to_unix(e.server_modified) catch 0;
            const key = try allocator.dupe(u8, e.name);
            try map.put(key, t);
        }

        if (!parsed.value.has_more) break;
        allocator.free(cursor);
        cursor = try allocator.dupe(u8, parsed.value.cursor);
        first = false;
    }
    return map;
}

fn dropbox_api_arg_path(allocator: std.mem.Allocator, vault_id: []const u8, filename: []const u8) ![]u8 {
    const esc = try json_escape_header(allocator, filename);
    defer allocator.free(esc);
    const folder = try dbx_vault_folder(allocator, vault_id);
    defer allocator.free(folder);
    return std.fmt.allocPrint(allocator, "{{\"path\": \"{s}/{s}\"}}", .{ folder, esc });
}

fn dropbox_download(allocator: std.mem.Allocator, token: []const u8, vault_id: []const u8, filename: []const u8) ![]u8 {
    const auth = try std.fmt.allocPrint(allocator, "Bearer {s}", .{token});
    defer allocator.free(auth);
    const arg = try dropbox_api_arg_path(allocator, vault_id, filename);
    defer allocator.free(arg);

    const res = try http_fetch(allocator, .POST, "https://content.dropboxapi.com/2/files/download", &[_]std.http.Header{
        .{ .name = "Authorization", .value = auth },
        .{ .name = "Dropbox-API-Arg", .value = arg },
    }, "");
    if (res.status != .ok) {
        defer allocator.free(res.body);
        std.debug.print("Dropbox download {s} failed {}: {s}\n", .{ filename, res.status, res.body });
        return error.DropboxDownloadFailed;
    }
    return res.body;
}

/// Upload (overwrite) and return the new server_modified unix time.
fn dropbox_upload(allocator: std.mem.Allocator, token: []const u8, vault_id: []const u8, filename: []const u8, content: []const u8) !i64 {
    const auth = try std.fmt.allocPrint(allocator, "Bearer {s}", .{token});
    defer allocator.free(auth);
    const esc = try json_escape_header(allocator, filename);
    defer allocator.free(esc);
    const folder = try dbx_vault_folder(allocator, vault_id);
    defer allocator.free(folder);
    const arg = try std.fmt.allocPrint(allocator,
        "{{\"path\": \"{s}/{s}\", \"mode\": \"overwrite\", \"mute\": true}}", .{ folder, esc });
    defer allocator.free(arg);

    const res = try http_fetch(allocator, .POST, "https://content.dropboxapi.com/2/files/upload", &[_]std.http.Header{
        .{ .name = "Authorization", .value = auth },
        .{ .name = "Dropbox-API-Arg", .value = arg },
        .{ .name = "Content-Type", .value = "application/octet-stream" },
    }, content);
    defer allocator.free(res.body);
    if (res.status != .ok) {
        std.debug.print("Dropbox upload {s} failed {}: {s}\n", .{ filename, res.status, res.body });
        return error.DropboxUploadFailed;
    }
    const parsed = try std.json.parseFromSlice(DbxFileMeta, allocator, res.body, .{ .ignore_unknown_fields = true });
    defer parsed.deinit();
    return parse_iso8601_to_unix(parsed.value.server_modified) catch 0;
}

fn dropbox_sync_impl(allocator: std.mem.Allocator, token: []const u8) !u32 {
    var conflicts: u32 = 0;
    var num_buf: [24]u8 = undefined;

    var vid_buf: [256]u8 = undefined;
    const vault_id = vault_id_hex(&vid_buf);

    var remote_map = try dropbox_remote_list(allocator, token, vault_id);
    defer remote_map.deinit();
    var local_map = try collect_local_files(allocator);
    defer local_map.deinit();

    var local_it = local_map.iterator();
    while (local_it.next()) |entry| {
        const filename = entry.key_ptr.*;
        const local_mtime = entry.value_ptr.*;
        const scoped_name = try scoped_key(allocator, vault_id, filename);
        defer allocator.free(scoped_name);

        if (remote_map.get(filename)) |remote_time| {
            const meta = sync_meta_get("dropbox_sync_meta", allocator, scoped_name);
            defer if (meta) |m| allocator.free(m.remote_marker);

            const stored_local: i64 = if (meta) |m| m.local_mtime else -1;
            const stored_remote: i64 = if (meta) |m|
                std.fmt.parseInt(i64, m.remote_marker, 10) catch -1
            else
                -1;
            const local_changed = local_mtime != stored_local;
            const remote_changed = remote_time != stored_remote;

            if (local_changed and remote_changed) {
                const remote_content = try dropbox_download(allocator, token, vault_id, filename);
                defer allocator.free(remote_content);
                const local_content = try read_file_content(allocator, filename);
                defer allocator.free(local_content);

                if (!std.mem.eql(u8, remote_content, local_content)) {
                    // Real conflict: keep both, remote wins the name.
                    try preserve_local_as_conflict(allocator, filename);
                    conflicts += 1;
                    try write_file_mmap(filename, remote_content);
                    const fz = try allocator.dupeZ(u8, filename);
                    defer allocator.free(fz);
                    gui_index_file(fz.ptr);
                    main.alert_file_updated();
                }
                const new_st = stat_path(filename) orelse continue;
                sync_meta_set("dropbox_sync_meta", allocator, scoped_name, new_st.st_mtim.tv_sec, i64_to_text(&num_buf, remote_time));
            } else if (local_changed) {
                const local_content = try read_file_content(allocator, filename);
                defer allocator.free(local_content);
                const new_remote = try dropbox_upload(allocator, token, vault_id, filename, local_content);
                sync_meta_set("dropbox_sync_meta", allocator, scoped_name, local_mtime, i64_to_text(&num_buf, new_remote));
            } else if (remote_changed) {
                const remote_content = try dropbox_download(allocator, token, vault_id, filename);
                defer allocator.free(remote_content);
                try write_file_mmap(filename, remote_content);
                const fz = try allocator.dupeZ(u8, filename);
                defer allocator.free(fz);
                gui_index_file(fz.ptr);
                main.alert_file_updated();
                const new_st = stat_path(filename) orelse continue;
                sync_meta_set("dropbox_sync_meta", allocator, scoped_name, new_st.st_mtim.tv_sec, i64_to_text(&num_buf, remote_time));
            } else if (meta == null) {
                sync_meta_set("dropbox_sync_meta", allocator, scoped_name, local_mtime, i64_to_text(&num_buf, remote_time));
            }
        } else {
            // Local only → upload
            const local_content = try read_file_content(allocator, filename);
            defer allocator.free(local_content);
            const new_remote = try dropbox_upload(allocator, token, vault_id, filename, local_content);
            sync_meta_set("dropbox_sync_meta", allocator, scoped_name, local_mtime, i64_to_text(&num_buf, new_remote));
        }
    }

    var remote_it = remote_map.iterator();
    while (remote_it.next()) |entry| {
        const filename = entry.key_ptr.*;
        if (local_map.contains(filename)) continue;
        const remote_content = try dropbox_download(allocator, token, vault_id, filename);
        defer allocator.free(remote_content);
        try write_file_mmap(filename, remote_content);
        const fz = try allocator.dupeZ(u8, filename);
        defer allocator.free(fz);
        gui_index_file(fz.ptr);
        const new_st = stat_path(filename) orelse continue;
        const scoped_name = try scoped_key(allocator, vault_id, filename);
        defer allocator.free(scoped_name);
        sync_meta_set("dropbox_sync_meta", allocator, scoped_name, new_st.st_mtim.tv_sec, i64_to_text(&num_buf, entry.value_ptr.*));
    }

    return conflicts;
}

fn dropbox_sync_worker() void {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

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

    const conflicts = dropbox_sync_impl(allocator, token) catch |err| {
        std.debug.print("Dropbox sync failed: {}\n", .{err});
        if (err == error.AuthenticationExpired) {
            zig_dropbox_disconnect();
            gui_update_dropbox_status(0, "Error: auth expired. reconnect.");
            return;
        }
        var msg_buf: [128]u8 = undefined;
        gui_update_dropbox_status(1, sync_error_message_z(err, &msg_buf));
        return;
    };

    gui_refresh_explorer();
    if (conflicts > 0) {
        var buf: [64]u8 = undefined;
        const msg = std.fmt.bufPrintZ(&buf, "Synced ✓ ({d} conflict{s} saved)", .{ conflicts, if (conflicts == 1) "" else "s" }) catch "Synced ✓";
        gui_update_dropbox_status(1, msg);
    } else {
        gui_update_dropbox_status(1, "Synced ✓");
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
        // Fall back to plaintext if decryption fails — tokens stored before
        // encryption was added to zig_save_github_credentials are raw PATs.
        const decrypted = decryptToken(allocator, encrypted) catch
            try allocator.dupe(u8, encrypted);
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
        _ = c.sqlite3_exec(db, "UPDATE github_sync_tokens SET personal_token = '' WHERE id = 1;", null, null, null);
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

// Connect using a Personal Access Token the user pastes. This is the reliable
// path: a classic PAT with `repo` scope (or a fine-grained token with Contents:
// read+write) can create repos and push, sidestepping the GitHub App
// install/permission limits that make the device-flow token unable to write.
pub export fn zig_github_connect_with_token(token_ptr: [*:0]const u8, repo_ptr: [*:0]const u8) callconv(.c) void {
    const token = std.mem.span(token_ptr);
    if (token.len == 0) {
        gui_update_github_status(0, "Paste a token first.");
        return;
    }
    zig_save_github_credentials(token_ptr, repo_ptr);
    gui_update_github_status(2, "Verifying token...");
    _ = std.Thread.spawn(.{}, github_verify_worker, .{}) catch {
        // Couldn't spawn — assume saved token is good; sync will surface errors.
        gui_update_github_status(1, "Connected");
    };
}

fn github_verify_worker() void {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    const creds = get_github_credentials(allocator) catch {
        gui_update_github_status(0, "Could not read saved token.");
        return;
    };
    const auth = std.fmt.allocPrint(allocator, "Bearer {s}", .{creds.token}) catch {
        gui_update_github_status(1, "Connected");
        return;
    };
    _ = github_owner_login(allocator, auth) catch |err| {
        if (err == error.AuthenticationExpired) {
            // Token reached GitHub but was rejected: bad/expired/missing scope.
            zig_github_disconnect();
            gui_update_github_status(0, "Token rejected. Needs 'repo' scope.");
        } else {
            gui_update_github_status(0, "Cannot reach GitHub. Check connection.");
        }
        return;
    };

    // A classic token advertises its scopes. If it has scopes but not `repo`,
    // it can read /user yet 404 on private repos — reject it now with a clear
    // message instead of a confusing sync failure later. (Empty = fine-grained
    // token, which doesn't report scopes here; allow it and let sync verify.)
    const scopes = github_token_scopes(allocator, auth) catch "";
    std.debug.print("GitHub token scopes: '{s}'\n", .{scopes});
    if (scopes.len > 0 and std.mem.indexOf(u8, scopes, "repo") == null) {
        zig_github_disconnect();
        gui_update_github_status(0, "Token lacks 'repo' scope. Make a new one.");
        return;
    }

    gui_update_github_status(1, "Connected");
}

// ─────────────────────────────────────────────────────────────
// Native GitHub sync via the Contents API (no shell script, no git
// binary). 3-way conflict detection keyed on blob SHAs stored in
// github_sync_meta. Both-changed → _conflict copy, remote wins.
// ─────────────────────────────────────────────────────────────

const GhUser = struct { login: []const u8 = "" };
const GhEntry = struct {
    name: []const u8 = "",
    sha: []const u8 = "",
    type: []const u8 = "",
};
const GhPutResp = struct {
    content: struct { sha: []const u8 = "" } = .{},
};

const GhRemote = struct { sha: []u8 };

fn gh_headers(auth: []const u8) [5]std.http.Header {
    return .{
        .{ .name = "Authorization", .value = auth },
        .{ .name = "Accept", .value = "application/vnd.github+json" },
        .{ .name = "User-Agent", .value = "qirtas-sync" },
        .{ .name = "X-GitHub-Api-Version", .value = "2022-11-28" },
        .{ .name = "Accept-Encoding", .value = "identity" },
    };
}

/// GET /user and return the value of the `X-OAuth-Scopes` response header
/// (the scopes a *classic* token was granted, comma-separated). Returns "" if
/// the header is absent — fine-grained tokens (`github_pat_…`) don't send it,
/// so an empty result means "can't tell from here", not "no access". Caller frees.
fn github_token_scopes(allocator: std.mem.Allocator, auth: []const u8) ![]u8 {
    var client = std.http.Client{ .allocator = allocator, .io = main.global_io };
    defer client.deinit();
    const uri = try std.Uri.parse("https://api.github.com/user");
    const hdrs = gh_headers(auth);
    var req = try client.request(.GET, uri, .{
        .extra_headers = &hdrs,
        .headers = .{ .accept_encoding = .omit },
    });
    defer req.deinit();
    try req.sendBodiless();

    var redirect_buf: [2048]u8 = undefined;
    var response = try req.receiveHead(&redirect_buf);

    // Capture the header BEFORE reading the body (head strings are invalidated
    // by subsequent stream consumption).
    var scopes: []u8 = try allocator.dupe(u8, "");
    var it = std.http.HeaderIterator.init(response.head.bytes);
    while (it.next()) |h| {
        if (std.ascii.eqlIgnoreCase(h.name, "x-oauth-scopes")) {
            allocator.free(scopes);
            scopes = try allocator.dupe(u8, h.value);
            break;
        }
    }

    // Drain the body so the connection can be cleanly closed.
    var transfer_buf: [1024]u8 = undefined;
    const rdr = response.reader(&transfer_buf);
    while (true) {
        var chunk: [1024]u8 = undefined;
        const n = rdr.readSliceShort(&chunk) catch break;
        if (n == 0) break;
    }
    return scopes;
}

fn github_owner_login(allocator: std.mem.Allocator, auth: []const u8) ![]u8 {
    const hdrs = gh_headers(auth);
    const res = try http_fetch(allocator, .GET, "https://api.github.com/user", &hdrs, null);
    defer allocator.free(res.body);
    std.debug.print("GitHub /user status={} body_len={} body_prefix={s}\n", .{ res.status, res.body.len, res.body[0..@min(120, res.body.len)] });
    if (res.status == .unauthorized or res.status == .forbidden) return error.AuthenticationExpired;
    if (res.status != .ok) return error.GithubUserFailed;
    const parsed = std.json.parseFromSlice(GhUser, allocator, res.body, .{ .ignore_unknown_fields = true }) catch |err| {
        std.debug.print("GitHub /user JSON parse failed {}: body={s}\n", .{ err, res.body });
        return err;
    };
    defer parsed.deinit();
    if (parsed.value.login.len == 0) return error.GithubUserFailed;
    return allocator.dupe(u8, parsed.value.login);
}

/// Returns error.GithubRepoCreateForbidden when the OAuth App lacks repo-creation
/// permission. Caller should surface this to the user.
fn github_ensure_repo(allocator: std.mem.Allocator, auth: []const u8, repo_name: []const u8) !void {
    var base = gh_headers(auth);
    var hdrs: [6]std.http.Header = undefined;
    @memcpy(hdrs[0..5], &base);
    hdrs[5] = .{ .name = "Content-Type", .value = "application/json" };
    const body = try std.fmt.allocPrint(allocator,
        "{{\"name\":\"{s}\",\"private\":true,\"auto_init\":true}}", .{repo_name});
    defer allocator.free(body);
    const res = try http_fetch(allocator, .POST, "https://api.github.com/user/repos", &hdrs, body);
    defer allocator.free(res.body);
    // 201 = created, 422 = already exists — both fine
    if (res.status == .created or res.status == .unprocessable_entity) return;
    if (res.status == .forbidden or res.status == .unauthorized) return error.GithubRepoCreateForbidden;
    // Any other non-success: log and return the forbidden sentinel so caller
    // can surface a helpful "create the repo manually" message.
    std.debug.print("GitHub ensure_repo unexpected status={} body={s}\n", .{ res.status, res.body[0..@min(120, res.body.len)] });
    return error.GithubRepoCreateForbidden;
}

fn github_remote_list(allocator: std.mem.Allocator, auth: []const u8, owner: []const u8, repo: []const u8, vault_id: []const u8) !std.StringHashMap(GhRemote) {
    var map = std.StringHashMap(GhRemote).init(allocator);
    errdefer map.deinit();
    const hdrs = gh_headers(auth);
    const url = try std.fmt.allocPrint(allocator, "https://api.github.com/repos/{s}/{s}/contents/{s}", .{ owner, repo, vault_id });
    defer allocator.free(url);
    const res = try http_fetch(allocator, .GET, url, &hdrs, null);
    defer allocator.free(res.body);
    std.debug.print("GitHub remote_list status={} body_len={} body_prefix={s}\n", .{ res.status, res.body.len, res.body[0..@min(120, res.body.len)] });
    if (res.status == .not_found) return map; // vault subfolder doesn't exist yet
    if (res.status == .unauthorized or res.status == .forbidden) return error.AuthenticationExpired;
    if (res.status != .ok) return error.GithubListFailed;

    const parsed = std.json.parseFromSlice([]GhEntry, allocator, res.body, .{ .ignore_unknown_fields = true }) catch |err| {
        std.debug.print("GitHub remote_list JSON parse failed {}: body={s}\n", .{ err, res.body });
        return err;
    };
    defer parsed.deinit();
    for (parsed.value) |e| {
        if (!std.mem.eql(u8, e.type, "file")) continue;
        if (!is_syncable_file(e.name)) continue;
        const key = try allocator.dupe(u8, e.name);
        try map.put(key, .{ .sha = try allocator.dupe(u8, e.sha) });
    }
    return map;
}

fn github_download_raw(allocator: std.mem.Allocator, auth: []const u8, owner: []const u8, repo: []const u8, vault_id: []const u8, filename: []const u8) ![]u8 {
    const enc = try percent_encode(allocator, filename);
    defer allocator.free(enc);
    const url = try std.fmt.allocPrint(allocator, "https://api.github.com/repos/{s}/{s}/contents/{s}/{s}", .{ owner, repo, vault_id, enc });
    defer allocator.free(url);
    const hdrs = [_]std.http.Header{
        .{ .name = "Authorization", .value = auth },
        .{ .name = "Accept", .value = "application/vnd.github.raw" },
        .{ .name = "User-Agent", .value = "qirtas-sync" },
        .{ .name = "X-GitHub-Api-Version", .value = "2022-11-28" },
    };
    const res = try http_fetch(allocator, .GET, url, &hdrs, null);
    if (res.status != .ok) {
        defer allocator.free(res.body);
        std.debug.print("GitHub download {s} failed {}\n", .{ filename, res.status });
        return error.GithubDownloadFailed;
    }
    return res.body;
}

/// PUT a file; existing_sha empty = create. Returns the new blob sha.
fn github_upload(allocator: std.mem.Allocator, auth: []const u8, owner: []const u8, repo: []const u8, vault_id: []const u8, filename: []const u8, content: []const u8, existing_sha: []const u8) ![]u8 {
    const enc = try percent_encode(allocator, filename);
    defer allocator.free(enc);
    const url = try std.fmt.allocPrint(allocator, "https://api.github.com/repos/{s}/{s}/contents/{s}/{s}", .{ owner, repo, vault_id, enc });
    defer allocator.free(url);

    const b64_len = std.base64.standard.Encoder.calcSize(content.len);
    const b64 = try allocator.alloc(u8, b64_len);
    defer allocator.free(b64);
    _ = std.base64.standard.Encoder.encode(b64, content);

    const body = if (existing_sha.len > 0)
        try std.fmt.allocPrint(allocator,
            "{{\"message\":\"qirtas sync\",\"content\":\"{s}\",\"sha\":\"{s}\"}}", .{ b64, existing_sha })
    else
        try std.fmt.allocPrint(allocator,
            "{{\"message\":\"qirtas sync\",\"content\":\"{s}\"}}", .{b64});
    defer allocator.free(body);

    var hdrs = gh_headers(auth);
    var put_hdrs: [6]std.http.Header = undefined;
    @memcpy(put_hdrs[0..5], &hdrs);
    put_hdrs[5] = .{ .name = "Content-Type", .value = "application/json" };

    // A repo that ensure_repo just auto-init'd isn't always immediately writable
    // via the Contents API — the first PUT can 404 for a second or two. Retry a
    // few times with backoff before giving up, so the user doesn't have to press
    // Sync Now twice on a brand-new repo.
    var attempt: u32 = 0;
    while (true) : (attempt += 1) {
        const res = try http_fetch(allocator, .PUT, url, &put_hdrs, body);
        if (res.status == .ok or res.status == .created) {
            defer allocator.free(res.body);
            const parsed = try std.json.parseFromSlice(GhPutResp, allocator, res.body, .{ .ignore_unknown_fields = true });
            defer parsed.deinit();
            return allocator.dupe(u8, parsed.value.content.sha);
        }
        const status = res.status;
        const retriable = status == .not_found and attempt < 4;
        if (!retriable)
            std.debug.print("GitHub upload {s} failed {}: {s}\n", .{ filename, status, res.body });
        allocator.free(res.body);
        if (status == .conflict) return error.GithubShaConflict;
        if (retriable) {
            _ = c.usleep((attempt + 1) * 1500 * 1000); // 1.5s, 3s, 4.5s, 6s
            continue;
        }
        if (status == .not_found) return error.GithubRepoNotFound;
        return error.GithubUploadFailed;
    }
}

/// Accept whatever the user pastes into the repo field: a bare name
/// (`qirtas-notes`), `owner/repo`, or a full URL
/// (`https://github.com/owner/repo`, with or without `.git`). Returns the
/// `owner/repo` or `repo` slice (a view into the input), stripped of scheme,
/// host, and trailing junk.
fn normalize_repo_input(repo_in: []const u8) []const u8 {
    var s = std.mem.trim(u8, repo_in, " \t\r\n/");
    inline for (.{ "https://", "http://" }) |scheme| {
        if (std.mem.startsWith(u8, s, scheme)) {
            s = s[scheme.len..];
            break;
        }
    }
    inline for (.{ "www.github.com/", "github.com/" }) |host| {
        if (std.mem.startsWith(u8, s, host)) {
            s = s[host.len..];
            break;
        }
    }
    if (std.mem.endsWith(u8, s, ".git")) s = s[0 .. s.len - 4];
    return std.mem.trim(u8, s, " \t\r\n/");
}

/// GitHub repo names allow only [A-Za-z0-9._-]. A space (e.g. a user typing
/// "qirtas sync") produces a malformed URL — GitHub answers 400 Bad Request
/// with an HTML body. Map spaces to '-', drop anything else invalid. An empty
/// result falls back to the default repo name.
fn sanitize_repo_name(allocator: std.mem.Allocator, name: []const u8) ![]u8 {
    var out = try allocator.alloc(u8, name.len);
    errdefer allocator.free(out);
    var n: usize = 0;
    for (name) |ch| {
        const ok = (ch >= 'A' and ch <= 'Z') or (ch >= 'a' and ch <= 'z') or
            (ch >= '0' and ch <= '9') or ch == '.' or ch == '_' or ch == '-';
        if (ok) {
            out[n] = ch;
            n += 1;
        } else if (ch == ' ') {
            out[n] = '-';
            n += 1;
        }
        // any other char (slash already handled by the caller, etc.) is dropped
    }
    if (n == 0) {
        allocator.free(out);
        return allocator.dupe(u8, "qirtas-notes");
    }
    return allocator.realloc(out, n);
}

fn github_sync_impl(allocator: std.mem.Allocator, token: []const u8, repo_in: []const u8) !u32 {
    var conflicts: u32 = 0;

    const auth = try std.fmt.allocPrint(allocator, "Bearer {s}", .{token});
    defer allocator.free(auth);

    // Accept bare name, owner/repo, or a full GitHub URL.
    const repo_spec = normalize_repo_input(repo_in);

    // owner/repo resolution mirrors the old script
    var owner: []u8 = undefined;
    var repo: []const u8 = undefined;
    if (std.mem.indexOfScalar(u8, repo_spec, '/')) |slash| {
        owner = try allocator.dupe(u8, repo_spec[0..slash]);
        repo = repo_spec[slash + 1 ..];
    } else {
        owner = try github_owner_login(allocator, auth);
        repo = if (repo_spec.len == 0 or std.mem.eql(u8, repo_spec, owner)) "qirtas-notes" else repo_spec;
    }
    defer allocator.free(owner);

    // Normalize the repo name to a valid GitHub slug before it touches any URL.
    const repo_clean = try sanitize_repo_name(allocator, repo);
    defer allocator.free(repo_clean);
    repo = repo_clean;

    // Ignore repo-creation failures — the repo may already exist (user created it manually).
    // If it truly doesn't exist, the first upload will fail with GithubRepoNotFound.
    github_ensure_repo(allocator, auth, repo) catch {};

    var vid_buf: [256]u8 = undefined;
    const vault_id = vault_id_hex(&vid_buf);

    var remote_map = try github_remote_list(allocator, auth, owner, repo, vault_id);
    defer remote_map.deinit();
    var local_map = try collect_local_files(allocator);
    defer local_map.deinit();

    var local_it = local_map.iterator();
    while (local_it.next()) |entry| {
        const filename = entry.key_ptr.*;
        const local_mtime = entry.value_ptr.*;
        const scoped_name = try scoped_key(allocator, vault_id, filename);
        defer allocator.free(scoped_name);

        if (remote_map.get(filename)) |remote| {
            const meta = sync_meta_get("github_sync_meta", allocator, scoped_name);
            defer if (meta) |m| allocator.free(m.remote_marker);

            const stored_local: i64 = if (meta) |m| m.local_mtime else -1;
            const stored_sha: []const u8 = if (meta) |m| m.remote_marker else "";
            const local_changed = local_mtime != stored_local;
            const remote_changed = !std.mem.eql(u8, remote.sha, stored_sha);

            if (local_changed and remote_changed) {
                const remote_content = try github_download_raw(allocator, auth, owner, repo, vault_id, filename);
                defer allocator.free(remote_content);
                const local_content = try read_file_content(allocator, filename);
                defer allocator.free(local_content);

                if (!std.mem.eql(u8, remote_content, local_content)) {
                    try preserve_local_as_conflict(allocator, filename);
                    conflicts += 1;
                    try write_file_mmap(filename, remote_content);
                    const fz = try allocator.dupeZ(u8, filename);
                    defer allocator.free(fz);
                    gui_index_file(fz.ptr);
                    main.alert_file_updated();
                }
                const new_st = stat_path(filename) orelse continue;
                sync_meta_set("github_sync_meta", allocator, scoped_name, new_st.st_mtim.tv_sec, remote.sha);
            } else if (local_changed) {
                const local_content = try read_file_content(allocator, filename);
                defer allocator.free(local_content);
                const new_sha = try github_upload(allocator, auth, owner, repo, vault_id, filename, local_content, remote.sha);
                defer allocator.free(new_sha);
                sync_meta_set("github_sync_meta", allocator, scoped_name, local_mtime, new_sha);
            } else if (remote_changed) {
                const remote_content = try github_download_raw(allocator, auth, owner, repo, vault_id, filename);
                defer allocator.free(remote_content);
                try write_file_mmap(filename, remote_content);
                const fz = try allocator.dupeZ(u8, filename);
                defer allocator.free(fz);
                gui_index_file(fz.ptr);
                main.alert_file_updated();
                const new_st = stat_path(filename) orelse continue;
                sync_meta_set("github_sync_meta", allocator, scoped_name, new_st.st_mtim.tv_sec, remote.sha);
            } else if (meta == null) {
                sync_meta_set("github_sync_meta", allocator, scoped_name, local_mtime, remote.sha);
            }
        } else {
            const local_content = try read_file_content(allocator, filename);
            defer allocator.free(local_content);
            const new_sha = try github_upload(allocator, auth, owner, repo, vault_id, filename, local_content, "");
            defer allocator.free(new_sha);
            sync_meta_set("github_sync_meta", allocator, scoped_name, local_mtime, new_sha);
        }
    }

    var remote_it = remote_map.iterator();
    while (remote_it.next()) |entry| {
        const filename = entry.key_ptr.*;
        if (local_map.contains(filename)) continue;
        const remote_content = try github_download_raw(allocator, auth, owner, repo, vault_id, filename);
        defer allocator.free(remote_content);
        try write_file_mmap(filename, remote_content);
        const fz = try allocator.dupeZ(u8, filename);
        defer allocator.free(fz);
        gui_index_file(fz.ptr);
        const new_st = stat_path(filename) orelse continue;
        const scoped_name = try scoped_key(allocator, vault_id, filename);
        defer allocator.free(scoped_name);
        sync_meta_set("github_sync_meta", allocator, scoped_name, new_st.st_mtim.tv_sec, entry.value_ptr.sha);
    }

    return conflicts;
}

fn github_sync_worker() void {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    const creds = get_github_credentials(allocator) catch {
        gui_update_github_status(0, "Error: missing credentials.");
        return;
    };
    defer {
        @memset(@constCast(creds.token), 0);
        allocator.free(creds.token);
        allocator.free(creds.repo);
    }

    const conflicts = github_sync_impl(allocator, creds.token, creds.repo) catch |err| {
        std.debug.print("GitHub sync failed: {}\n", .{err});
        if (err == error.AuthenticationExpired) {
            zig_github_disconnect();
            gui_update_github_status(0, "Error: auth expired. reconnect.");
            return;
        }
        var msg_buf: [128]u8 = undefined;
        gui_update_github_status(1, sync_error_message_z(err, &msg_buf));
        return;
    };

    gui_refresh_explorer();
    if (conflicts > 0) {
        var buf: [64]u8 = undefined;
        const msg = std.fmt.bufPrintZ(&buf, "Synced ✓ ({d} conflict{s} saved)", .{ conflicts, if (conflicts == 1) "" else "s" }) catch "Synced ✓";
        gui_update_github_status(1, msg);
    } else {
        gui_update_github_status(1, "Synced ✓");
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

test "percent_encode path segments" {
    const allocator = std.testing.allocator;
    const a = try percent_encode(allocator, "notes file.md");
    defer allocator.free(a);
    try std.testing.expectEqualStrings("notes%20file.md", a);
    const b = try percent_encode(allocator, "ملف.md");
    defer allocator.free(b);
    try std.testing.expectEqualStrings("%D9%85%D9%84%D9%81.md", b);
    const c_ = try percent_encode(allocator, "safe-name_1.2~x");
    defer allocator.free(c_);
    try std.testing.expectEqualStrings("safe-name_1.2~x", c_);
}

test "json_escape_header ascii-safe output" {
    const allocator = std.testing.allocator;
    const a = try json_escape_header(allocator, "قرطاس \"x\"\\y.md");
    defer allocator.free(a);
    // all bytes must be printable ASCII
    for (a) |ch| try std.testing.expect(ch >= 0x20 and ch < 0x7F);
    try std.testing.expectEqualStrings("\\u0642\\u0631\\u0637\\u0627\\u0633 \\\"x\\\"\\\\y.md", a);
    // astral plane → surrogate pair
    const b = try json_escape_header(allocator, "📝");
    defer allocator.free(b);
    try std.testing.expectEqualStrings("\\uD83D\\uDCDD", b);
}

test "i64_to_text round-trip" {
    var buf: [24]u8 = undefined;
    const t = i64_to_text(&buf, 1781222400);
    try std.testing.expectEqual(@as(i64, 1781222400), try std.fmt.parseInt(i64, t, 10));
}
