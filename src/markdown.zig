//! Portable, UI-agnostic markdown/text logic. No GTK, no allocator state —
//! pure functions over byte slices. The Linux GTK UI calls these over the C
//! ABI; a future Android/Kotlin UI calls the same exports over JNI. See
//! docs/PORTABILITY.md. New markdown/text parsing belongs HERE, not in C.

const std = @import("std");

/// True if `c` falls in an Arabic Unicode block (Arabic, Supplement, Extended-A,
/// Presentation Forms A/B). Same ranges as the old C is_arabic_char.
pub fn isArabicChar(c: u21) bool {
    return (c >= 0x0600 and c <= 0x06FF) or
        (c >= 0x0750 and c <= 0x077F) or
        (c >= 0x08A0 and c <= 0x08FF) or
        (c >= 0xFB50 and c <= 0xFDFF) or
        (c >= 0xFE70 and c <= 0xFEFF);
}

/// Paragraph direction by first strong character: RTL if the first strong char
/// is Arabic, LTR if it is an ASCII letter. Everything else — whitespace,
/// digits, punctuation, markdown syntax (`#`, `*`, `-`, …), and other scripts —
/// is not decisive and is skipped, so "## عنوان" and "123 مرحبا" are still RTL.
/// Malformed UTF-8 bytes are skipped rather than trapped.
pub fn detectRtl(text: []const u8) bool {
    var i: usize = 0;
    while (i < text.len) {
        const len = std.unicode.utf8ByteSequenceLength(text[i]) catch {
            i += 1;
            continue;
        };
        if (i + len > text.len) break;
        const c = std.unicode.utf8Decode(text[i .. i + len]) catch {
            i += 1;
            continue;
        };
        i += len;
        if (isArabicChar(c)) return true;
        if ((c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z')) return false;
    }
    return false;
}

/// C-ABI entry point for the GTK UI (gui_rtl.c). Returns 1 for RTL, 0 for LTR,
/// matching the codebase's c_int boolean convention.
pub export fn zig_detect_rtl(text: ?[*:0]const u8) callconv(.c) c_int {
    const t = text orelse return 0;
    return if (detectRtl(std.mem.span(t))) 1 else 0;
}

test "isArabicChar covers Arabic blocks, rejects Latin/digits" {
    try std.testing.expect(isArabicChar('م'));
    try std.testing.expect(isArabicChar('ع'));
    try std.testing.expect(!isArabicChar('a'));
    try std.testing.expect(!isArabicChar('5'));
}

test "detectRtl: first strong char decides direction" {
    try std.testing.expect(!detectRtl("hello world"));
    try std.testing.expect(detectRtl("مرحبا بالعالم"));
    // markdown syntax + leading spaces/digits are skipped
    try std.testing.expect(detectRtl("## عنوان"));
    try std.testing.expect(detectRtl("123  مرحبا"));
    try std.testing.expect(!detectRtl("- [ ] task في"));
    // no strong char → LTR default
    try std.testing.expect(!detectRtl(""));
    try std.testing.expect(!detectRtl("123 ### --- |"));
    // mixed: Latin first wins LTR even with Arabic later
    try std.testing.expect(!detectRtl("note: ملاحظة"));
}
