
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
│   ├── gui.c                            ← GTK layout, viewport scroll, window setup
│   ├── gui_internal.h                   ← UI-only shared state, AppGui struct, module hooks
│   ├── gui_shared.h                     ← Zig-facing FFI declarations
│   └── gui/
│       ├── gui_theme.c                  ← CSS loading, theme switching, font selection
│       ├── gui_cursor.c                 ← Cursor trail animations
│       ├── gui_editor.c                 ← Editing, buffer events, gestures, shortcuts
│       ├── gui_popover.c                ← Markdown formatting popup, undo sealing
│       ├── gui_conceal.c                ← Markdown concealment passes, heading tags, idle guard
│       ├── gui_wiki.c                   ← Wiki-link parsing and navigation, idle guard
│       ├── gui_hr.c                     ← Horizontal rule renderer
│       ├── gui_search.c                 ← Inline search bar overlay
│       ├── gui_explorer.c               ← Directory tree and active files drawer
│       ├── gui_tabs.c                   ← Document tab controls in status bar
│       └── gui_sync.c                   ← Cloud credentials and sync event UI
│   └── ui/
│       ├── themes/
│       │   ├── base.css                 ← Shared layout, spacing, widget styles
│       │   ├── theme-dark.css
│       │   ├── theme-midnight.css
│       │   ├── theme-qirtas-light.css
│       │   ├── theme-sepia.css
│       │   ├── theme-things.css
│       │   ├── theme-typewriter-dark.css
│       │   ├── theme-typewriter-light.css
│       │   └── README.md
│       ├── icons/
│       ├── qirtas_markdown.lang         ← GtkSourceView language definition
│       └── qirtas*.style-scheme.xml     ← Editor colour schemes
├── assets/
│   └── style.css
├── docs/
│   └── plans/                           ← Engineering decision logs and recovery plans
├── codex/
└── .agents/
```

## Build Graph

`build.zig` currently builds:

1. `src/main.zig` as the Zig application root.
2. `src/root.zig` as the `qirtas` Zig module.
3. `src/gui.c`, and all modular C GUI source files under `src/gui/` as compiled C UI sources.

## Where To Edit What

| What you want to change | File to edit |
|---|---|
| App behaviour, file I/O, autosave, inotify | `src/main.zig` |
| Undo stack, mmap-backed snapshots, save/restore, text edit APIs | `src/main.zig` |
| Virtual scroll threshold and adaptive page sizing | `src/main.zig` |
| BIP-39 recovery phrase helpers | `src/bip39.zig` |
| Cloud sync logic | `src/sync.zig` |
| GTK UI layout, window setup, and viewport scrolling | `src/gui.c` |
| Scroll debounce, direction lock, fire_scroll timer | `src/gui.c` |
| Cloud sync status UI callbacks | `src/gui/gui_sync.c` |
| Editor gesture handling and undo commit boundaries | `src/gui/gui_editor.c` |
| Formatting popovers and post-edit undo sealing | `src/gui/gui_popover.c` |
| Markdown concealment, heading tags, buffer-generation guards | `src/gui/gui_conceal.c` |
| Wiki-link parsing and idle guards | `src/gui/gui_wiki.c` |
| UI-only shared state, window pointers, and module hooks | `src/gui_internal.h` |
| Zig-facing FFI declarations | `src/gui_shared.h` |
| App theme variables | `src/ui/themes/theme-<name>.css` |
| Shared theme/layout rules | `src/ui/themes/base.css` |
| GtkSourceView language definition | `src/ui/qirtas_markdown.lang` |
| Editor colour schemes | `src/ui/qirtas*.style-scheme.xml` |
| Build configuration | `build.zig` |

## GUI Layout and Modules

`src/gui.c` contains the main entry point, layout manager, viewport reload guard, scroll debounce logic, and application setup. Specific UI modules are split into separate C source files under `src/gui/`:

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
| `gui_sync` | Cloud credentials and synchronization event UI | `src/gui/gui_sync.c` |

## Virtual Scroll & Viewport Loading

> [!NOTE]
> **Status: Stable (v0.9.1)**
> The virtual viewport subsystem is now stable. Buffer-generation guards, scroll direction locking, and adaptive page sizing have resolved the re-entrancy crashes and infinite scroll loops seen in earlier sessions.

The virtual viewport maps only the visible line window into GTK's `GtkTextBuffer`. The rest of the document is owned by Zig and fetched on demand:

```text
Total Document Lines (e.g. 17,556 lines total, owned by Zig backend)
0 ------------------------------------------------------------------- 17,556
                  [ Loaded Viewport Page Window (GTK buffer slice) ]
                14,434 ------------------- 14,534
                         [ Visible Viewport ]
                        (Scrollbar Adjustment)
