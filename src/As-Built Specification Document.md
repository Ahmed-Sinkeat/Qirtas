# Qirtas — As-Built Specification Document

**Version:** 0.9.4-dev
**Branch:** full-buffer-editor-v2
**Updated:** 2026-06-12



## 1. Project Summary

Qirtas is a focused markdown notebook for Linux. It combines a Zig backend for file I/O, autosave, vault obfuscation, and cloud sync with a C GTK4/Adwaita frontend for native rendering and editing.

**Design Principles:**
- Native GTK4 rendering, no Electron
- Zig backend owns all persistent state
- C frontend is a pure presentation layer
- ChaCha20Poly1305 for vault content — **but see `docs/SECURITY.md`: the unlock key derives from world-readable `/etc/machine-id` on the same disk, so this currently defeats casual browsing only, NOT device theft. Do not market as "encrypted"/"privacy-first" until the key-handling roadmap there is done and human-reviewed.**
- On-demand cloud sync (not continuous) — conflict safety differs per backend, see `docs/SYNC.md` conflict matrix (Dropbox/Local can silently lose edits)

---

## 2. Core Technology Decisions

### 2.1 Backend: Zig

- **Seamless C FFI:** Zig exports C-linkage functions consumed by GTK4/Adwaita without wrapper overhead.
- **Unified Build:** `build.zig` compiles both Zig and C sources into a single executable.
- **File I/O Ownership:** The Zig backend owns the full document text, inotify watches, undo stack, autosave timer, and crypto vault.

### 2.2 Frontend: C GTK4 & Libadwaita

- **Native UI Performance:** Hardware-accelerated GTK4 rendering. No web-based overhead.
- **GtkSourceView:** Provides syntax highlighting, custom colour schemes, and markdown language definitions.
- **Full-Buffer Model:** The GTK `GtkTextBuffer` holds the entire document. Virtual scroll was prototyped and removed. GTK handles natural scrolling natively.

### 2.3 Local Cryptography: ChaCha20Poly1305

- Files encrypted with a random 32-byte Master Key.
- Master Key stored in `system_keys`, unlocked via `machine-id`-derived key.
- Recovery uses a 24-word BIP-39 mnemonic with optional passphrase.

**Threat-model caveat (2026-06-12):** `/etc/machine-id` is world-readable and
on the same disk as the vault — an attacker with the disk has both ciphertext
and key material. Honest scope today: protection against casual file browsing
only. Working-directory `.md` files are plaintext regardless. Full analysis,
accurate-claims wording, and the fix roadmap (Argon2id passphrase unlock —
schema columns already reserved — or libsecret keyring) live in
`docs/SECURITY.md`. Crypto path needs human security review before any
user-facing "encrypted" claim.

### 2.4 Cloud Sync: On-Demand Event-Driven

Sync fires only on: Save, App Close, or "Sync Now". Supports Google Drive, Dropbox, GitHub, and Local folder sync. Since 2026-06-12 all four backends are native Zig HTTP/file code with unified 3-way conflict detection (`_conflict` copies, never silent loss); the external bash helper scripts are gone.

**Auth, as of 2026-06-16:**
- **GitHub** — primary path is a **Personal Access Token** the user pastes (`zig_github_connect_with_token` → verify via `GET /user` → store encrypted). A `repo`-scope PAT creates the repo and pushes with no app-permission limits. The browser **device flow** (bundled GitHub *App* client ID) remains a fallback, but its user-to-server token 404s on writes to repos the App isn't installed on — hence the PAT default.
- **Google Drive / Dropbox** — OAuth loopback now uses **PKCE (S256)**: `zig_pkce_challenge()` generates a verifier (stored module-static), returns the base64url-SHA256 challenge for the auth URL; the verifier is replayed in the token exchange. Public clients require this or the exchange is rejected. App credentials still come from `QIRTAS_GOOGLE_CLIENT_ID` / `QIRTAS_DROPBOX_APP_KEY`.
- **All sync HTTP** sends `Accept-Encoding: identity` (`accept_encoding = .omit`); gzipped JSON previously surfaced as `Error: SyntaxError`.

---

## 3. Architecture: Buffer Model

### 3.1 Current State (Full Buffer, Native Scrollable)

The GTK `GtkTextBuffer` holds the **entire document**. Since 2026-06-12 the `GtkSourceView` is the **direct scrollable child** of the `GtkScrolledWindow` (the leftover virtual-layout spacer box was removed). GTK therefore validates Pango layout **lazily** — only visible lines — so big files open instantly, and GtkTextView's native scroll-to-cursor works.

```
Document (owned by Zig backend)
↓ zig_get_document_text()
GtkTextBuffer (entire file)
↓ GtkSourceView (direct GtkScrollable child) → lazy line validation
Visible viewport (native GTK scroll position)
```

**Do not re-wrap the source view in a GtkBox** — that allocates it at full document height and forces whole-document layout on open (multi-second UI freeze, choppy pointer).

Per-keystroke costs are debounced: word/char count + full-buffer conceal pass run once per 220 ms typing pause (`buffer_stats_timeout_cb`), not per keystroke. Local conceal around the cursor stays instant via `on_mark_set`.

### 3.2 Virtual Scroll (Removed)

A virtual viewport prototype was developed that loaded only a window of ~300 lines into GTK buffer. This required buffer-generation guards, spacer widgets, and complex position remapping.

**Status: Fully removed.** The code was reverted to full-buffer mode. The prototype remains tagged as `viewport-prototype` in git.

### 3.3 Buffer-Generation Guard Pattern

Deferred `g_idle_add` callbacks that walk `GtkTextIter` capture `buffer_generation` at schedule time and discard themselves if the buffer was swapped:

```c
// Snapshot on schedule:
d->generation = gui->buffer_generation;

// Check on execute:
if (d->generation != d->gui->buffer_generation) {
    g_free(d);
    return G_SOURCE_REMOVE;
}
```

This guard remains active even in full-buffer mode as a safety net.

---

## 4. Cursor Movement Architecture

### 4.1 Mark-Set Chain

Every cursor movement fires `on_mark_set` for both `insert` and `selection_bound` marks. Each `on_mark_set` call schedules:

