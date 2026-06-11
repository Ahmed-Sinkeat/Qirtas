# Qirtas — As-Built Specification Document

**Version:** 0.9.4-dev
**Branch:** full-buffer-editor-v2
**Updated:** 2026-06-12

---

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

Sync fires only on: Save, App Close, or "Sync Now". Supports Google Drive, Dropbox, GitHub (Device Flow), and Local folder sync.

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

## 4b. Preferences, Localization, Icons (2026-06-12)

- **`app_prefs` store** — generic key/value table in the vault DB (`qirtas_pref_*` helpers, `gui_cursor.c`). Holds: `wrap_lines`, `show_line_numbers`, `highlight_current_line`, `show_right_margin`, `right_margin_pos`, `show_overview_map`, `restore_session`, `compact_mode`, `app_language`, `icon_style`, `last_file`.
- **Settings** moved out of the sidebar into the status-bar menu (☰ → Preferences, Ctrl+,). New entries: line numbers, highlight current line, overview map (GtkSourceMap overlay on the editor card), right margin + position, restore session, compact layout, language, icon style.
- **Status-bar menu** also carries: Copy File (puts the active `.md` on the clipboard as a `text/uri-list` file), Save As, Find/Replace, Fullscreen, Keyboard Shortcuts, Quit (for environments without window decorations).
- **Find & Replace** — second row in the search bar; uses `gtk_source_search_context_replace[_all]`.
- **Localization** — `qirtas_tr()` English→Arabic table in `gui.c`; Arabic mode flips app RTL (`gtk_widget_set_default_direction`), status bar pinned LTR. Labels apply on next launch; direction flips live.
- **Icon styles** — `qirtas_icon()` logical-key lookup, Classic/Modern symbolic sets; main bar + explorer icons swap live, popovers on next launch.
- **Caret color fix** — custom pointer color is emitted from the font provider (`APPLICATION+1`); emitting it only from the theme provider was silently overridden.

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
│       ├── gui_tabs.c                   ← Document tab controls in status bar
│       ├── gui_pdf.c                    ← PDF export (print pagination, draw, save dialog)
│       ├── gui_shortcuts.c              ← Keyboard shortcuts table, keybindings window
│       └── gui_sync.c                   ← Cloud credentials and sync event UI
│   └── ui/
│       ├── themes/
│       │   ├── base.css                 ← Shared layout, spacing, widget styles
│       │   ├── theme-dark.css
│       │   ├── theme-midnight.css
│       │   ├── theme-qirtas-light.css   ← Paper & Ink light
│       │   ├── theme-qirtas-dark.css    ← Paper & Ink dark
│       │   ├── theme-sepia.css
│       │   ├── theme-things.css
│       │   ├── theme-typewriter-dark.css
│       │   └── theme-typewriter-light.css
│       ├── icons/
│       ├── qirtas_markdown.lang         ← GtkSourceView language definition
│       └── qirtas*.style-scheme.xml     ← Editor colour schemes
├── docs/
│   ├── SYNC.md                          ← Sync setup, troubleshooting, conflict matrix
│   └── SECURITY.md                      ← Crypto threat model + roadmap
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
| `gui_set_sync_status(status)` | Updates status pill |
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

---

## 7. Theme System

Themes use two CSS layers:

1. `src/ui/themes/theme-<name>.css` — Color tokens per theme
2. `src/ui/themes/base.css` — Shared layout, spacing, and widget styles

Default typography: **Inter** (premium writing experience). Tabs are consolidated inside the status bar to minimize vertical clutter.

### Adding a Theme

1. Copy `src/ui/themes/theme-dark.css`.
2. Update color tokens.
3. Add branch in `apply_theme()` in `src/gui.c`.
4. Add to settings dropdown.
5. Optionally add matching GtkSourceView style scheme.

---

## 8. Testing

`zig build test` builds and runs the Zig test suite (`build.zig` wires the
GTK/sqlite linkage for it). Coverage as of 2026-06-12:

- `main.zig` — system_keys schema shape test
- `sync.zig` — token crypto round-trip (encrypt→decrypt identity), tamper
  rejection (modified ciphertext must fail authentication), ISO-8601
  timestamp parsing, conflict filename generation, syncable-file filter,
  XDG path resolution shape

Known gaps (highest value next): C-side is untested (manual memory management
in 6k+ lines of GUI code); no integration test for the sync state machine
(metadata-based 3-way decisions in `sync_now_impl`); no test for BIP-39
recovery round-trip; no fuzzing of `parse_json_value` in `gui_sync.c`.

---

## 9. Known Technical Debt

| Item | Status |
|---|---|
| Debug instrumentation (ITER_DEBUG, MARK_SET, IDLE_CALLBACK_*, CallbackMetric, etc.) | **Removed.** All `g_print`/`debug_*` helpers and the SIGUSR1 metrics handler are gone from `gui.c` and `gui/*.c`. `g_printerr` error logging in `gui_theme.c` is unaffected (legitimate error reporting, not debug instrumentation). |
| `idle_scroll_to_cursor` accumulation | **Fixed.** `scroll_queued` flag added to `AppGui`; `on_mark_set` only schedules `idle_scroll_to_cursor` if not already queued, and the callback clears the flag on entry. |
| Dead duplicate code in `gui.c` | **Removed.** Three copies of `apply_paragraph_alignment` and a dead duplicate `on_paste_plain_text_received` existed across `gui.c`/`gui_editor.c`/`gui_popover.c`; `gui.c`'s copies were unused and deleted, the live copies kept in `gui_editor.c` (paste handler) and `gui_popover.c` (alignment helper, now exported via `gui_internal.h`). Also removed unused duplicate `apply_regex_conceal`/`apply_regex_conceal_local`/`replace_anchors_with_hrs` from `gui.c` (live copies remain in `gui_conceal.c`). |
| Cursor position char/byte unit mismatch | **Fixed.** See §4.4. |
| `Gtk-ERROR: Byte index N is off the end of the line` crash on mouse hover | **Fixed.** See §4.5 — `editor_get_iter_at_widget_point()` no longer calls the buggy `gtk_text_view_get_iter_at_position()`. |
| `GET_RANGE` debug print in `main.zig` | **Removed.** |
| `gui.c` size | Reduced from 5139 → 4155 lines by extracting PDF export and shortcuts; has since regrown to ~4700 with the prefs system, status menu, localization table, and icon table. Candidates for extraction: `tr_table`/`qirtas_tr`/`qirtas_icon` → `gui_i18n.c`, settings window construction → `gui_settings.c`. `gui.c` remains exempted from the modular file size check in `build.zig`. |
| Crash-investigation harness (`simulate_crash_cb`, SIGUSR1 wiring) | **Removed.** |
| `test_*.md` files in root | Temporary profiling files, still present — recommend gitignoring or deleting. |
| `.bak` and `.step*` files in `src/` | Backup artifacts from the refactor, still present — recommend deleting once this branch is verified stable. |
