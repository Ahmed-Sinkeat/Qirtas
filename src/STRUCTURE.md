
## Repository Layout

```text
Qirtas/
├── build.zig
├── build.zig.zon
├── src/
│   ├── main.zig                         ← Zig app root, file I/O, undo, autosave, FFI exports
│   ├── bip39.zig                        ← BIP-39 recovery phrase helpers
│   ├── sync.zig                         ← Cloud sync logic (Google Drive, Dropbox, GitHub, local)
│   ├── root.zig                         ← Zig module root
│   ├── gui.c                            ← Entry/FFI layer + redesign UI shell: run_gui,
│   │                                       activate() builds the paper-card window (tab strip,
│   │                                       editor card, outline panel, sidebar logo, status bar,
│   │                                       settings), paper-card column subsystem (paper_column_tick,
│   │                                       focus/read mode), explorer header toolbar + folder prompt,
│   │                                       app shutdown + unsaved-changes dialog, EN→AR tr_table
│   ├── gui_internal.h                   ← Canonical AppGui struct + UI-only shared state, module hooks
│   ├── gui_shared.h                     ← Zig-facing FFI declarations, Position, QirtasSyncState
│   ├── STRUCTURE.md                     ← This file
│   ├── As-Built Specification Document.md  ← Engineering spec with profiling results
│   └── gui/
│       ├── gui_theme.c                  ← CSS loading, theme switching, font selection, brand logo
│       ├── gui_buffer.c                 ← Buffer/undo core: insert/delete/replace signal wiring,
│       │                                   word-grain undo, stats+conceal debounce, autosave, Arabic counts
│       ├── gui_cursor.c                 ← Cursor trail animations
│       ├── gui_editor.c                 ← Editing, buffer events, gestures, paragraph alignment, paste
│       ├── gui_popover.c                ← Markdown formatting popup, undo sealing, paragraph alignment
│       ├── gui_conceal.c                ← Markdown concealment passes, heading tags, idle guard
│       ├── gui_wiki.c                   ← Wiki-link parsing and navigation, idle guard
│       ├── gui_hr.c                     ← Horizontal rule renderer
│       ├── gui_search.c                 ← Inline search bar overlay
│       ├── gui_explorer.c               ← File tree (rows, right-click menu, search filter)
│       ├── gui_outline.c                ← Document outline / table-of-contents panel
│       ├── gui_switcher.c               ← Quick-open file switcher
│       ├── gui_tabs.c                   ← Tab strip + tab cache + gui_reload_full_buffer (full-buffer model)
│       ├── gui_history.c               ← Per-file edit-history snapshots
│       ├── gui_shortcuts.c              ← Keyboard shortcuts table, keybindings window
│       └── gui_sync.c                   ← Cloud credentials and sync event UI
│   └── ui/
│       ├── themes/
│       │   ├── base.css                 ← Shared layout (tab strip, paper card, desk outline, status pill)
│       │   ├── theme-qirtas-light.css   ← Qirtas Light (warm paper, navy accent — matches light logo)
│       │   ├── theme-qirtas-dark.css    ← Qirtas Dark (matches dark logo, gold thread)
│       │   └── theme-qirtas-navy.css    ← Paper & Ink Navy (white paper, #213A63 navy)
│       ├── icons/                       ← App icons + qirtas-logo.png (light/navy) + qirtas-logo-dark.png
│       ├── qirtas_markdown.lang         ← GtkSourceView language definition
│       └── qirtas*.style-scheme.xml     ← Editor colour schemes: qirtas (light), qirtas-night (dark),
│                                            qirtas-navy (navy). Each gives markdown h1-h6 + bold/italic/
│                                            blockquote/inline-code/list-marker its own color (As-Built §4d)
├── scratch/                             ← Developer profiling and test scripts
│   └── profile_cursor_movement.py      ← Cursor movement profiling harness (SIGUSR1-based)
├── assets/
│   └── style.css
└── .agents/
```
## Architecture (current)

**Single canonical `AppGui` struct.** Defined once in `gui_internal.h`. `gui.c`
and every `gui/*.c` module `#include "gui_internal.h"` and share it via
`extern AppGui *global_gui`. (A clobbered build had re-injected a private,
smaller `AppGui` into `gui.c`; that has been removed — never reintroduce a
second struct.)

