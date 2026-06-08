const std = @import("std");
const main = @import("main.zig");

test "file encryption/decryption" {
    const key: [32]u8 = .{
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    };

    const plaintext = "standalone crypto test";
    const blob = try main.encryptWithMasterKey(std.testing.allocator, key, plaintext);
    defer std.testing.allocator.free(blob);

    const decrypted = try main.decryptWithMasterKey(std.testing.allocator, key, blob);
    defer std.testing.allocator.free(decrypted);

    try std.testing.expectEqualStrings(plaintext, decrypted);
}
