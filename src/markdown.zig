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

// ---- shared UTF-8 helper ------------------------------------------------

const Cp = struct { cp: u21, len: usize };

/// Decode the codepoint at byte `i`. Malformed bytes decode as themselves
/// (len 1) rather than trapping, matching GLib's lenient g_utf8_* behaviour.
fn decodeAt(s: []const u8, i: usize) Cp {
    const len = std.unicode.utf8ByteSequenceLength(s[i]) catch return .{ .cp = s[i], .len = 1 };
    if (i + len > s.len) return .{ .cp = s[i], .len = 1 };
    const cp = std.unicode.utf8Decode(s[i .. i + len]) catch return .{ .cp = s[i], .len = 1 };
    return .{ .cp = cp, .len = len };
}

fn lowerCp(c: u21) u21 {
    return if (c >= 'A' and c <= 'Z') c + 32 else c;
}

// ---- headings -----------------------------------------------------------

/// ATX heading level (1-6) of a line, or 0 if it is not a heading. Requires
/// 1-6 leading '#', a space, then at least one more character.
pub fn headingLevel(line: []const u8) u8 {
    var level: usize = 0;
    while (level < line.len and line[level] == '#') level += 1;
    if (level >= 1 and level <= 6 and level + 1 < line.len and line[level] == ' ')
        return @intCast(level);
    return 0;
}

pub export fn zig_heading_level(line: ?[*:0]const u8) callconv(.c) c_int {
    const t = line orelse return 0;
    return headingLevel(std.mem.span(t));
}

// ---- fuzzy quick-open ranking -------------------------------------------

/// Subsequence fuzzy score of `needle` against `haystack`: rewards
/// consecutive runs and matches at word boundaries (space/-/_/./'/'). Returns
/// -1 if `needle` is not a subsequence. Case-folds ASCII (Arabic is caseless,
/// so this matches g_unichar_tolower for paths in practice).
pub fn fuzzyScore(haystack: []const u8, needle: []const u8) i32 {
    if (needle.len == 0) return 0;
    var score: i32 = 0;
    var run: i32 = 0;
    var prev_boundary = true;
    var hi: usize = 0;
    var ni: usize = 0;
    while (hi < haystack.len and ni < needle.len) {
        const h = decodeAt(haystack, hi);
        const n = decodeAt(needle, ni);
        const hc = lowerCp(h.cp);
        const nc = lowerCp(n.cp);
        if (hc == nc) {
            score += 1 + run * 2 + (if (prev_boundary) @as(i32, 3) else 0);
            run += 1;
            ni += n.len;
        } else {
            run = 0;
        }
        prev_boundary = (hc == ' ' or hc == '-' or hc == '_' or hc == '/' or hc == '.');
        hi += h.len;
    }
    return if (ni >= needle.len) score else -1;
}

pub export fn zig_fuzzy_score(haystack: ?[*:0]const u8, needle: ?[*:0]const u8) callconv(.c) c_int {
    const h = haystack orelse return -1;
    const n = needle orelse return 0;
    return fuzzyScore(std.mem.span(h), std.mem.span(n));
}

// ---- Arabic-tolerant search regex ---------------------------------------

fn isRegexMeta(c: u8) bool {
    return switch (c) {
        '.', '*', '+', '?', '^', '$', '{', '}', '(', ')', '|', '[', ']', '\\' => true,
        else => false,
    };
}

fn put(buf: []u8, w: *usize, bytes: []const u8) void {
    @memcpy(buf[w.* .. w.* + bytes.len], bytes);
    w.* += bytes.len;
}