**Full-buffer editing model.** The Zig side (`main.zig`) is the source of truth
for document text. The whole document lives in the GTK buffer (no virtual
paging — that system was removed). Plain typing flows GTK signal → Zig
(`on_insert_text_after` / `on_delete_range_before` call `zig_insert_text` /
`zig_delete_range`). Word-grain undo seals on a typing-pause boundary or
immediately on a discrete delete.

**Programmatic edits go through `gui_buffer_replace()` (gui_buffer.c), NOT a
reload.** Formatting, list continuation, bracket/quote auto-pair, comment
toggle, line duplicate/delete/move, Tab indent, word-delete (Ctrl+Backspace/
Delete) and paste all edit the GTK buffer directly with the sync signals
blocked, mirror the change into Zig with one `zig_replace_range` (atomic = one
undo step), re-decorate only the touched lines, and defer conceal/stats. No
full-document reload, so no scroll jump. Add new edit commands this way.

**`gui_reload_full_buffer()` (gui_tabs.c) is reserved** for whole-document
state changes that GTK can't derive from signals: **undo** (restores a full
snapshot) and **file load**, plus insert-horizontal-rule (needs the separator
child-widget render). It set_texts the buffer, then defers the O(document)
decoration passes (HR widgets, RTL paragraph direction, wiki links, outline)
and the scroll restore to idle so the reload feels instant, not like a file
load. The 220ms conceal pass anchors the top-of-viewport line and scrolls it
back after the reflow so concealing markers doesn't shift the view.

**UI shell.** `gui.c`'s `activate()` builds the redesign: a floating paper
card (`editor_card`) on a desk, top tab strip, card-header breadcrumb, desk
outline panel, sidebar with brand logo + explorer header toolbar (new file /
new folder / open vault in FM) + file tree, and a status bar. `paper_column_tick`
keeps the text column centered as the card resizes. Focus mode and read mode
toggle on the same card.

**Localization.** `qirtas_tr()` (gui.c) looks up an EN→AR `tr_table` when the
app language is Arabic (persisted pref, RTL default direction). Labels are
built at startup, so a language switch needs a restart to fully apply.

**Themes (3).** `qirtas` (Qirtas Light), `qirtas-dark` (Qirtas Dark), `navy`
(Paper & Ink Navy), plus user `custom`. `apply_theme` remaps any removed/legacy
name (sepia, typewriter-*, old `dark`) to `qirtas`.

## Build Graph

`build.zig` currently builds:

1. `src/main.zig` as the Zig application root.
2. `src/root.zig` as the `qirtas` Zig module.
3. `src/gui.c`, and all modular C GUI source files under `src/gui/` as compiled C UI sources.

## Tests

`zig build test` — Zig test suite (token crypto round-trip + tamper rejection,
ISO-8601 parsing, conflict filenames, syncable-file filter, XDG path shape,
system_keys schema, active-file-path bounds check). Run it before pushing anything touching `sync.zig` or
`main.zig` key handling. C side has no tests yet — see As-Built §8 for gaps.

## Security & Sync caveats

- Vault crypto threat model is honest-documented in `docs/SECURITY.md` —
  currently protects against casual browsing only (machine-id-anchored key).
  No "encrypted"/"privacy-first" user-facing claims until that roadmap is done.
- Sync conflict behavior is unified (2026-06-12): all four backends do                                                                              
  3-way detection with per-file metadata and `_conflict` copies — no
  silent edit loss. Details in `docs/SYNC.md`.
- `file_history` snapshots (`src/gui/gui_history.c`) are stored **plaintext**
  in the vault DB — including for encrypted vaults. Must be encrypted with the
  master key before any version-restore UI ships; users will assume history
  inherits the vault's promises.
- Sync status crosses the C/Zig FFI as the `QirtasSyncState` enum
  (`gui_set_sync_state`), never as strings — string matching broke silently
  once text passed through `qirtas_tr` for Arabic users.
- Active file path goes through `setActiveFilePath` in `src/main.zig`
  (bounds-checked, refuses over-long paths rather than truncating — Arabic
  paths hit byte limits at half the character count).

## Where To Edit What

