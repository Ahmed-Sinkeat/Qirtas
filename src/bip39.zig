const std = @import("std");

pub const wordlist = blk: {
    @setEvalBranchQuota(250000);
    const raw_data = @embedFile("english.txt");
    var list: [2048][]const u8 = undefined;
    var it = std.mem.splitScalar(u8, raw_data, '\n');
    var i: usize = 0;
    while (it.next()) |line| {
        const word = std.mem.trim(u8, line, " \r\t");
        if (word.len == 0) continue;
        if (i >= 2048) {
            @compileError("Wordlist has more than 2048 words!");
        }
        list[i] = word;
        i += 1;
    }
    if (i < 2048) {
        @compileError("Wordlist has fewer than 2048 words!");
    }
    break :blk list;
};

pub fn entropyToMnemonic(entropy: [32]u8, out_words: *[24][]const u8) !void {
    // 1. Calculate SHA-256 checksum (first 8 bits of hash)
    var hash: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(&entropy, &hash, .{});
    const checksum = hash[0];

    // 2. Concatenate 256 bits of entropy + 8 bits checksum = 264 bits
    var bits: [264]bool = undefined;
    for (entropy, 0..) |byte, i| {
        for (0..8) |b| {
            bits[i * 8 + b] = ((byte >> @intCast(7 - b)) & 1) == 1;
        }
    }
    for (0..8) |b| {
        bits[256 + b] = ((checksum >> @intCast(7 - b)) & 1) == 1;
    }

    // 3. Split into 24 chunks of 11 bits and map to wordlist indices
    for (0..24) |i| {
        var index: u11 = 0;
        for (0..11) |b| {
            index = (index << 1) | @intFromBool(bits[i * 11 + b]);
        }
        out_words[i] = wordlist[index];
    }
}

pub fn mnemonicToEntropy(words: []const []const u8) ![32]u8 {
    if (words.len != 24) return error.InvalidMnemonicLength;

    // 1. Map words back to 11-bit indices
    var bits: [264]bool = undefined;
    for (words, 0..) |word, i| {
        const index = try findWordIndex(word);
        for (0..11) |b| {
            bits[i * 11 + b] = ((index >> @intCast(10 - b)) & 1) == 1;
        }
    }

    // 2. Extract entropy (first 256 bits)
    var entropy: [32]u8 = undefined;
    for (0..32) |i| {
        var byte: u8 = 0;
        for (0..8) |b| {
            byte = (byte << 1) | @intFromBool(bits[i * 8 + b]);
        }
        entropy[i] = byte;
    }

    // 3. Verify checksum (last 8 bits)
    var hash: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(&entropy, &hash, .{});
    const expected_checksum = hash[0];

    var actual_checksum: u8 = 0;
    for (0..8) |b| {
        actual_checksum = (actual_checksum << 1) | @intFromBool(bits[256 + b]);
    }

    if (expected_checksum != actual_checksum) return error.InvalidChecksum;
    return entropy;
}

fn findWordIndex(word: []const u8) !u11 {
    var low: usize = 0;
    var high: usize = 2047;
    while (low <= high) {
        const mid = low + (high - low) / 2;
        const cmp = std.mem.order(u8, wordlist[mid], word);
        switch (cmp) {
            .eq => return @intCast(mid),
            .lt => low = mid + 1,
            .gt => {
                if (mid == 0) break;
                high = mid - 1;
            },
        }
    }
    return error.WordNotFound;
}

test "BIP-39 entropy to mnemonic and back" {
    const entropy = [_]u8{0x00} ** 32;
    var mnemonic_buf: [24][]const u8 = undefined;
    
    // We expect standard BIP-39 mapping for 32 bytes of zeros to be 23 "abandon" words and the last to be "art" (checksum)
    try entropyToMnemonic(entropy, &mnemonic_buf);
    for (mnemonic_buf[0..23]) |word| {
        try std.testing.expectEqualStrings("abandon", word);
    }
    try std.testing.expectEqualStrings("art", mnemonic_buf[23]);
    
    const parsed_entropy = try mnemonicToEntropy(&mnemonic_buf);
    try std.testing.expectEqualSlices(u8, &entropy, &parsed_entropy);
}

test "BIP-39 round-trip with random entropy" {
    var threaded = std.Io.Threaded.init_single_threaded;
    const io = threaded.io();

    var i: usize = 0;
    while (i < 100) : (i += 1) {
        var entropy: [32]u8 = undefined;
        try io.randomSecure(&entropy);

        var mnemonic_buf: [24][]const u8 = undefined;
        try entropyToMnemonic(entropy, &mnemonic_buf);

        const parsed_entropy = try mnemonicToEntropy(&mnemonic_buf);
        try std.testing.expectEqualSlices(u8, &entropy, &parsed_entropy);
    }
}