/// Build an Arabic-tolerant search regex (PCRE `\x{…}` syntax, for GRegex /
/// GtkSourceSearch). Unifies alef/teh-marbuta/yaa variants into character
/// classes, strips tashkeel from the query, and allows optional tashkeel in the
/// target after each Arabic letter. The caller must NFKC-normalize first (GLib
/// on Linux, java.text.Normalizer on Android) — that platform step is the only
/// non-portable piece. Returns a heap string owned by `alloc`.
pub fn arabicSearchRegex(alloc: std.mem.Allocator, input: []const u8) ![:0]u8 {
    // Worst case per input byte: ~12 (char class) + 28 (tashkeel suffix).
    const buf = try alloc.alloc(u8, input.len * 48 + 16);
    defer alloc.free(buf);
    var w: usize = 0;
    var i: usize = 0;
    while (i < input.len) {
        const d = decodeAt(input, i);
        i += d.len;
        const c = d.cp;
        // strip tashkeel from the query
        if ((c >= 0x064B and c <= 0x065F) or c == 0x0670) continue;

        if (c < 128 and isRegexMeta(@intCast(c))) {
            buf[w] = '\\';
            buf[w + 1] = @intCast(c);
            w += 2;
        } else if (c == 0x0627 or c == 0x0623 or c == 0x0625 or c == 0x0622 or c == 0x0671) {
            put(buf, &w, "[اأإآٱ]");
        } else if (c == 0x0629 or c == 0x0647) {
            put(buf, &w, "[ةه]");
        } else if (c == 0x064A or c == 0x0649) {
            put(buf, &w, "[يى]");
        } else {
            var tmp: [4]u8 = undefined;
            const n = std.unicode.utf8Encode(c, &tmp) catch 0;
            put(buf, &w, tmp[0..n]);
        }
        // allow optional tashkeel in the target after any Arabic letter
        if (c >= 0x0600 and c <= 0x06FF)
            put(buf, &w, "[\\x{064B}-\\x{065F}\\x{0670}]*");
    }
    return alloc.dupeZ(u8, buf[0..w]);
}

/// C-ABI: returns a page_allocator string; free with zig_free_document_text.
/// Pass NFKC-normalized input (see gui_search.c).
pub export fn zig_arabic_search_regex(input: ?[*:0]const u8) callconv(.c) ?[*:0]u8 {
    const t = input orelse return null;
    const r = arabicSearchRegex(std.heap.page_allocator, std.mem.span(t)) catch return null;
    return r.ptr;
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

test "headingLevel: ATX levels 1-6, needs space + content" {
    try std.testing.expectEqual(@as(u8, 1), headingLevel("# Title"));
    try std.testing.expectEqual(@as(u8, 3), headingLevel("### Sub"));
    try std.testing.expectEqual(@as(u8, 6), headingLevel("###### Deep"));
    try std.testing.expectEqual(@as(u8, 0), headingLevel("####### too deep"));
    try std.testing.expectEqual(@as(u8, 0), headingLevel("#no space"));
    try std.testing.expectEqual(@as(u8, 0), headingLevel("## ")); // no content
    try std.testing.expectEqual(@as(u8, 0), headingLevel("plain text"));
    try std.testing.expectEqual(@as(u8, 2), headingLevel("## عنوان"));
}

test "fuzzyScore: subsequence with run + boundary bonuses" {
    try std.testing.expect(fuzzyScore("readme.md", "rme") >= 0);
    try std.testing.expect(fuzzyScore("readme.md", "xyz") == -1);
    try std.testing.expectEqual(@as(i32, 0), fuzzyScore("anything", ""));
    // consecutive + boundary scores higher than scattered
    try std.testing.expect(fuzzyScore("my-notes.md", "notes") > fuzzyScore("my-notes.md", "mnts"));
    // case-insensitive
    try std.testing.expect(fuzzyScore("README", "readme") >= 0);
}

test "arabicSearchRegex: escapes meta, unifies Arabic, strips tashkeel" {
    const a = std.testing.allocator;
    const r1 = try arabicSearchRegex(a, "a.b");
    defer a.free(r1);
    try std.testing.expectEqualStrings("a\\.b", r1);

    // alef → class + tashkeel-tolerant suffix
    const r2 = try arabicSearchRegex(a, "ا");
    defer a.free(r2);
    try std.testing.expectEqualStrings("[اأإآٱ][\\x{064B}-\\x{065F}\\x{0670}]*", r2);

    // tashkeel in the query is dropped (fatha 0x064E)
    const r3 = try arabicSearchRegex(a, "بَ");
    defer a.free(r3);
    try std.testing.expect(std.mem.indexOf(u8, r3, "\\x{064E}") == null);
    try std.testing.expect(std.mem.startsWith(u8, r3, "ب"));

    const r4 = try arabicSearchRegex(a, "");
    defer a.free(r4);
    try std.testing.expectEqualStrings("", r4);
}
