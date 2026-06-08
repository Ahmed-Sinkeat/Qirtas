# Qirtas Source Structure

Qirtas uses Zig for app logic, file I/O, and sync, plus a C GTK4/Adwaita UI layer.
The executable entry point is `src/main.zig`.

## Repository Layout

```text
Qirtas/
├── build.zig
├── build.zig.zon
├── src/
│   ├── main.zig
│   ├── bip39.zig
│   ├── sync.zig
│   ├── root.zig
│   ├── gui.c
│   ├── gui_internal.h
│   ├── gui_shared.h
│   ├── gui/
│   │   ├── gui_theme.c
│   │   ├── gui_cursor.c
│   │   ├── gui_hr.c
│   │   ├── gui_search.c
│   │   ├── gui_conceal.c
│   │   ├── gui_wiki.c
│   │   ├── gui_popover.c
│   │   ├── gui_explorer.c
│   │   ├── gui_tabs.c
│   │   ├── gui_editor.c
│   │   └── gui_sync.c
│   └── ui/
│       ├── themes/
│       │   ├── base.css
│       │   ├── theme-dark.css
│       │   ├── theme-midnight.css
│       │   ├── theme-qirtas-light.css
│       │   ├── theme-sepia.css
│       │   ├── theme-things.css
│       │   ├── theme-typewriter-dark.css
│       │   ├── theme-typewriter-light.css
│       │   └── README.md
│       ├── icons/
│       ├── qirtas_markdown.lang
│       └── qirtas*.style-scheme.xml
├── assets/
│   └── style.css
├── codex/
└── .agents/
```

## Build Graph

`build.zig` currently builds:

1. `src/main.zig` as the Zig application root.
2. `src/root.zig` as the `qirtas` Zig module.
3. `src/gui.c`, `src/gui/gui_sync.c`, and all modular C GUI source files under `src/gui/` as compiled C UI sources.

## Where To Edit What

| What you want to change | File to edit |
|---|---|
| App behaviour, file I/O, autosave, inotify | `src/main.zig` |
| BIP-39 recovery phrase helpers | `src/bip39.zig` |
| Cloud sync logic | `src/sync.zig` |
| GTK UI layout and window setup | `src/gui.c` |
| Cloud sync status UI callbacks | `src/gui/gui_sync.c` |
| UI-only shared state, window pointers, and module hooks | `src/gui_internal.h` |
| Zig-facing FFI declarations | `src/gui_shared.h` |
| App theme variables | `src/ui/themes/theme-<name>.css` |
| Shared theme/layout rules | `src/ui/themes/base.css` |
| GtkSourceView language definition | `src/ui/qirtas_markdown.lang` |
| Editor colour schemes | `src/ui/qirtas*.style-scheme.xml` |
| Build configuration | `build.zig` |

## GUI Layout and Modules

`src/gui.c` contains the main entry point, application setup, and main layout structure. Other specific UI modules are split into separate C source files under `src/gui/`:

| Module | Responsibility | File Path |
|---|---|---|
| `gui_theme` | CSS loading, theme switching, font selection options | `src/gui/gui_theme.c` |
| `gui_cursor` | Smear pointer trail animations | `src/gui/gui_cursor.c` |
| `gui_editor` | Editing, buffer event handling, shortcuts | `src/gui/gui_editor.c` |
| `gui_popover` | Formatting markdown tooltip popup | `src/gui/gui_popover.c` |
| `gui_wiki` | Wiki-link parsing and document navigation | `src/gui/gui_wiki.c` |
| `gui_hr` | Custom horizontal line formatting renderer | `src/gui/gui_hr.c` |
| `gui_conceal` | Markdown syntax concealment passes and headers | `src/gui/gui_conceal.c` |
| `gui_search` | Editor inline query search bar overlay | `src/gui/gui_search.c` |
| `gui_explorer` | Directory trees and active files drawer | `src/gui/gui_explorer.c` |
| `gui_tabs` | Document tab controls and active buffers | `src/gui/gui_tabs.c` |
| `gui_sync` | Cloud credentials and synchronization events UI | `src/gui/gui_sync.c` |

## Theme System

Themes use two layers:

1. `src/ui/themes/theme-<name>.css`
   - Color tokens
2. `src/ui/themes/base.css`
   - Shared layout, spacing, widget styling, unsaved tab indicator

### Adding A Theme

1. Copy `src/ui/themes/theme-dark.css` to a new file.
2. Update the color tokens.
3. Add a branch in `apply_theme()` in `src/gui.c`.
4. Add the theme to the settings dropdown.
5. Optionally add a matching GtkSourceView style scheme.

## FFI Bridge

### C functions called from Zig (Declared in `gui_shared.h` / `main.zig`)

- `char *gui_get_text(void)`: Gets text from editor buffer.
- `void gui_free_text(char *text)`: Safely frees text allocated in C layer.
- `void gui_set_text(const char *text, int len)`: Sets text buffer and resets cursor trail.
- `void gui_set_title(const char *title)`: Updates window title and selects active tab.
- `void gui_set_sync_status(const char *status)`: Updates the sync status text pill.
- `void gui_show_editor(void)`: Switches workspace view stack to the editor page.
- `void gui_show_recovery_dialog(void)`: Opens the recovery modal when the backend cannot unlock the vault.
- `void gui_get_cursor_position(int *line, int *col)`: Retrieves current cursor position.
- `void gui_set_cursor_position(int line, int col)`: Restores cursor position.
- `void gui_refresh_explorer(void)`: Refreshes directory tree explorer on idle.
- `void gui_set_virtual_scroll_mode(int enabled, int total_lines)`: Configures virtual scrolling mode.
- `void gui_init_virtual_document(int total_lines, int start_line, int end_line)`: Sets up virtual layout variables.
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