| What you want to change | File to edit |
|---|---|
| App behaviour, file I/O, autosave, inotify | `src/main.zig` |
| Debounced save-on-pause (2.5 s after typing stops) | `autosave_debounce_cb` in `src/gui.c` |
| Crash-recovery snapshot history (`file_history` table, pruning tiers) | `src/gui/gui_history.c` |
| Sync status dot states | `QirtasSyncState` enum in `src/gui_shared.h`, `gui_set_sync_state` in `src/gui.c` |
| Undo stack (heap snapshots, capped at 64 MB total), save/restore, text edit APIs | `src/main.zig` |
| Undo keybinding routing (GTK built-in undo is disabled — keep it that way) | `on_editor_key_pressed` in `src/gui/gui_editor.c`, `set_enable_undo(FALSE)` in `src/gui.c` |
| BIP-39 recovery phrase helpers | `src/bip39.zig` |
| Cloud sync logic | `src/sync.zig` |
| GTK UI layout, window setup, key handling | `src/gui.c` |
| Cloud sync status UI callbacks | `src/gui/gui_sync.c` |
| Editor gesture handling and undo commit boundaries | `src/gui/gui_editor.c` |
| Formatting popovers and post-edit undo sealing | `src/gui/gui_popover.c` |
| Markdown concealment, heading tags, buffer-generation guards | `src/gui/gui_conceal.c` |
| Wiki-link parsing and idle guards | `src/gui/gui_wiki.c` |
| Themed PDF export (متن etc., Editor — Plain), theme chooser, block renderer | `src/gui/gui_export.c` (PrintTheme struct = one theme) |
| Keyboard shortcuts table and keybindings window | `src/gui/gui_shortcuts.c` |
| UI-only shared state, window pointers, and module hooks | `src/gui_internal.h` |
| Zig-facing FFI declarations | `src/gui_shared.h` |
| App theme variables | `src/ui/themes/theme-<name>.css` |
| Shared theme/layout rules | `src/ui/themes/base.css` |
| GtkSourceView language definition | `src/ui/qirtas_markdown.lang` |
| Editor colour schemes | `src/ui/qirtas*.style-scheme.xml` |
| Build configuration | `build.zig` |
| UI string translations (English→Arabic) | `tr_table` in `src/gui.c` (`qirtas_tr`) |                           jjjjjjjj
| Icon style sets (Classic/Modern) | `icon_table` in `src/gui.c` (`qirtas_icon`) |
| Persisted editor/app preferences | `qirtas_pref_*` helpers in `src/gui/gui_cursor.c`, applied in `apply_editor_prefs` in `src/gui.c` |
| Find & Replace | `src/gui/gui_search.c` |
| Settings window contents | settings section of `activate()` in `src/gui.c` |
| Status-bar menu items | `status_menu_item` block in `src/gui.c` |
| XDG config path / legacy migration | `configDir`/`dbPathZ`/`migrateLegacyConfig` in `src/main.zig` |
| Sync setup & troubleshooting docs | `docs/SYNC.md` |
| Crypto threat model | `docs/SECURITY.md` |
| Arabic search normalization | `normalizeArabicAlloc` in `src/main.zig` (C side via `zig_normalize_arabic`) |
| Paragraph direction detection | `detect_rtl` in `src/gui/gui_conceal.c` (first-strong-char, markdown-aware) |
| Quick Switcher (Ctrl+P) | `src/gui/gui_switcher.c` |
| Outline panel (heading TOC) | `src/gui/gui_outline.c` |
| UI layout map (what is where on screen) | `docs/LAYOUT.md` |
| CI workflow | `.github/workflows/ci.yml` |
| Flatpak manifest (untested draft) | `packaging/org.qirtas.Qirtas.yml` |

## GUI Layout and Modules

`src/gui.c` contains the main entry point, window layout, scroll handling, and application setup. Specific UI subsystems are split into separate C source files under `src/gui/`:

