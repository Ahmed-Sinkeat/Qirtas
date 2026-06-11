
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
│   ├── gui.c                            ← GTK layout, window setup, key handling, scroll
│   ├── gui_internal.h                   ← UI-only shared state, AppGui struct, module hooks
│   ├── gui_shared.h                     ← Zig-facing FFI declarations
│   ├── STRUCTURE.md                     ← This file
│   ├── As-Built Specification Document.md  ← Engineering spec with profiling results
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
│       │   ├── base.css                 ← Shared layout, spacing, widget styles (incl. compact-ui, paper card)
│       │   ├── theme-dark.css
│       │   ├── theme-midnight.css
│       │   ├── theme-qirtas-light.css   ← Paper & Ink light (matches light logo)
│       │   ├── theme-qirtas-dark.css    ← Paper & Ink dark (matches dark logo)
│       │   ├── theme-sepia.css
│       │   ├── theme-things.css
│       │   ├── theme-typewriter-dark.css
│       │   ├── theme-typewriter-light.css
│       │   └── README.md
│       ├── icons/
│       ├── qirtas_markdown.lang         ← GtkSourceView language definition
│       └── qirtas*.style-scheme.xml     ← Editor colour schemes (qirtas = ink light, qirtas-night = ink dark)
├── scratch/                             ← Developer profiling and test scripts
│   └── profile_cursor_movement.py      ← Cursor movement profiling harness (SIGUSR1-based)
├── assets/
│   └── style.css
└── .agents/
```

## Build Graph

`build.zig` currently builds:

1. `src/main.zig` as the Zig application root.
2. `src/root.zig` as the `qirtas` Zig module.
3. `src/gui.c`, and all modular C GUI source files under `src/gui/` as compiled C UI sources.

## Tests

`zig build test` — Zig test suite (token crypto round-trip + tamper rejection,
ISO-8601 parsing, conflict filenames, syncable-file filter, XDG path shape,
system_keys schema). Run it before pushing anything touching `sync.zig` or
`main.zig` key handling. C side has no tests yet — see As-Built §8 for gaps.

## Security & Sync caveats

- Vault crypto threat model is honest-documented in `docs/SECURITY.md` —
  currently protects against casual browsing only (machine-id-anchored key).
  No "encrypted"/"privacy-first" user-facing claims until that roadmap is done.
- Sync conflict behavior differs per backend — `docs/SYNC.md` matrix.
  Dropbox and Local folder sync can silently lose edits on two-machine
  conflicts; only Google Drive makes `_conflict` copies.

## Where To Edit What

| What you want to change | File to edit |
|---|---|
| App behaviour, file I/O, autosave, inotify | `src/main.zig` |
| Undo stack, mmap-backed snapshots, save/restore, text edit APIs | `src/main.zig` |
| BIP-39 recovery phrase helpers | `src/bip39.zig` |
| Cloud sync logic | `src/sync.zig` |
| GTK UI layout, window setup, key handling | `src/gui.c` |
| Cloud sync status UI callbacks | `src/gui/gui_sync.c` |
| Editor gesture handling and undo commit boundaries | `src/gui/gui_editor.c` |
| Formatting popovers and post-edit undo sealing | `src/gui/gui_popover.c` |
| Markdown concealment, heading tags, buffer-generation guards | `src/gui/gui_conceal.c` |
| Wiki-link parsing and idle guards | `src/gui/gui_wiki.c` |
| PDF export (printing, pagination, save dialog) | `src/gui/gui_pdf.c` |
| Keyboard shortcuts table and keybindings window | `src/gui/gui_shortcuts.c` |
| UI-only shared state, window pointers, and module hooks | `src/gui_internal.h` |
| Zig-facing FFI declarations | `src/gui_shared.h` |
| App theme variables | `src/ui/themes/theme-<name>.css` |
| Shared theme/layout rules | `src/ui/themes/base.css` |
| GtkSourceView language definition | `src/ui/qirtas_markdown.lang` |
| Editor colour schemes | `src/ui/qirtas*.style-scheme.xml` |
| Build configuration | `build.zig` |

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
| `gui_tabs` | Document tab controls (inside the status bar) and active buffer management | `src/gui/gui_tabs.c` |
| `gui_pdf` | PDF export (print pagination, page drawing, save dialog) | `src/gui/gui_pdf.c` |
| `gui_shortcuts` | Keyboard shortcuts table, keybindings settings window | `src/gui/gui_shortcuts.c` |
| `gui_sync` | Cloud credentials and synchronization event UI | `src/gui/gui_sync.c` |

## Buffer Model

**Current:** Full-buffer GTK editor. `GtkTextBuffer` holds the entire document. The `GtkSourceView` is the **direct scrollable child** of the `GtkScrolledWindow` — GTK validates Pango layout lazily (visible lines only) and its native scroll-to-cursor logic works. Do **not** wrap the view in a box: that allocates it at full document height, forcing whole-document layout on open (multi-second freeze on big files).

**Removed:** A virtual viewport prototype (`viewport-prototype` git tag) that loaded only a 300-line window was developed but removed. Its leftover spacer-box wrapper around the source view was also removed (2026-06-12).

## Preferences Store

`app_prefs` key/value table in the vault SQLite DB (`qirtas_pref_get_int/set_int/get_string/set_string` in `gui_cursor.c`, declared in `gui_internal.h`). Holds editor prefs (wrap, line numbers, highlight line, right margin, overview map, restore session, compact mode), `app_language` (0 = English, 1 = Arabic RTL), `icon_style` (0 = Classic, 1 = Modern), and `last_file` for session restore. Never extend the zig-owned `session_state` schema from C — use this table.

## Localization & Icons

- `qirtas_tr(en)` in `gui.c` translates UI strings via an in-file English→Arabic table; wrap any new user-visible literal in it.
- `qirtas_icon(key)` maps logical icon keys ("search", "folder", …) to Classic/Modern symbolic names.
- Arabic mode flips the whole app RTL via `gtk_widget_set_default_direction`; the bottom status bar is pinned LTR.
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

1. Copy `src/ui/themes/theme-dark.css` to a new file.
2. Update the color tokens.
3. Add a branch in `apply_theme()` in `src/gui.c`.
4. Add the theme to the settings dropdown.
5. Optionally add a matching GtkSourceView style scheme.

## FFI Bridge

Both C and Zig communicate via memory-mapped C linkage.

### C functions called from Zig (Declared in `gui_shared.h` / `main.zig` externs)

- `void gui_set_text(const char *text, int len)`: Sets GtkTextBuffer content from Zig-owned text.
- `void gui_set_title(const char *title)`: Updates window title and selects active tab.
- `void gui_set_sync_status(const char *status)`: Updates the sync status text pill.
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