- `idle_local_conceal_cb` — re-conceals the current line only
- `idle_scroll_to_cursor` — scrolls the view to keep cursor visible. Runs at `G_PRIORITY_HIGH_IDLE + 15` (after GTK layout at +10, before paint at +20) so the viewport moves in the same frame the caret is drawn. It calls `gtk_text_view_scroll_to_mark` on the live insert mark (offset −1 sentinel) — **never scroll synchronously inside `mark-set`**: the signal fires while the text btree is mid-mutation and synchronous layout validation there aborts.

### 4.2 Conceal Callback Model

| Callback | Trigger | Scope |
|---|---|---|
| `idle_local_conceal_cb` | Every cursor move | Current line only |
| `idle_global_conceal_cb` | File open / large buffer change | Entire buffer |
| `idle_wiki_local_cb` | Cursor move | Current paragraph |
| `idle_wiki_global_cb` | File open | Entire buffer |

### 4.3 Profiling Results (historical, 2026-06-11)

Measured on the `measure-cursor-v3` branch using a temporary SIGUSR1-based metrics harness. **That harness, all `g_print` debug instrumentation, and the SIGUSR1 handler have since been removed from `gui.c` and the `gui/*.c` modules** — the numbers below are kept as a historical baseline only and cannot be reproduced without re-adding the harness.

Test: hold Down Arrow ~100 presses over 5 seconds.

**1000-line file:**

| Callback | Calls | Total ms | Avg ms | Max ms |
|---|---|---|---|---|
| `on_mark_set` | 31 | 0.043 | 0.001 | 0.011 |
| `idle_local_conceal_cb` | 1 | 0.197 | 0.197 | 0.197 |
| `idle_global_conceal_cb` | 0 | 0.000 | — | — |
| `idle_wiki_local_cb` | 0 | 0.000 | — | — |
| `idle_wiki_global_cb` | 1 | 0.041 | 0.041 | 0.041 |

**5000-line file:**

| Callback | Calls | Total ms | Avg ms | Max ms |
|---|---|---|---|---|
| `on_mark_set` | 37 | 0.084 | 0.002 | 0.015 |
| `idle_local_conceal_cb` | 3 | 0.759 | 0.253 | 0.323 |
| `idle_global_conceal_cb` | 0 | 0.000 | — | — |
| `idle_wiki_local_cb` | 0 | 0.000 | — | — |
| `idle_wiki_global_cb` | 1 | 0.057 | 0.057 | 0.057 |

**Interpretation:**
- `on_mark_set` itself is negligible (< 0.015 ms max).
- `idle_local_conceal_cb` is the heaviest per-keypress cost. Max 0.323 ms at 5000 lines.
- Global passes (`idle_global_conceal_cb`, `idle_wiki_global_cb`) do not run on cursor movement — only on file open.
- Total cursor-movement overhead per keypress: **< 0.35 ms** at 5000 lines. Not a bottleneck.
- Low idle_local call count (1–3) for ~100 key presses indicates the virtual-scroll viewport residual code was limiting cursor travel. Full-buffer mode should see higher counts.

### 4.4 Cursor Position Char/Byte Unit Fix (2026-06-11)

`gui_get_cursor_position()` / `gui_set_cursor_position()` previously mixed char and byte units in places. They now consistently use **character offsets** end to end:

- `gui_get_cursor_position()` reads `gtk_text_iter_get_line_offset()` (char-based), matching `iter_to_position()` used everywhere else.
- `gui_set_cursor_position()` clamps the incoming `col` to `gtk_text_iter_get_chars_in_line()` (char count) before calling `gtk_text_buffer_get_iter_at_line_offset()` (char-based), instead of clamping to byte length and/or mixing in a byte-based iter call.

### 4.5 Conceal fragility + escape hatch (2026-06-15)

The markdown conceal hides syntax markers (`**`, `#`, `[`…`](…)`, …) by applying
a `conceal` text tag (`scale 0.01` + transparent foreground) over the marker
ranges, plus `heading1..4` size tags. This mechanism is **fragile in GTK's text
layout on some pathological documents** (e.g. the markdown-test-file `TEST.md`:
inline HTML, a table, many links). The buffer loads fine but, on cursor
move/scroll, GTK's own layout idle aborts with
`gtk_text_buffer_apply_tag: gtk_text_iter_get_buffer(end) == buffer` /
`Byte index N off the end of the line`. The backtrace is stripped of Qirtas
frames — the failing `apply_tag` is dispatched from a GTK-internal idle, not our
`g_idle` callbacks — confirming the live conceal code (`gui_conceal.c`,
`apply_regex_conceal_local`, clamped/fresh iters) is not the direct caller; GTK
chokes on the scale-tagged ranges. Switching the tag to `invisible` instead of
`scale 0.01` trades this for a different navigation-time `Byte index off the
end` abort, so neither tag form is safe on this content.

NOTE: `gui.c` still carries **dead duplicate** `apply_regex_conceal` /
`apply_regex_conceal_local` (declared + defined, never called) — clutter, not
the live path.

**Escape hatch (shipped):** conceal is now a persistent setting
(`conceal_enabled` pref, default on) exposed as the "Conceal Markdown Markers"
checkbox in editor settings; `QIRTAS_NO_CONCEAL=1` still overrides for
debugging. Turning it off strips the conceal/heading tags (raw markers render,
crash-safe). A real fix needs a conceal mechanism that hides markers without
disrupting GTK's char↔layout mapping; until then, a heuristic skips concealing
markers on layout-hostile lines (raw HTML `<…>`, tables `|`, or very long
lines).

### 4.6 Editor/UX fixes batch (2026-06-14/15)

- Zoom shortcuts (Ctrl +/=/-/0) wired in `on_editor_key_pressed` (the
  window-level handler was pre-empted); `quick_switch` (Ctrl+P) wired.
- Read mode keeps full-page width when set; line-wrap toggle forces an immediate
  paper-column recompute (no more dead space on the right).
- Undo / full reload restores the viewport by **line** (reflow-stable) instead
  of pixel offset — fixes the Ctrl+Z page jump.
- Vault switch clears all tabs first (`gui_tabs_close_all`) — old-vault tabs no
  longer resolve into the new vault as empty files (data loss).
- Keyboard-shortcuts window is non-modal (no longer blocks editing / settings
  close) and translatable.
- Pointer-color setting removed. Added **Column Width** (centered text column,
  `centered_text_width` pref) and **Card Gap** (`desk_gap`) sliders.
- Library bar stays open on editor/workspace clicks — only its toggle icon
  closes it (removed the hide from `on_editor_left_click` + `on_workspace_click`).