| Module | Responsibility | File Path |
|---|---|---|
| `gui_theme` | CSS loading, theme switching, typography, and font selection (custom GtkFontDialog) | `src/gui/gui_theme.c` |
| `gui_cursor` | Cursor pointer trail animations | `src/gui/gui_cursor.c` |
| `gui_editor` | Editing, buffer event handling, gesture completion, shortcuts, paragraph alignment | `src/gui/gui_editor.c` |
| `gui_popover` | Formatting markdown tooltip popup and undo sealing after transform edits | `src/gui/gui_popover.c` |
| `gui_conceal` | Markdown syntax concealment passes, heading scale tags, buffer-generation idle guards | `src/gui/gui_conceal.c` |
| `gui_wiki` | Wiki-link parsing, document navigation, buffer-generation idle guards | `src/gui/gui_wiki.c` |
| `gui_hr` | Custom horizontal line formatting renderer | `src/gui/gui_hr.c` |
| `gui_search` | Editor inline query search bar overlay | `src/gui/gui_search.c` |
| `gui_explorer` | Directory trees and active files drawer | `src/gui/gui_explorer.c` |
| `gui_tabs` | Document tab controls (top tab strip, flat with active underline) and active buffer management | `src/gui/gui_tabs.c` |
| `gui_export` | Themed PDF export (متن classical + 4 more, incl. "Editor — Plain"), theme chooser dialog, block renderer | `src/gui/gui_export.c` |
| `gui_history` | Crash-recovery snapshots: `file_history` table in vault DB, written after autosave, tiered pruning | `src/gui/gui_history.c` |
| `gui_switcher` | Quick Switcher (Ctrl+P) fuzzy file palette | `src/gui/gui_switcher.c` |
| `gui_outline` | Outline panel content (heading TOC); the panel now lives on the desk left of the paper card as a GtkRevealer built in `gui.c` | `src/gui/gui_outline.c` |
| `gui_shortcuts` | Keyboard shortcuts table, keybindings settings window | `src/gui/gui_shortcuts.c` |
| `gui_sync` | Cloud credentials and synchronization event UI | `src/gui/gui_sync.c` |

## UI Redesign (2026-06-13)

The window chrome was reworked to close the polish gap with editors like Obsidian
while keeping the floating-paper identity. All of this is built in `activate()` in
`src/gui.c` and styled in `base.css`.

- **Top tab strip.** The document tabs moved out of the bottom bar to a pinned 42px
  strip at the very top of `main_vertical_box` (`gui->tab_strip`, LTR). Tabs are flat
  with a 2px accent underline on the active tab and an accent unsaved dot.
  `reorder_main_layout` keeps the strip anchored to the top regardless of status-bar
  position.
- **Paper card.** The editor is wrapped in a single `.editor-card` box
  (`gui->editor_card`) that carries the border / radius / shadow / desk margins:
  a 2px gradient **thread** (`gui->editor_thread`, gold on dark via `--thread-color`,
  navy on the navy theme) → a 46px **header band** (`gui->editor_header`:
  breadcrumb + reparented 🔍 search, ≡ outline toggle, ⋮ menu) → the scrolling
  `GtkSourceView`. The inner `.editor-scroll` is borderless.
