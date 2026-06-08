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
3. `src/gui.c`, `src/gui_sync.c`, and `src/gui/` phase-1 modular GUI sources as the compiled C UI sources.

## Where To Edit What

| What you want to change | File to edit |
|---|---|
| App behaviour, file I/O, autosave, inotify | `src/main.zig` |
| BIP-39 recovery phrase helpers | `src/bip39.zig` |
| Cloud sync logic | `src/sync.zig` |
| GTK UI layout and widget wiring | `src/gui.c` |
| Cloud sync status UI callbacks | `src/gui_sync.c` |
| UI-only shared state, window pointers, and module hooks | `src/gui_internal.h` |
| Zig-facing FFI declarations | `src/gui_shared.h` |
| App theme variables | `src/ui/themes/theme-<name>.css` |
| Shared theme/layout rules | `src/ui/themes/base.css` |
| GtkSourceView language definition | `src/ui/qirtas_markdown.lang` |
| Editor colour schemes | `src/ui/qirtas*.style-scheme.xml` |
| Build configuration | `build.zig` |

## GUI Layout In `gui.c`

`src/gui.c` contains the GTK UI in logical sections. The other `gui_*.c` files in the `src/` directory are stubs for a planned modular split. The current split is internal organization within `gui.c`, not separate compiled modules.

| Section | Responsibility |
|---|---|
| `gui_theme` | CSS loading, theme switching, font updates (`src/gui/gui_theme.c`) |
| `gui_cursor_trail` | Cursor trail animation (`src/gui/gui_cursor.c`) |
| `gui_editor` | Editing, formatting, keyboard shortcuts |
| `gui_popover` | Formatting popover |
| `gui_wiki` | Wiki-link tagging and navigation |
| `gui_hr` | Horizontal rule rendering (`src/gui/gui_hr.c`) |
| `gui_conceal` | Markdown concealment |
| `gui_search` | In-file search bar |
| `gui_explorer` | File/vault tree explorer |
| `gui_settings` | Settings window and shortcuts |
| `gui_sync` | Sync status UI callbacks (in `src/gui_sync.c`) |

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
