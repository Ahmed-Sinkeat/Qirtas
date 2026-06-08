const std = @import("std");

pub fn main(init: std.process.Init) !void {
    const allocator = std.heap.page_allocator;
    const io = init.io;

    var client = std.http.Client{
        .allocator = allocator,
        .io = io,
    };
    defer client.deinit();

    var file = try std.Io.Dir.cwd().createFile(io, "src/english.txt", .{});
    defer file.close(io);

    var write_buffer: [4096]u8 = undefined;
    var file_writer = file.writer(io, &write_buffer);

    const result = try client.fetch(.{
        .location = .{ .url = "https://raw.githubusercontent.com/bitcoin/bips/master/bip-0039/english.txt" },
        .response_writer = &file_writer.interface,
    });

    if (result.status != .ok) {
        std.debug.print("HTTP status: {}\n", .{result.status});
        return error.HttpError;
    }

    std.debug.print("Successfully fetched and decompressed wordlist!\n", .{});
}