- **Centred text column**, recomputed each tick by `paper_column_tick` from the
  card's width: `text_w = clamp(card_width - QIRTAS_CARD_CHROME,
  QIRTAS_TEXT_COLUMN_MIN=420, QIRTAS_TEXT_COLUMN_MAX=840)`, result cached in
  `gui->text_column_width`. `GtkSourceView` has no max-width, so this drives
  symmetric left/right text margins directly. GTK4 dropped `size-allocate`, so
  steady state polls via a ~120ms `g_timeout_add` (no-op unless width/gutter
  actually changed); a 60fps `gtk_widget_add_tick_callback` runs only while
  dragging a column edge (`on_column_resize_begin`/`_end`). In focus mode the
  card itself is centred with a capped width (`text_column_width + 160`) so the
  paper floats in the middle of the desk.
- **Text Width setting** (Editor section): "Centered (Fixed Width)" (default) vs
  "Full Page Width" — `gui->text_width_full_page` / pref `text_width_full_page`,
  toggled via `on_text_width_mode_changed`. "Full Page Width" skips the
  `QIRTAS_TEXT_COLUMN_MAX` clamp above so the column always fills the card.
- **Desk outline panel.** The heading TOC left the sidebar and now sits on the desk
  (left of the paper under RTL) as a `GtkRevealer` (`gui->outline_panel`). Toggled by
  the header ≡ button and a `×` close button; visibility persists in the
  `outline_panel_visible` pref (default on). `gui_outline_refresh` still owns content.
- **Sidebar brand header.** A `.sidebar-header` row (`qirtas-logo.png` feather+wordmark
  logo + translatable "Library" label) sits above the workspace search.
- **Floating status pill.** No bottom bar — the pill (`.status-pill`, sync dot +
  word + char counts) is a `GtkOverlay` child on the paper card, pinned bottom-**end**
  (`GTK_ALIGN_END`): bottom-left under Arabic, bottom-right under English. Search/menu
  live in the card header; the path is the breadcrumb. The empty `bottom_bar` is kept
  only so the layout/focus reorder code has a widget to move.
- **Card-header toggles.** Book/sidebar icon → workspace+files sidebar
  (`on_logo_clicked`); a separate list icon → desk outline panel.
- **Line numbers** default off; when shown, `paper_column_tick` slides the gutter via
  `gtk_widget_set_margin_start` so the digits hug the centred text column.
- **Smart Arabic counts.** `arabic_count_phrase` (`gui_buffer.c`) applies تمييز العدد
  for كلمة/حرف (1 singular, 2 dual, 3-10 spelled+plural, 11+ digits+singular).
- **Themes.** `qirtas-dark` is dark + quill-gold `#C9A86B` (matches the handoff dark
  mockup); `navy` is the white-paper navy light theme.
- **Settings** regrouped Appearance / Editor / Sync / General, explicit header bar for
  light-theme contrast; removed Status Bar Position, Layout Dividers, Scroll Past End,
  Overview Map, Right Margin.

## Buffer Model

**Current:** Full-buffer GTK editor. `GtkTextBuffer` holds the entire document. The `GtkSourceView` is the **direct scrollable child** of the `GtkScrolledWindow` — GTK validates Pango layout lazily (visible lines only) and its native scroll-to-cursor logic works. Do **not** wrap the view in a box: that allocates it at full document height, forcing whole-document layout on open (multi-second freeze on big files).

**Removed:** A virtual viewport prototype (`viewport-prototype` git tag) that loaded only a 300-line window was developed but removed. Its leftover spacer-box wrapper around the source view was also removed (2026-06-12).

## Preferences Store

`app_prefs` key/value table in the vault SQLite DB (`qirtas_pref_get_int/set_int/get_string/set_string` in `gui_cursor.c`, declared in `gui_internal.h`). Holds editor prefs (wrap, line numbers, highlight line, right margin, overview map, restore session, compact mode), `app_language` (0 = English, 1 = Arabic RTL), `icon_style` (0 = Classic, 1 = Modern), and `last_file` for session restore. Never extend the zig-owned `session_state` schema from C — use this table.

## Localization & Icons

- `qirtas_tr(en)` in `gui.c` translates UI strings via an in-file English→Arabic table; wrap any new user-visible literal in it.
- `qirtas_icon(key)` maps logical icon keys ("search", "folder", …) to Classic/Modern symbolic names.
- Arabic mode flips the whole app RTL via `gtk_widget_set_default_direction`; the top tab strip, the card header, and the bottom status pill are pinned LTR (tabs, breadcrumb, numeric counts read left-to-right in both languages). The desk row order is `[card][outline panel]`, so the panel lands left of the paper under RTL and right of it under LTR — no extra direction code needed.
- Markdown conceal must NEVER use the `invisible` tag property (GTK4 aborts on invisible + multi-byte lines); it uses 1% scale + transparent foreground at max tag priority instead.

## Buffer-Generation Guard Pattern

Every deferred callback (`g_idle_add`) that walks `GtkTextIter` over the buffer captures `buffer_generation` at schedule time and discards itself if the buffer was swapped before it ran:

```c
// Snapshot on schedule:
d->generation = gui->buffer_generation;

// Check on execute:
if (d->generation != d->gui->buffer_generation) {
    g_free(d);
    return G_SOURCE_REMOVE;  // stale — buffer replaced, drop silently
}
```

Modules using this pattern: `gui_conceal.c`, `gui_wiki.c`, `gui_hr.c`, `gui_popover.c`.

## Document Ownership Model

