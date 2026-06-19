# Portability plan — grow the Zig core (Windows + Android)

Goal: ship Qirtas on **Linux** (now), **Windows** (later), **Android** (later, Kotlin/Compose UI).
This doc is the standing plan for *how* — read it before adding markdown/text logic.

## The one rule

> **Logic goes in Zig. C only renders.**
> Every editor decoration = **parse (Zig, portable, tested)** + **render (per-platform)**.

```
Zig:      parse_tables(text) → [{row, col, align, cell}]   ← one shared brain
Linux C:  spans → GtkGrid + GtkTextTags
Android:  spans → Jetpack Compose (via JNI)
```

Today the markdown parser lives in **C** (`gui_conceal.c`, `gui_table.c`, `gui_export.c`…), so an Android
Kotlin UI would have to re-implement the whole parser — a second brain that drifts. Moving parsers to Zig
gives every platform one parser.

## Where we are (measured)

| Layer | Lines | % | GTK? |
|---|---|---|---|
| Zig core (`main.zig`, `sync.zig`) | 5,993 | 29% | none |
| C/GTK UI (`gui.c` + `gui/*.c`) | 14,852 | 71% | 2,840 gtk/pango/cairo calls |

FFI seam already exists: Zig exports **63** `zig_*` C-ABI functions; the Zig core calls **24** `gui_*`
callbacks (the UI contract each platform implements). `aarch64-linux-android` is a Zig target; the same
`export fn … callconv(.c)` functions bind from Kotlin with `external fun` over JNI.

Moving the ~2,768 movable lines below grows the portable core to **~42%** and, more importantly, puts the
**markdown parser** in the shared core.

## Platform notes

- **Windows** — GTK4 + libadwaita + GtkSourceView run on Windows via MSYS2 (MINGW64). Build and packaging
  steps live in [`BUILDING-WINDOWS.md`](BUILDING-WINDOWS.md). The Zig core needed a small OS-abstraction
  pass first — it had POSIX-only dependencies: `mmap`→`read`, `getrandom`/`/dev/urandom`→`std.crypto.random`,
  `/etc/machine-id`→registry `MachineGuid`, exe path via `GetModuleFileNameW`, and the inotify file-watcher
  is gated off (no live reload of externally-changed files on Windows yet — a `ReadDirectoryChangesW`
  backend is the follow-up). Everything sits behind `builtin.os.tag` checks, so the Linux build is
  unchanged.
- **Android** — GTK does **not** run on Android. Reuse the Zig core via JNI; build a new Kotlin/Compose UI.
  This plan is what makes that UI cheap (it renders spans, doesn't re-parse markdown).

## Move order (checklist)

Do it **incrementally, one parser per feature** — never big-bang. No Linux-visible change; this is a
portability + testability investment. Each move also becomes a `zig build test-regression` unit (closes the
C-untested gap).

### Tier 1 — pure logic, low risk (done)
Criterion that matters: **pure AND genuinely reused cross-platform** (not just "movable").
- [x] `gui_rtl.c` → `detectRtl` / `isArabicChar` — Arabic core *(markdown.zig)*
- [x] `gui_outline.c` → `headingLevel` (heading parse) *(markdown.zig)*
- [x] `gui_switcher.c` → `fuzzyScore` (quick-open ranking) *(markdown.zig; dir-walk left in C)*
- [x] `gui_search.c` → `arabicSearchRegex` *(markdown.zig; NFKC stays in C — platform-provided)*

Deprioritized after a closer read (movable, but low cross-platform reuse):
- ~~`gui_i18n.c`~~ — pure data, but Android localizes via `res/values/strings.xml`, not a Zig table.
- ~~`gui_shortcuts.c`~~ — GDK-bound (`gdk_keyval_from_name`); Android input handling is unrelated.
- `gui_index.c` (SQLite FTS, ~210 lines) — high value but its own focused task, not a Tier-1 lump-in.

### Tier 2 — the markdown parsers (the real prize, ~1,420 lines)

**Approach: one parser per change, carefully.** Move the *pure predicates and
structure parse* (return bool / ints / a small fixed result); **leave functions
that return GLib string arrays as C glue** until a deliberate array-FFI design —
those feed the widget builder directly and aren't worth a risky boundary yet.

- [x] `gui_table.c` — `is_delimiter_row`, `is_table_row`, column alignment → Zig (`isTableDelimiter`/`isTableRow`/`tableColumnAligns`). `split_row` stays C glue (returns a GPtrArray of cell strings).
- [x] `gui_codeblock.c` → fence/language parse (`fenceOnly`, `codeFenceLang`)
- [ ] `gui_wiki.c` → `[[link]]` parse (~180)
- [ ] `gui_export.c` → `parse_blocks` / `parse_inline` (~155)
- [~] `gui_buffer.c` — heading detect now reuses `headingLevel` (one parser). **Word count stays C** — it depends on Unicode `g_unichar_isspace`/`ispunct` classification (see Constraints); porting ASCII-only would break Arabic word counts.
- [ ] `gui_conceal.c` → marker parsing (~520) — biggest; extract regex/offset layer first

## Constraints — why the remaining parsers aren't drop-ins

The easy structure parsers (table, code-fence, heading) are done. The rest each
need a deliberate design step, not a swap:

1. **Unicode classification** — Zig std has no `isspace`/`ispunct`/`tolower` over
   full Unicode (only encoding). Word count and any tokenizer that splits on
   Unicode punctuation need either a small Arabic+ASCII+space classification
   table in Zig, or to keep classification platform-side (GLib here,
   `java.lang.Character` on Android) and pass results in. Same family as the
   NFKC decision in `gui_search.c`.
2. **Array-FFI** — parsers that return a *list* (cells: `split_row`; inline
   tokens: `gui_export.c parse_inline`) need a C-ABI for returning many
   spans/structs (e.g. fill a caller array of `{offset,len,kind}`). Worth doing
   once, then reused by every list-returning parser.
3. **Iterator-scan rework** — `gui_wiki.c` and `gui_conceal.c` scan the live
   GtkTextBuffer via `gtk_text_iter_forward_search`/regex, not a string. Moving
   them means pulling the line/range text into a slice, scanning in Zig, and
   returning offset ranges the C maps back to iters + tags. A render-vs-parse
   re-split, not a drop-in.

### Keep 100% C (don't bother — pure GTK plumbing)
`gui.c` (3512), `gui_theme.c`, `gui_layout.c`, `gui_tabs.c`, `gui_statusbar.c`, `gui_hr.c`.

## Per-feature migration recipe

1. Write the pure parser in Zig (e.g. `src/markdown.zig`), returning a span/struct list. Export with
   `pub export fn … callconv(.c)` + a C-ABI struct in `gui_shared.h`.
2. Replace the C parsing code with a call to the Zig function; keep the GTK render code as-is.
3. Add Zig tests (`integration:`/unit) — the parser is now covered by `zig build test-regression`.
4. Verify the Linux render is unchanged (smoke checklist).
5. The same `zig_*` export is the Android JNI entry point — no extra work for Kotlin reuse.

API tip: return a **batch** (whole span list per pass), not per-char FFI, so the boundary stays cheap.
