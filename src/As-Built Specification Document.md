# Qirtas — As-Built Specification Document

**Version:** 0.9.2-dev
**Branch:** measure-cursor-v3
**Updated:** 2026-06-11

---

## 1. Project Summary

Qirtas is a focused, privacy-first markdown notebook for Linux. It combines a Zig backend for file I/O, autosave, encryption, and cloud sync with a C GTK4/Adwaita frontend for native rendering and editing.

**Design Principles:**
- Native GTK4 rendering, no Electron
- Zig backend owns all persistent state
- C frontend is a pure presentation layer
- ChaCha20Poly1305 encryption for local vault files
- On-demand cloud sync (not continuous)

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

### 2.4 Cloud Sync: On-Demand Event-Driven

Sync fires only on: Save, App Close, or "Sync Now". Supports Google Drive, Dropbox, GitHub (Device Flow), and Local folder sync.

---

## 3. Architecture: Buffer Model

### 3.1 Current State (Full Buffer)

The GTK `GtkTextBuffer` holds the **entire document**. GTK manages scrolling natively. Pango computes layout coordinates for each line.

```
Document (owned by Zig backend)
↓ zig_get_document_text()
GtkTextBuffer (entire file)
↓ GtkSourceView → GTK scroll
Visible viewport (native GTK scroll position)
```

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
- `idle_scroll_to_cursor` — scrolls the view to keep cursor visible

### 4.2 Conceal Callback Model

| Callback | Trigger | Scope |
|---|---|---|
| `idle_local_conceal_cb` | Every cursor move | Current line only |
| `idle_global_conceal_cb` | File open / large buffer change | Entire buffer |
| `idle_wiki_local_cb` | Cursor move | Current paragraph |
| `idle_wiki_global_cb` | File open | Entire buffer |

### 4.3 Profiling Results (2026-06-11)

Measured on `measure-cursor-v3` branch. Test: hold Down Arrow ~100 presses over 5 seconds. Metrics written via SIGUSR1 signal handler.

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
│       │   └── theme-typewriter-light.css
│       ├── icons/
│       ├── qirtas_markdown.lang         ← GtkSourceView language definition
│       └── qirtas*.style-scheme.xml     ← Editor colour schemes
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

## 8. Known Technical Debt

| Item | Status |
|---|---|
| Debug instrumentation (ITER_DEBUG, MARK_SET, etc.) | Partially removed — some log lines remain from profiling session |
| `idle_scroll_to_cursor` accumulation | Investigated — multiple callbacks can queue per cursor move; needs dedup |
| Virtual scroll residuals in `gui.c` | Some virtual-scroll functions may remain; audit needed |
| `test_*.md` files in root | Temporary profiling files, should be gitignored |
| `.bak` and `.step*` files in `src/` | Backup artifacts, should be cleaned up |
