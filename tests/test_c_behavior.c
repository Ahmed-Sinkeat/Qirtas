/*
 * Standalone C behavioral tests for pure-logic functions in gui_buffer.c.
 *
 * Tests arabize_digits, arabic_count_phrase, arabic_lines_phrase,
 * and advance_position. None of these require GTK display init — they
 * are pure string / codepoint logic.
 *
 * Build:
 *   zig build test-c
 *
 * Or manually:
 *   cc tests/test_c_behavior.c src/gui/gui_buffer.c -I src \
 *      $(pkg-config --cflags --libs gtk4 gtksourceview-5 libadwaita-1) \
 *      -o /tmp/qirtas-test-c && /tmp/qirtas-test-c
 */

#include "../src/gui_internal.h"
#include <stdio.h>
#include <string.h>

/* ── stubs: Zig-exported symbols ──────────────────────────────────────────
 * gui_buffer.c references these at link time. None are reachable from the
 * pure-logic functions under test, but the linker requires them resolved. */

AppGui *global_gui = NULL;

const char *zig_db_path(void)                                        { return ":memory:"; }
const char *zig_get_text_for_line_range(int s, int e, int *l)        { (void)s;(void)e;(void)l; return ""; }
void        zig_insert_text(Position p, const char *t)               { (void)p;(void)t; }
void        zig_delete_range(Position s, Position e)                 { (void)s;(void)e; }
void        zig_replace_range(Position s, Position e, const char *t) { (void)s;(void)e;(void)t; }
void        zig_undo_push(int l, int c)                              { (void)l;(void)c; }
void        zig_undo_commit(void)                                    {}
int         zig_heading_level(const char *l)                         { (void)l; return 0; }

/* ── stubs: other gui_*.c symbols ────────────────────────────────────────*/
void        gui_get_cursor_position(int *l, int *c)                  { *l = 0; *c = 0; }
void        gui_set_cursor_position(int l, int c)                    { (void)l;(void)c; }
void        gui_set_cursor_position_quiet(int l, int c)              { (void)l;(void)c; }
void        gui_outline_refresh(AppGui *g)                           { (void)g; }
gboolean    gui_autosave_enabled(void)                               { return FALSE; }
void        gui_trigger_autosave(void)                               {}
void        gui_set_sync_state(QirtasSyncState s)                    { (void)s; }
void        gui_show_toast(const char *m)                            { (void)m; }
void        gui_reload_full_buffer(void)                             {}

/* ── test helpers ─────────────────────────────────────────────────────────*/
static int g_passed = 0, g_failed = 0;

#define EXPECT_STR(got, want) do {                                      \
    const char *_g = (got); const char *_w = (want);                   \
    if (strcmp(_g, _w) == 0) { g_passed++; }                           \
    else { printf("FAIL %s:%d\n  got  : [%s]\n  want : [%s]\n",        \
                  __FILE__, __LINE__, _g, _w); g_failed++; }           \
} while(0)

#define EXPECT_INT(label, got, want) do {                               \
    int _g = (int)(got); int _w = (int)(want);                         \
    if (_g == _w) { g_passed++; }                                       \
    else { printf("FAIL %s:%d  %s: got %d  want %d\n",                 \
                  __FILE__, __LINE__, (label), _g, _w); g_failed++; }  \
} while(0)

/* ── forward declarations ─────────────────────────────────────────────────*/
void     arabize_digits(const char *in, char *out, size_t out_size);
void     arabic_count_phrase(long n, gboolean feminine, char *out, size_t out_size);
void     arabic_lines_phrase(long n, char *out, size_t out_size);
Position advance_position(Position pos, const char *text);

/* ── arabize_digits ───────────────────────────────────────────────────────*/
static void test_arabize_digits(void) {
    char buf[64];

    arabize_digits("0123456789", buf, sizeof buf);
    /* 0-9 → ٠-٩ (U+0660..U+0669, each 2 bytes: 0xD9 0xA0..0xA9) */
    EXPECT_STR(buf,
        "\xD9\xA0\xD9\xA1\xD9\xA2\xD9\xA3\xD9\xA4"
        "\xD9\xA5\xD9\xA6\xD9\xA7\xD9\xA8\xD9\xA9");

    arabize_digits("abc", buf, sizeof buf);
    EXPECT_STR(buf, "abc");

    arabize_digits("page 42", buf, sizeof buf);
    EXPECT_STR(buf, "page \xD9\xA4\xD9\xA2");  /* page ٤٢ */

    arabize_digits("", buf, sizeof buf);
    EXPECT_STR(buf, "");
}