- **Backend (Zig)**: Owns the complete document state (Source of Truth). Manages line offsets, file system updates, persistent state, and all document edits.
- **Frontend (GTK/C)**: Presentation layer. Loads document text from Zig via `zig_get_document_text()` and renders it in a full `GtkTextBuffer`.

## Theme System

Themes use two layers:

1. `src/ui/themes/theme-<name>.css` — Color tokens
2. `src/ui/themes/base.css` — Shared layout, spacing, widget styling, unsaved tab indicator

Default typography: `Inter` (premium writing experience). Tabs are consolidated inside the status bar (`.tab-bar`) to minimise vertical screen clutter.

### Adding A Theme

1. Copy `src/ui/themes/theme-sepia.css` to a new file.
2. Update the color tokens.
3. Add a branch in `apply_theme()` in `src/gui.c`.
4. Add the theme to the settings dropdown.
5. Optionally add a matching GtkSourceView style scheme.

## FFI Bridge

Both C and Zig communicate via memory-mapped C linkage.

### C functions called from Zig (Declared in `gui_shared.h` / `main.zig` externs)

- `void gui_set_text(const char *text, int len)`: Sets GtkTextBuffer content from Zig-owned text.
- `void gui_set_title(const char *title)`: Updates window title and selects active tab.
- `void gui_set_sync_state(QirtasSyncState state)`: Updates the sync status dot (enum, never translated strings).
- `void gui_show_editor(void)`: Switches workspace view stack to the editor page.
- `void gui_show_recovery_dialog(void)`: Opens the recovery modal.
- `void gui_get_cursor_position(int *line, int *col)`: Retrieves current cursor position. `col` is a **character** offset (`gtk_text_iter_get_line_offset`), matching `Position.col` everywhere else in the codebase.
- `void gui_set_cursor_position(int line, int col)`: Restores cursor. `col` is clamped to `gtk_text_iter_get_chars_in_line()` (character count, not byte count) before calling `gtk_text_buffer_get_iter_at_line_offset`.
- `void gui_refresh_explorer(void)`: Refreshes directory tree explorer on idle.
- `void gui_trigger_autosave(void)`: Invokes active page save logic in Zig backend.
- `void gui_run_on_main_thread(void (*callback)(void *), void *user_data)`: Runs C functions safely from Zig threads.
- `void gui_update_sync_status(int connected, const char *status_text)`: Updates Google sync status.
- `void gui_update_dropbox_status(int connected, const char *status_text)`: Updates Dropbox status.
- `void gui_update_github_status(int connected, const char *status_text)`: Updates GitHub status.
- `void gui_update_local_sync_status(int connected, const char *status_text)`: Updates local sync status.
- `void gui_tabs_close(AppGui *gui, int index)`: Closes a document tab.
- `void gui_tabs_add_or_select(AppGui *gui, const char *filepath)`: Opens or focuses a tab.

### Zig functions called from C (Declared in `gui_shared.h` / `main.zig`)

- `void zig_on_gui_ready(void)`: Signals UI setup completion.
- `int zig_has_active_master_key(void)`: Reports whether the backend has unlocked the master key.
- `void zig_open_file(const char *filename)`: Opens a file and loads content.
- `void zig_open_vault(const char *dir_path)`: Toggles active project directories.
- `void zig_search_workspace(const char *query)`: Queries local files for matches.
- `const char *zig_get_search_snippet(const char *filepath)`: Retrieves search match highlight preview.
- `int zig_get_search_rank(const char *filepath)`: Ranks search results.
- `void zig_set_cursor_trail(int enabled)`: Saves cursor animation config.
- `int zig_get_cursor_trail(void)`: Returns cursor animation config.
- `void zig_open_wiki_link(const char *note_name)`: Resolves or auto-generates linked markdown files.
- `void zig_create_new_file(const char *filename)`: Initializes new notebook documents.
- `void zig_on_shutdown(void)`: Triggered on app shutdown to save states.
- `void zig_force_save(void)`: Triggers immediate data flush to disk.
- `void zig_set_editor_border(int enabled)` / `int zig_get_editor_border(void)`: Configures layout margins.
- `int zig_dropbox_check_status(void)` / `int zig_github_check_status(void)`: Check cloud connection status.