```

1. **Active Range Boundaries (`viewport_set_range`)**: Configures active line boundaries (`active_page_start_line` / `active_page_end_line`) and manages 1px-minimum top and bottom layout spacers.
2. **Viewport Request (`request_viewport_position`)**: Single decision point for scroll, cursor, and open-file requests. Reloads only when the target line is outside safe margins.
3. **Adaptive Page Loading (`load_viewport_page` + `get_page_size`)**: Selects page size (400 / 300 / 250 lines) based on total document line count. Increments `buffer_generation` before swapping the buffer, causing all pending idle callbacks to self-cancel.
4. **Scroll Debounce (`on_scroll_changed` → `fire_scroll`)**: 60ms `g_timeout_add` debounce. Tracks scroll direction; suppresses direction reversals within 200ms to prevent spacer-collapse feedback loops.

### Buffer-Generation Guard Pattern

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

Modules using this pattern: `gui_conceal.c` (global + local conceal), `gui_wiki.c` (global + local wiki tags), `gui_conceal.c` (`idle_scroll_to_cursor`), `gui_popover.c` (`idle_scroll_to_cursor`).

## Document Ownership Model

- **Backend (Zig)**: Owns the complete document state (Source of Truth). Maintains line offsets, file system updates, persistent state, and all document edits.
- **Frontend (GTK/C)**: Owns only the visual viewport representation — a presentation layer displaying the active text slice.

> [!NOTE]
> Live document reads come from Zig-side accessors: `zig_get_document_text()` and `zig_get_text_for_line_range()`. The GTK `GtkTextBuffer` is viewport-only and always refreshed from Zig-owned content.

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

- `void gui_set_text(const char *text, int len)`: Sets viewport slice, increments `buffer_generation`, updates layout.
- `void gui_set_title(const char *title)`: Updates window title and selects active tab.
- `void gui_set_sync_status(const char *status)`: Updates the sync status text pill.
- `void gui_show_editor(void)`: Switches workspace view stack to the editor page.
- `void gui_show_recovery_dialog(void)`: Opens the recovery modal when the backend cannot unlock the vault.
- `void gui_get_cursor_position(int *line, int *col)`: Retrieves current cursor position.
- `void gui_set_cursor_position(int line, int col)`: Restores cursor (clamped to line byte length) through shared viewport request path.
- `void request_viewport_position(AppGui *gui, int abs_line)`: Single viewport request entry point; reloads only when outside safe margins.
- `void gui_refresh_explorer(void)`: Refreshes directory tree explorer on idle.
- `void gui_set_virtual_scroll_mode(int enabled, int total_lines)`: Configures virtual scrolling mode.
- `void gui_init_virtual_document(int total_lines, int start_line, int end_line)`: Sets up virtual layout variables and spacer ranges.
- `void gui_trigger_autosave(void)`: Invokes active page save logic in Zig backend.
- `void gui_get_active_page_bounds(int *start_line, int *end_line, int *total_lines)`: Gets current layout bounds.
- `void gui_update_total_virtual_lines(int total_lines)`: Synchronizes virtual page size.
- `void gui_run_on_main_thread(void (*callback)(void *), void *user_data)`: Runs C functions safely from Zig threads.
- `void gui_update_sync_status(int connected, const char *status_text)`: Updates Google/Dropbox sync status.
- `void gui_update_dropbox_status(int connected, const char *status_text)`: Updates Dropbox status text.
- `void gui_update_github_status(int connected, const char *status_text)`: Updates GitHub status text.
- `void gui_update_local_sync_status(int connected, const char *status_text)`: Updates local sync status text.
- `void gui_tabs_save_active_to_cache(void)`: Saves current active tab buffer to in-memory cache.
- `void gui_tabs_restore_active_from_cache(void)`: Restores active tab buffer and modified state from cache.

### Zig functions called from C (Declared in `gui_shared.h` / `main.zig`)

- `void zig_on_gui_ready(void)`: Signals UI setup completion.
- `int zig_has_active_master_key(void)`: Reports whether the backend has unlocked the master key.
- `void zig_open_file(const char *filename)`: Opens a file, updates watch lists, and loads content.
- `void zig_open_vault(const char *dir_path)`: Toggles active project directories.
- `void zig_search_workspace(const char *query)`: Queries local files for matches.
- `const char *zig_get_search_snippet(const char *filepath)`: Retrieves search match highlight preview.
- `int zig_get_search_rank(const char *filepath)`: Ranks search results.
- `void zig_set_cursor_trail(int enabled)`: Saves user configuration for the cursor animation.
- `int zig_get_cursor_trail(void)`: Returns active cursor trail configuration.
- `void zig_open_wiki_link(const char *note_name)`: Resolves or auto-generates linked markdown files.
- `void zig_create_new_file(const char *filename)`: Initializes new notebook documents.
- `void zig_on_shutdown(void)`: Triggered on window close to save states.
- `void zig_force_save(void)`: Triggers immediate data flush to disk.
- `void zig_save_sync_credentials(...)` / `zig_sync_connect(...)` / `zig_sync_now(...)`: Google Drive sync routines.
- `void zig_save_dropbox_credentials(...)` / `zig_dropbox_connect(...)` / `zig_dropbox_now(...)`: Dropbox sync routines.
- `void zig_save_github_credentials(...)` / `zig_github_connect(...)` / `zig_github_now(...)`: GitHub sync routines.
- `void zig_local_sync_now(...)`: Direct folder-to-folder synchronization.
- `void zig_set_editor_border(int enabled)` / `int zig_get_editor_border(void)`: Configures layout margins.