- HR rendering (`parse_and_render_hrs`, `check_and_insert_hr`) reworked to
  iterate by line number / reacquire iters by offset — the old single-iter loop
  reused iterators across `delete`+`insert_child_anchor` and corrupted the
  document (cut-all-then-paste truncation) + spewed the iterator storm. CRLF/CR
  is normalized to LF on load (`loadDocFromSlice`) — stray `\r` desynced the
  conceal offset math on CRLF Markdown.

This matters most for Arabic/RTL and any multi-byte UTF-8 content, where `chars_in_line != bytes_in_line`. A previous mismatch here could pass an out-of-range byte index into a char-based iter call (or vice versa). This was a real correctness bug, but **not** the cause of the `"Byte index N is off the end of the line"` crash — see §4.5 for the actual root cause and fix.

### 4.5 Root Cause and Fix: `Gtk-ERROR: Byte index N is off the end of the line` (2026-06-11)

**Confirmed via gdb backtrace** (reproduced by opening a doc with box-drawing/arrow characters — e.g. this file's repository-layout block — and moving the mouse over the editor):

```
#5  gtk_text_iter_set_visible_line_index () from libgtk-4.so.1
#6  ?? () from libgtk-4.so.1
#7  ?? () from libgtk-4.so.1
#8  editor_get_iter_at_widget_point (...) at src/gui/gui_wiki.c:13
#9  on_editor_motion (...) at src/gui/gui_wiki.c:219
```

**Root cause:** `editor_get_iter_at_widget_point()` (three copies existed: `gui.c`, `gui_wiki.c`, `gui_popover.c`) called `gtk_text_view_get_iter_at_position()` on every mouse-motion event over the editor (used for wiki-link hover detection and formatting-popover placement). That GTK4 function internally calls `gtk_text_iter_set_visible_line_index()`, which has a bug: on a line that has both an `invisible`-tagged range (our markdown `conceal` tag, used to hide `**`, `==`, `#`, `[[`/`]]`, etc.) **and** multi-byte UTF-8 characters, the "visible byte index" GTK computes from the Pango layout overshoots the real line's byte length. GTK hits its internal assertion in `gtktextbtree.c:4012` and calls `g_error()`, which is fatal — `Gtk-ERROR **: Byte index N is off the end of the line` aborts the whole process (SIGABRT).

This matches the user's hypothesis: it **is** a real iterator out-of-bounds bug from a GTK iterator API being handed a value larger than the line's byte length — just one level removed (inside `gtk_text_view_get_iter_at_position`'s internal `set_visible_line_index` call, not a direct call we wrote to `get_iter_at_line_offset`/`set_line_offset`).

**Fix:** rewrote `editor_get_iter_at_widget_point()` in `gui_wiki.c` and `gui_popover.c` (the two live copies; the dead third copy in `gui.c` was deleted) to never call `gtk_text_view_get_iter_at_position()`. Instead:

1. `gtk_text_view_get_line_at_y()` finds the line under the pointer — always returns byte 0 of a line, can't overshoot.
2. Walk forward character-by-character with `gtk_text_iter_forward_chars()`, calling `gtk_text_view_get_iter_location()` (iter → pixel, the safe direction — never triggers `set_visible_line_index`) until the target x-coordinate falls within a character's rectangle.

**Verified:** rebuilt, ran under gdb with a scripted mouse sweep over a doc containing the repository-layout box-drawing block (the exact reproduction case) — no crash, no `Gtk-ERROR`, process stayed alive.

### 4.6 Final Fix: Conceal No Longer Uses `invisible` (2026-06-12)

The §4.5 fix patched our own call sites, but the same buggy GTK path (`gtk_text_iter_set_visible_line_index`) is also reachable from **GTK-internal** pixel→iter conversions (mouse-click cursor placement, vertical cursor motion) that application code cannot intercept. The crash recurred (`Byte index 49/89 is off the end of the line`).

**Definitive fix:** the markdown `conceal` tag no longer sets `invisible`. It hides markers with `scale = 0.01` + fully transparent `foreground` (`rgba(0,0,0,0)`), with tag priority forced above heading/syntax tags. With zero invisible text in the buffer, GTK never enters its visible-line-index bookkeeping, so the whole bug class is unreachable. Rule for future work: **never apply an `invisible` text tag in this codebase.**

---

## 4a-bis. Typing Feel & Arabic Depth (2026-06-12)

- **Word-grain undo** — the per-keystroke commit at the end of
  `on_editor_key_pressed` now fires only on boundaries (space, Enter, Tab,
  punctuation, deletions); `zig_undo()` seals pending edits before undoing so
  nothing is lost. Ctrl+Z removes words, not characters.
- **Tab / Shift+Tab** indents/outdents list items (bullets, checkboxes,
  numbered) by two spaces; non-list lines keep default Tab behavior.
- **Selection wrapping** — typing `*` or `_` with a selection wraps it
  (`( [ { " \`` already wrapped via `insert_text_pair`).
- **Paragraph direction by first strong char** — `detect_rtl` in
  `gui_conceal.c` skips leading markdown syntax (`#`, `-`, digits, checkbox
  brackets) and decides on the first strong character, instead of "any Arabic
  char anywhere → RTL". `# عنوان` renders RTL; "see ملاحظة for details"
  stays LTR.
- **Arabic-normalized workspace search** — `normalizeArabicAlloc` in
  `main.zig` (alef forms→ا, ة→ه, ى→ي, strips tashkeel/tatweel); the FTS
  table gained a `content_norm` shadow column (auto-migrated from v1 schema,
  forces one reindex) and queries are normalized, so مدرسه finds المدرسة.
  Single implementation, exported to the C indexer as `zig_normalize_arabic`.
- **Eastern Arabic numerals** in the word/char counts when the UI language
  is Arabic.
- **Stats perf** — char count now uses the free
  `gtk_text_buffer_get_char_count()`; the O(n) word count is skipped above
  500k chars instead of copying the whole buffer every typing pause.
- **Shortcuts under the Arabic keyboard layout: verified safe** —
  `match_app_shortcut` already falls back to hardware-keycode matching
  through the Latin (group 0) layout for every shortcut.

## 4a-ter. Navigation, Observability, Distribution (2026-06-12, round 2)

- **Quick Switcher (Ctrl+P)** — fuzzy filename popup (`gui_switcher.c`):
  recursive scan (depth 3, .md/.txt, capped 2000), subsequence scorer with
  consecutive/word-start bonuses, both sides run through
  `zig_normalize_arabic` so ملاحظه finds الملاحظة.md. Enter/↑↓/click;
  Export PDF moved to Ctrl+Shift+P.
- **Outline panel** — heading TOC (`gui_outline.c`), reuses the conceal-style
  line scan, refreshed by the existing 220 ms debounce and on buffer reload;
  hidden when the note has no headings. (Moved from the sidebar to a desk-side
  GtkRevealer in the 2026-06-13 redesign — see §4c.)
- **Session restore, all tabs** — shutdown stores every open tab
  (`session_tabs` pref, newline-joined); startup reopens each, active file
  last.
- **`QIRTAS_PERF=1`** — permanent lightweight observability: wrapped
  main-loop callbacks (stats pass, global/local conceal) log to stderr when
  they exceed 8 ms (`QIRTAS_PERF_BEGIN/END` macros in `gui_internal.h`).
  Optimization policy: fix what this log catches; the deferred items (delta
  undo, viewport-limited conceal) only matter if it ever fires.
- **`QIRTAS_BENCH=<file>`** — offline edit-path microbenchmark. Loads the file
  into the real `doc_buf`, drives `zig_insert_text` N times mid-document, and
  reports min/avg/max ms for `zig_insert_text` and `populate_line_offsets`,
  then exits before the GUI. Isolates per-keystroke CPU (mine) from GTK layout
  (the widget's). Measured 2026-06-14: `populate_line_offsets` is ~100% of the
  per-keystroke cost (the in-place splice memmove is free), strictly
  O(document) — 0.52 ms @ 29 k bytes / 3 k lines → 10.2 ms @ 581 k / 60 k lines
  (linear). Scrolling triggers none of this path (edit code only runs on buffer
  change), so scroll CPU is pure GTK relayout. **Fixed 2026-06-14:**
  `zig_insert_text`/`zig_delete_range`/`zig_replace_range` now patch
  `line_offsets` incrementally (`lineOffsetsInsert`/`lineOffsetsDelete` — shift
  entries after the edit, add/remove only the touched newline entries) instead
  of a full rescan; `populate_line_offsets` is now load-time + fallback only.
  Per-keystroke cost dropped 10.5 ms → 0.25 ms @ 581 k (~42×), bench-verified
  byte-identical to a full rescan across 6 k insert/delete/replace/newline
  edits. Scroll CPU still pure GTK relayout — only viewport virtualization
  touches that.
- **CI** — `.github/workflows/ci.yml` builds + tests on every push
  (ubuntu-24.04, GTK stack via apt, Zig 0.16.0). First run may need version
  pin adjustments.
- **Flatpak** — starting-point manifest at `packaging/org.qirtas.Qirtas.yml`,
  explicitly marked untested (Zig sha256 placeholder, file-access scoping
  to tighten).
- **UI layout map** — `docs/LAYOUT.md` describes every screen region, the
  widget that owns it, and where it's built.

## 4a-quater. Themed PDF Export (2026-06-12)

`gui_export.c` owns `qirtas_export_to_pdf`: a theme chooser (last choice in
`app_prefs.export_theme`) → cairo_pdf_surface renderer. Direct surface
access (not GtkPrintOperation) provides PDF outline bookmarks from headings
(`cairo_pdf_surface_add_outline`), title/creator metadata, and exact page
control. The legacy GtkSourcePrintCompositor path (`gui_pdf.c`, "Editor
look" card) is removed — it printed raw markdown source with the live
syntax-highlight scheme (invisible text on white pages under dark themes)
and a page-numbered footer. Replaced by the **Editor — Plain** theme below,
which renders through the same block pipeline as the other themes (real
bold/headings/lists, theme-independent ink) with no frame, title header, or
folio.

Architecture: `PrintTheme` struct (page geometry with inner/outer margins
mirrored per page parity, fonts, scales, inks, feature flags) — a new theme
is one static initializer. Document → blocks (heading/para/quote/list/code/
HR, same line vocabulary as conceal), each block → a PangoLayout
(auto-dir per paragraph, justify, line-spacing multiplier), rendered
line-by-line with page breaks: frame and furniture redraw per page,
≥2-line widow/orphan minimum, headings keep-with-next. Inline `**`/`*`/
`` ` ``/`~~`/`==`/links → PangoAttrList.

Themes: **متن** (classical matn GENRE — Amiri ~14.5pt × 1.9 leading,
justified, RTL gutter, double-rule frame, rubrication: bold/headings/
blockquote-as-matn in deep red, Eastern folios ‹ ٥ ›; deliberately our own
take, no publisher's template), **Paper & Ink**, **Academic** (numbered
headings), **Typewriter**, **Editor — Plain** (Inter 11pt, no frame/title/
folio — `hide_page_number` flag on `PrintTheme` suppresses the footer
entirely). Manual test checklist: tashkeel-heavy doc, paragraph splitting
across a framed page, justified RTL page. No kashida — plain justify, like
modern matn editions.

Settings gained **Apply & Restart** next to Language: relaunches the
binary (g_spawn self + quit; session restore brings tabs back) because
labels are baked at widget construction. Docs/tooltips now state this.

**Editor-clobber hazard (process note):** two committed batches of gui.c
edits were silently reverted by an external editor holding a stale buffer
of the file. When editing this repo with agents, close `gui.c` in other
editors; verify-after-build before committing.

**Next (planned, not built):** spell check via libspelling + hunspell Arabic
dictionaries (system dependency decision), Flatpak manifest verification,
typewriter mode (caret vertically centered),
focus-paragraph dimming (low-opacity tag outside current paragraph),
libsecret keyring for sync tokens, smarter undo sealing on idle pause,
conceal-vs-diacritics stress test note, kashida-free justified Arabic PDF
export check.

## 4a-quinquies. Save Pipeline & Crash Recovery (2026-06-12)

Three layers between a keystroke and durable storage:

1. **Debounced save-on-pause.** `autosave_debounce_cb` in `gui.c` fires
   `gui_trigger_autosave()` 2.5 s after typing stops. The 30 s autosave
   thread stays as the backstop. Worst-case loss on `kill -9`: ~2.5 s of
   typing. Verified by smoke checklist (`docs/SMOKE-CHECKLIST.md`).
2. **Atomic writes.** All save paths go through `atomicWriteFile`
   (tmp → fsync → rename), see §9.
3. **Snapshot history.** After each successful autosave,
   `gui_history_record` (`src/gui/gui_history.c`) stores a full document
   snapshot in the vault DB `file_history` table — at most one per
   300 s per file, skipping unchanged content. Tiered pruning: keep all
   from last 24 h, one/hour for 7 days, one/day for 30 days, drop older.
   **Caveat:** snapshots are plaintext even in encrypted vaults; must be
   encrypted with the master key before a restore UI ships. No restore
   UI exists yet — recovery is manual SQL.

Related hardening, same date:

- **Sync status enum.** `gui_set_sync_status(const char *)` (strcmp on
  English tokens) replaced by `gui_set_sync_state(QirtasSyncState)`
  (`QIRTAS_SYNC_SYNCED` / `SAVING` / `NOT_SYNCED`, defined in
  `gui_shared.h`). String classification would silently misclassify once
  text passed through `qirtas_tr`. Translation now happens only at labels.
- **Active file path bounds check.** All writes to the 1024-byte
  `active_file_path` buffer in `main.zig` go through `setActiveFilePath`,
  which refuses over-long paths (returns false, keeps current file) rather
  than truncating to a wrong path. Arabic paths are ~2 bytes/char in UTF-8,
  so byte limits arrive at half the visible length. Covered by Zig test.

## 4a. Undo Architecture — single system, GTK's disabled

Undo/redo is owned **entirely by the Zig backend**: full-document heap
snapshots (`captureUndoEntry`) on commit boundaries, 100 entries max and
64 MB total per stack, oldest evicted first.

GTK's built-in delta-based undo is **explicitly disabled** right after the
buffer is created (`gtk_text_buffer_set_enable_undo(buf, FALSE)` in
`gui.c`) — do not re-enable it. Two parallel histories would double memory
cost and make Ctrl+Z behavior depend on which system catches the keystroke
first. `on_editor_key_pressed` intercepts Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y and
routes them to `zig_undo()` / `zig_redo()`, returning TRUE so GTK's (no-op
but still bound) `text.undo` action never fires.

If a new editable GtkTextView is ever added (dialogs etc.), GTK4 enables its
undo by default — that's fine for throwaway entry widgets, but any view
showing *document* content must have it disabled and route through the Zig
stack.

---

## 4b. Preferences, Localization, Icons (2026-06-12)

- **`app_prefs` store** — generic key/value table in the vault DB (`qirtas_pref_*` helpers, `gui_cursor.c`). Holds: `wrap_lines`, `show_line_numbers`, `highlight_current_line`, `show_right_margin`, `right_margin_pos`, `show_overview_map`, `restore_session`, `compact_mode`, `app_language`, `icon_style`, `last_file`, `outline_panel_visible`.
- **Settings** moved out of the sidebar into the status-bar menu (☰ → Preferences, Ctrl+,). New entries: line numbers, highlight current line, overview map (GtkSourceMap overlay on the editor card), right margin + position, restore session, compact layout, language, icon style.
- **Status-bar menu** also carries: Copy File (puts the active `.md` on the clipboard as a `text/uri-list` file), Save As, Find/Replace, Fullscreen, Keyboard Shortcuts, Quit (for environments without window decorations).
- **Find & Replace** — second row in the search bar; uses `gtk_source_search_context_replace[_all]`.
- **Localization** — `qirtas_tr()` English→Arabic table in `gui.c`; Arabic mode flips app RTL (`gtk_widget_set_default_direction`); the tab strip, card header, and status pill are pinned LTR. Labels apply on next launch; direction flips live.
- **Icon styles** — `qirtas_icon()` logical-key lookup, Classic/Modern symbolic sets; main bar + explorer icons swap live, popovers on next launch.
- **Caret color fix** — custom pointer color is emitted from the font provider (`APPLICATION+1`); emitting it only from the theme provider was silently overridden.

---

## 4c. UI Redesign — Floating Paper, Top Tabs, Navy Theme (2026-06-13)

Reworked the window chrome to match the `design_handoff_ui_redesign` mockups while
keeping the floating-paper identity, RTL-first layout, and pinned-LTR status. All
structure is built in `activate()` (`gui.c`) and styled in `base.css`.

- **Top tab strip.** Tabs left the bottom bar for a pinned 42px strip at the top of
  `main_vertical_box` (`gui->tab_strip`, LTR). Flat tabs, 2px accent underline on the
  active tab, accent unsaved dot. `reorder_main_layout` keeps the strip pinned top.
- **Paper card wrapper.** `.editor-card` (`gui->editor_card`) owns the border / radius /
  shadow / desk margins and stacks: a 2px gradient **thread** (`gui->editor_thread`,
  gold `#c9a86b` on dark via `--thread-color`, navy on the navy theme) → a 46px
  **header band** (`gui->editor_header`: path breadcrumb + reparented 🔍 search /
  ≡ outline toggle / ⋮ menu) → the borderless scrolling view.
- **Breadcrumb** mirrors the active file's vault-relative path (`folder / sub / file`),
  updated in `gui_set_title`.
- **Centred text column**, derived each tick from the card width via
  `paper_column_tick` (`gui->text_column_width`, `QIRTAS_TEXT_COLUMN_MIN`=420 /
  `QIRTAS_TEXT_COLUMN_MAX`=840, see §4d). No native max-width on
  `GtkSourceView`; `paper_column_tick` recomputes symmetric text margins from
  the card width (GTK4 dropped `size-allocate`). For CPU reasons this
  runs as a ~120ms `g_timeout_add` in steady state (no-op unless width/gutter
  changed) and only escalates to a 60fps `gtk_widget_add_tick_callback` while the
  user is actively dragging a column edge (cursor hints via
  `on_column_resize_motion`, drag handled by `on_column_resize_begin/_update/_end`
  on `editor_overlay`). In focus mode the card is centred on the desk with a capped
  width so the paper floats in the middle when both panes are hidden / fullscreen.
- **Desk outline panel.** The heading TOC moved out of the sidebar onto the desk as a
  `GtkRevealer` (`gui->outline_panel`), left of the paper under RTL. Header ≡ toggles
  it, `×` closes it; state persists in the new `outline_panel_visible` pref (default 1).
  `gui_outline.c` still owns the content.
- **Sidebar brand header.** `.sidebar-header` row: `qirtas-logo.png` feather+wordmark
  logo + translatable "Library" label.
- **Bottom status pill.** The two-row status bar is gone; what remains is a small
  centred floating pill (`.status-pill`) with only the sync dot + word + char counts,
  defaulting to the bottom (`Status Bar Position` still works). Sidebar toggle is
  Ctrl+\ only.
- **Navy theme.** New `theme-qirtas-navy.css` + `qirtas-navy.style-scheme.xml`
  ("Paper & Ink Navy"): pure-white paper, warm parchment desk, `#213A63` navy accent.
  Wired through `apply_theme`, `theme_name_to_index`, the Settings dropdown, and the
  cursor-trail color map. Six themes total.
- **Line numbers** default off (`show_line_numbers` pref default 0).

### Redesign round 2 (same day)

- **Floating status pill.** The status row no longer lives in a bottom bar; it
  is a `GtkOverlay` child on the paper card, pinned to the bottom-**end** corner
  (bottom-left under Arabic/RTL, bottom-right under English/LTR via `GTK_ALIGN_END`).
  Only the sync dot + word + char counts remain. `bottom_bar` is kept as an empty
  0-height tray so the layout/focus reorder code still has a valid widget.
- **Dark theme = gold.** `theme-qirtas-dark.css` and the `qirtas-night` editor
  scheme were retuned to the handoff dark direction: neutral near-black surfaces
  (`#101218`→`#14161C`→`#1D1F27`) with a single quill-gold accent `#C9A86B`
  (thread, headings, bold, links, active tab, unsaved dot).
- **Card-header toggles.** The book/sidebar icon toggles the workspace+files
  sidebar (`on_logo_clicked`); a separate list icon toggles the desk outline panel.
- **Line numbers hug the text.** When the gutter is shown, `paper_column_tick`
  slides it rightward (`gtk_widget_set_margin_start` on the source-view gutter) so
  the digits sit just left of the centred column instead of at the card's edge.
- **Smart Arabic counts.** `arabic_count_phrase` in `gui_buffer.c` applies تمييز
  العدد: 1 كلمة واحدة، 2 كلمتان، 3-10 spelled + plural (ست كلمات)، 11+ Eastern
  digits + singular (٢٠ كلمة). حرف uses the masculine number forms.
- **Settings regrouped** into Appearance / Editor / Sync / General with an explicit
  high-contrast header bar (the default CSD title/close washed out on light themes).
  Removed: Status Bar Position, Show Layout Dividers, Scroll Past End, Overview Map,
  Show Right Margin + Margin Position.

## 4d. Text Width Setting, Heading Rubrication, Tooltip Fix (2026-06-13)

- **Text Width setting.** New Editor-section dropdown "Text Width": **Centered
  (Fixed Width)** (default) vs **Full Page Width**. Backed by
  `gui->text_width_full_page` / pref `text_width_full_page`
  (`on_text_width_mode_changed`, gui.c). `paper_column_tick`'s clamp:
  `text_w = clamp(card_width - QIRTAS_CARD_CHROME, QIRTAS_TEXT_COLUMN_MIN,
  QIRTAS_TEXT_COLUMN_MAX)`, but the upper clamp is skipped entirely in
  "Full Page Width" mode so the column always fills the card. Toggling sets
  `s_last_paper_width = -1` to force an immediate recompute.
- **`QIRTAS_TEXT_COLUMN_MAX` corrected 1400px → 840px.** The old constant let
  "Centered" effectively behave like full-page on any normal-width monitor
  (the clamp never engaged). 840px is the actual fixed reading-column measure
  the centred mode is meant to enforce.
- **Heading/markdown rubrication.** `qirtas.style-scheme.xml` (light, theme
  `qirtas`) and `qirtas-night.style-scheme.xml` (dark, theme `qirtas-dark`)
  now give each `qirtas_markdown:h1`-`h6` level (plus bold/italic/blockquote/
  inline-code/list-marker) its own complementary color instead of one
  monochrome "ink"/"gold":
  - Light: h1 rubric red `#8a3324`, h2 pine `#2f5d46`, h3 plum `#5b3a6e`,
    h4 amber `#8a5a1e`, h5/h6 fade to ink-muted/ink-faint. Blockquote =
    slate-blue `#3a4f7a` text on tinted `#eef1f8` background. Inline-code →
    pine, list markers → amber.
  - Dark: h1 gold-bright `#d9b577`, h2 sapphire `#9bc4e6`, h3 coral `#d98a73`,
    h4 sage `#9bc49a`, h5/h6 fade to ink-muted/ink-faint. Inline-code → sage,
    list markers → gold. Blockquote keeps its existing slate-blue-on-navy tint.
  `markdown:*` compatibility aliases updated to match.
- **Tooltip text fix.** GTK tooltips always render with a dark pill regardless
  of app theme. The global `label { color: var(--text-primary) }` rule in
  `base.css` was making tooltip text dark-on-dark (invisible) on light themes.
  Added `tooltip label { color: #ffffff; }` override.

---

## 5. Repository Layout

```
Qirtas/
├── build.zig
├── build.zig.zon
├── src/
│   ├── main.zig                         ← Zig app root, file I/O, undo, autosave, FFI exports
│   ├── bip39.zig                        ← BIP-39 recovery phrase helpers
│   ├── sync.zig                         ← Cloud sync (Google Drive, Dropbox, GitHub, local)
│   ├── root.zig                         ← Zig module root
│   ├── gui.c                            ← GTK layout, window setup, scroll, key handling
│   ├── gui_internal.h                   ← UI-only shared state, AppGui struct, module hooks
│   ├── gui_shared.h                     ← Zig-facing FFI declarations
│   ├── STRUCTURE.md                     ← Source layout and edit guide
│   ├── As-Built Specification Document.md  ← This file
│   └── gui/
│       ├── gui_theme.c                  ← CSS loading, theme switching, font selection
│       ├── gui_cursor.c                 ← Cursor trail animations
│       ├── gui_editor.c                 ← Editing, buffer events, gestures, paragraph alignment, paste handling
│       ├── gui_popover.c                ← Markdown formatting popup, undo sealing, paragraph alignment helper
│       ├── gui_conceal.c                ← Markdown concealment passes, heading tags, idle guard
│       ├── gui_wiki.c                   ← Wiki-link parsing and navigation, idle guard
│       ├── gui_hr.c                     ← Horizontal rule renderer
│       ├── gui_search.c                 ← Inline search bar overlay
│       ├── gui_explorer.c               ← Directory tree and active files drawer
│       ├── gui_tabs.c                   ← Document tab controls (top tab strip)
│       ├── gui_export.c                 ← Themed PDF export (متن + 4 more, incl. Editor — Plain), theme chooser
│       ├── gui_history.c                ← Crash-recovery snapshots (file_history table, pruning)
│       ├── gui_switcher.c               ← Quick Switcher (Ctrl+P)
│       ├── gui_outline.c                ← Outline panel (heading TOC)
│       ├── gui_shortcuts.c              ← Keyboard shortcuts table, keybindings window
│       └── gui_sync.c                   ← Cloud credentials and sync event UI
│   └── ui/
│       ├── themes/
│       │   ├── base.css                 ← Shared layout, spacing, widget styles (tab strip, paper card, desk outline, status pill)
│       │   ├── theme-qirtas-light.css   ← Paper & Ink light
│       │   ├── theme-qirtas-dark.css    ← Paper & Ink dark (gold thread)
│       │   ├── theme-qirtas-navy.css    ← Paper & Ink Navy (redesign light, #213A63)
│       │   ├── theme-sepia.css          ← Classic / Deep Sepia
│       │   ├── theme-typewriter-dark.css
│       │   └── theme-typewriter-light.css
│       ├── icons/
│       ├── qirtas_markdown.lang         ← GtkSourceView language definition
│       └── qirtas*.style-scheme.xml     ← Editor colour schemes
├── docs/
│   ├── SYNC.md                          ← Sync setup, troubleshooting, conflict matrix
│   ├── SECURITY.md                      ← Crypto threat model + roadmap
│   ├── LAYOUT.md                        ← UI layout map
│   └── SMOKE-CHECKLIST.md               ← Manual pre-push checklist (editor core)
├── README.md                            ← Repo front door
├── scratch/                             ← Developer profiling and test scripts
│   └── profile_cursor_movement.py      ← Cursor movement profiling harness
├── assets/
│   └── style.css
└── .agents/
```

---

## 6. FFI Bridge

C and Zig communicate via C-linkage exports. The Zig backend is the source of truth for document state.

### 6.1 C Functions Called from Zig

| Function | Purpose |
|---|---|
| `gui_set_text(text, len)` | Sets GtkTextBuffer from Zig-owned content |
| `gui_set_title(title)` | Updates window title and active tab |
| `gui_set_sync_state(state)` | Updates status dot (enum) |
| `gui_show_editor()` | Switches to editor view |
| `gui_show_recovery_dialog()` | Opens vault recovery modal |
| `gui_get_cursor_position(line, col)` | Gets current cursor position |
| `gui_set_cursor_position(line, col)` | Restores cursor (clamped) |
| `gui_refresh_explorer()` | Refreshes directory tree |
| `gui_trigger_autosave()` | Invokes autosave flush |
| `gui_update_sync_status(ok, text)` | Updates Google Drive status |
| `gui_update_dropbox_status(ok, text)` | Updates Dropbox status |
| `gui_update_github_status(ok, text)` | Updates GitHub status |
| `gui_update_local_sync_status(ok, text)` | Updates local sync status |
| `gui_tabs_close(gui, index)` | Closes a document tab |
| `gui_tabs_add_or_select(gui, path)` | Opens or focuses tab |
| `gui_run_on_main_thread(cb, data)` | Runs callback on GTK main thread |

### 6.2 Zig Functions Called from C

| Function | Purpose |
|---|---|
| `zig_on_gui_ready()` | Signals UI setup completion |
| `zig_has_active_master_key()` | Checks vault unlock state |
| `zig_open_file(filename)` | Opens file, updates watches |
| `zig_open_vault(dir_path)` | Toggles project directory |
| `zig_search_workspace(query)` | Searches local files |
| `zig_get_search_snippet(path)` | Gets search result preview |
| `zig_get_search_rank(path)` | Ranks search results |
| `zig_set_cursor_trail(enabled)` | Saves cursor animation config |
| `zig_get_cursor_trail()` | Gets cursor animation config |
| `zig_open_wiki_link(note_name)` | Resolves/creates wiki link file |
| `zig_create_new_file(filename)` | Creates new notebook doc |
| `zig_on_shutdown()` | Saves state on window close |
| `zig_force_save()` | Immediate disk flush |
| `zig_set_editor_border(enabled)` | Configures layout margins |
| `zig_get_editor_border()` | Gets margin config |
| `zig_dropbox_check_status()` | Checks Dropbox connection |
| `zig_github_check_status()` | Checks GitHub connection |
| `zig_db_path()` | Resolved XDG vault-DB path (single source of truth; `DB_PATH` macro in `gui_internal.h` expands to this) |
| `zig_sync_now()` / `zig_local_sync_now()` / `zig_dropbox_now()` / `zig_github_now()` | Fire on-demand sync per provider |
| `zig_sync_connect()` / `zig_sync_submit_code(code)` / `zig_sync_disconnect()` | Google Drive OAuth lifecycle (Dropbox/GitHub have equivalents) |
| `zig_save_sync_credentials(id, secret)` (+ dropbox/github variants) | Persist provider credentials (secrets encrypted) |
| `zig_get_github_credentials_decrypted(...)` / `zig_get_dropbox_credentials_decrypted(...)` | Decrypted credential readback for the C flows |
| `zig_github_connect_with_token(token, repo)` | Save a pasted GitHub PAT, then verify it via `GET /user` (background); updates the GitHub status label with the result |
| `zig_pkce_challenge(out, out_max)` | Generate a fresh PKCE verifier (stored module-static for the pending exchange) and write the base64url-SHA256 S256 challenge into `out`; used by the Drive/Dropbox auth URLs |

---

## 7. Theme System

Themes use two CSS layers:

1. `src/ui/themes/theme-<name>.css` — Color tokens per theme (current set: `qirtas-light`, `qirtas-dark`, `qirtas-navy`, `sepia`, `typewriter-light`, `typewriter-dark`)
2. `src/ui/themes/base.css` — Shared layout, spacing, and widget styles (incl. the
   `tooltip label` override — GTK tooltips are always dark, so the global
   `label { color: var(--text-primary) }` rule must not apply to them, see §4d)

Default typography: **Inter** (premium writing experience). Tabs live in the top
tab strip (`gui->tab_strip`, see §4c), not a status bar.

### Adding a Theme

1. Copy `src/ui/themes/theme-sepia.css`.
2. Update color tokens.
3. Add branch in `apply_theme()` in `src/gui.c`.
4. Add to settings dropdown.
5. Optionally add matching GtkSourceView style scheme.

---

## 8. Testing

`zig build test` builds and runs the Zig test suite (`build.zig` wires the
GTK/sqlite linkage for it). Coverage as of 2026-06-12:

- `main.zig` — system_keys schema shape test, file encryption round-trip, atomic-write round-trip (content + no leftover tmp file), active-file-path bounds (over-long path refused, previous path kept)
- `sync.zig` — token crypto round-trip (encrypt→decrypt identity), tamper
  rejection (modified ciphertext must fail authentication), ISO-8601
  timestamp parsing, conflict filename generation, syncable-file filter,
  XDG path resolution shape, percent-encoding, header-JSON escaping (incl. surrogate pairs)

Known gaps (highest value next): C-side is untested (manual memory management
in 6k+ lines of GUI code); no integration test for the sync state machine
(metadata-based 3-way decisions in `sync_now_impl`); no test for BIP-39
recovery round-trip; no fuzzing of `parse_json_value` in `gui_sync.c`.

---

## 9. Known Technical Debt

| Item | Status |
|---|---|
| Debug instrumentation (ITER_DEBUG, MARK_SET, IDLE_CALLBACK_*, CallbackMetric, etc.) | **Removed.** All `g_print`/`debug_*` helpers and the SIGUSR1 metrics handler are gone from `gui.c` and `gui/*.c`. `g_printerr` error logging in `gui_theme.c` is unaffected (legitimate error reporting, not debug instrumentation). |
| **Non-atomic saves (truncate-then-write)** | **Fixed (2026-06-12).** All three write paths (`zig_save_document`, `zig_save_active_page`, sync downloads) now go through `atomicWriteFile`: tmp file in same dir → fsync → `rename()`. Crash/power-loss mid-save leaves the original note intact. Covered by test. |
| **External-file reload dead (`RELOAD_BLOCKED_TEST` stub)** | **Fixed (2026-06-12).** Stub removed; reload is live again with a guard: if the GTK buffer has unsaved edits, reload is skipped and the status bar shows "File changed on disk (unsaved edits kept)" instead of clobbering the user's work. This also closes the sync amplifier where a stale buffer re-saved over freshly synced content. |
| **Autosave thread never spawned (doc fiction)** | **Fixed (2026-06-12).** The 30-second autosave loop is enabled — safe now that saves are atomic. |
| Leftover `SAVE_PAGE` / `RELOAD_BLOCKED_TEST` debug prints in `main.zig` | **Removed (2026-06-12).** The earlier "instrumentation removed" claim only covered the C side. |
| `file_open_in_progress` cross-thread data race (plain `bool` read by inotify thread) | **Fixed (2026-06-12).** Now `std.atomic.Value(bool)` with acquire/release ordering, matching the neighboring `global_wd` atomics. |
| Undo snapshots unbounded by size (100 full-document heap copies) | **Mitigated (2026-06-12).** Stacks are byte-capped at 64 MB total, oldest evicted. Docs no longer claim "mmap-backed". |
| **Dropbox/Local sync silently destroying conflicting edits; sync via out-of-repo bash scripts** | **Fixed (2026-06-12).** All backends ported to the Drive-style 3-way model with per-file metadata tables and `_conflict` copies; Dropbox (API v2) and GitHub (Contents API) reimplemented in native Zig HTTP — script dependency eliminated, which also removes the biggest Windows-port blocker. |
| `idle_scroll_to_cursor` accumulation | **Fixed.** `scroll_queued` flag added to `AppGui`; `on_mark_set` only schedules `idle_scroll_to_cursor` if not already queued, and the callback clears the flag on entry. |
| Dead duplicate code in `gui.c` | **Removed.** Three copies of `apply_paragraph_alignment` and a dead duplicate `on_paste_plain_text_received` existed across `gui.c`/`gui_editor.c`/`gui_popover.c`; `gui.c`'s copies were unused and deleted, the live copies kept in `gui_editor.c` (paste handler) and `gui_popover.c` (alignment helper, now exported via `gui_internal.h`). Also removed unused duplicate `apply_regex_conceal`/`apply_regex_conceal_local`/`replace_anchors_with_hrs` from `gui.c` (live copies remain in `gui_conceal.c`). |
| Cursor position char/byte unit mismatch | **Fixed.** See §4.4. |
| `Gtk-ERROR: Byte index N is off the end of the line` crash on mouse hover | **Fixed.** See §4.5 — `editor_get_iter_at_widget_point()` no longer calls the buggy `gtk_text_view_get_iter_at_position()`. |
| `GET_RANGE` debug print in `main.zig` | **Removed.** |
| `gui.c` size | Reduced from 5139 → 4155 lines by extracting PDF export and shortcuts; has since regrown to ~4900 with the prefs system, status menu, localization table, icon table, and debounced autosave. Candidates for extraction: `tr_table`/`qirtas_tr`/`qirtas_icon` → `gui_i18n.c`, settings window construction → `gui_settings.c`. `gui.c` remains exempted from the modular file size check in `build.zig`. |
| Crash-investigation harness (`simulate_crash_cb`, SIGUSR1 wiring) | **Removed.** |
| `test_*.md` files in root | **Fixed (2026-06-12).** `test_large.md` untracked from git (was tracked, making the `test_*.md` ignore pattern inert); kept on disk as profiling scratch. |
| `.bak` and `.step*` files in `src/` | Backup artifacts from the refactor, still present — recommend deleting once this branch is verified stable. |
| Scratch files tracked in `src/` (`english.txt` stale code dump, `sa.md`, `test.md`, `Wiki link.md`, `ىى.md`) | **Fixed (2026-06-12).** Untracked and gitignored; kept on disk. Root cause (app writes into its own source tree when opened as a vault) still open — needs external-files/vault separation. |
| Sync status passed as English strings over FFI, classified by `strcmp` | **Fixed (2026-06-12).** Replaced with `QirtasSyncState` enum — see §4a-quinquies. |
| `active_file_path` unchecked `@memcpy` overflow on long (esp. Arabic) paths | **Fixed (2026-06-12).** Buffer grown 256 → 1024 bytes, single bounds-checked setter refuses over-long paths, regression test added. |
| `file_history` snapshots plaintext in encrypted vaults | **Open.** Must encrypt with master key before version-restore UI ships. |