/* ── arabic_count_phrase ──────────────────────────────────────────────────*/
static void test_arabic_count_phrase(void) {
    char buf[128];

    /* feminine = TRUE → counting كلمة (words, feminine noun) */
    arabic_count_phrase(1, TRUE, buf, sizeof buf);
    EXPECT_STR(buf, "\xD9\x83\xD9\x84\xD9\x85\xD8\xA9 \xD9\x88\xD8\xA7\xD8\xAD\xD8\xAF\xD8\xA9");
    /* كلمة واحدة */

    arabic_count_phrase(2, TRUE, buf, sizeof buf);
    EXPECT_STR(buf, "\xD9\x83\xD9\x84\xD9\x85\xD8\xAA\xD8\xA7\xD9\x86");
    /* كلمتان */

    /* Gender polarity (n 3-10): feminine noun → number without ة (ست) */
    arabic_count_phrase(6, TRUE, buf, sizeof buf);
    EXPECT_STR(buf, "\xD8\xB3\xD8\xAA \xD9\x83\xD9\x84\xD9\x85\xD8\xA7\xD8\xAA");
    /* ست كلمات */

    /* feminine = FALSE → counting حرف (chars, masculine noun)
     * Gender polarity: masculine noun → number with ة (ستة) */
    arabic_count_phrase(6, FALSE, buf, sizeof buf);
    EXPECT_STR(buf, "\xD8\xB3\xD8\xAA\xD8\xA9 \xD8\xAD\xD8\xB1\xD9\x88\xD9\x81");
    /* ستة حروف */

    /* 11+ → Eastern Arabic numeral + singular */
    arabic_count_phrase(11, TRUE, buf, sizeof buf);
    EXPECT_STR(buf, "\xD9\xA1\xD9\xA1 \xD9\x83\xD9\x84\xD9\x85\xD8\xA9");
    /* ١١ كلمة */

    /* 0 → Eastern Arabic numeral + singular (falls to the else branch) */
    arabic_count_phrase(0, TRUE, buf, sizeof buf);
    EXPECT_STR(buf, "\xD9\xA0 \xD9\x83\xD9\x84\xD9\x85\xD8\xA9");
    /* ٠ كلمة */
}

/* ── arabic_lines_phrase ──────────────────────────────────────────────────*/
static void test_arabic_lines_phrase(void) {
    char buf[128];

    arabic_lines_phrase(1, buf, sizeof buf);
    EXPECT_STR(buf, "\xD8\xB3\xD8\xB7\xD8\xB1 \xD9\x88\xD8\xA7\xD8\xAD\xD8\xAF");
    /* سطر واحد */

    arabic_lines_phrase(2, buf, sizeof buf);
    EXPECT_STR(buf, "\xD8\xB3\xD8\xB7\xD8\xB1\xD8\xA7\xD9\x86");
    /* سطران */

    /* masculine noun (سطر) → number with ة for 3-10 */
    arabic_lines_phrase(3, buf, sizeof buf);
    EXPECT_STR(buf, "\xD8\xAB\xD9\x84\xD8\xA7\xD8\xAB\xD8\xA9 \xD8\xA3\xD8\xB3\xD8\xB7\xD8\xB1");
    /* ثلاثة أسطر */

    arabic_lines_phrase(11, buf, sizeof buf);
    EXPECT_STR(buf, "\xD9\xA1\xD9\xA1 \xD8\xB3\xD8\xB7\xD8\xB1");
    /* ١١ سطر */
}

/* ── advance_position ─────────────────────────────────────────────────────*/
static void test_advance_position(void) {
    Position p;

    p = advance_position((Position){0, 0}, "hello");
    EXPECT_INT("line", p.line, 0);
    EXPECT_INT("col",  p.col,  5);

    p = advance_position((Position){0, 0}, "hi\nbye");
    EXPECT_INT("line", p.line, 1);
    EXPECT_INT("col",  p.col,  3);

    /* Arabic: 5 codepoints, 10 UTF-8 bytes — col counts codepoints */
    p = advance_position((Position){0, 0},
        "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7"); /* مرحبا */
    EXPECT_INT("line", p.line, 0);
    EXPECT_INT("col",  p.col,  5);

    p = advance_position((Position){2, 3}, "!!");
    EXPECT_INT("line", p.line, 2);
    EXPECT_INT("col",  p.col,  5);
}

/* ── main ─────────────────────────────────────────────────────────────────*/
int main(void) {
    test_arabize_digits();
    test_arabic_count_phrase();
    test_arabic_lines_phrase();
    test_advance_position();

    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
