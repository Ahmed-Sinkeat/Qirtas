# As-Built Specification Document
## Project: Qirtas — MVP Release



## 1. Introduction

### 1.1 Document Purpose
This document provides a comprehensive and precise engineering documentation of the current architecture of the Qirtas application after compiling and assembling its first stable Minimum Viable Product (MVP). Unlike traditional requirement documents written prior to development, this specification acts as an accurate technical reference based on the actual, working codebase to guide future maintenance, style polishing, and feature development.

### 1.2 Product Philosophy & Target Audience
Qirtas is designed to be a lightweight, distraction-free desktop markdown editor tailored for writers, manuscript editors, and researchers. It strictly targets non-technical users, and its design philosophy adheres to the following principles:
- **High Performance & Minimal Footprint:** RAM usage is optimized to stay under 30MB, and volatile stack memory is zeroed immediately to ensure data integrity and security.
- **Simplicity & Ease of Use:** Eliminates all complex technical configurations (such as manual JSON editing or direct API key entry) and replaces them with intuitive 1-click graphical interfaces.

### 1.3 Technical Scope & Hybrid Architecture
Qirtas utilizes a hybrid architecture combining two programming languages to achieve the optimal balance between performance and native GUI layout:
- **Backend (Zig):** The application core, file input/output (I/O) management, autosaving, encryption, and local data synchronization are implemented in Zig (`src/main.zig` and `src/sync.zig`).
- **Frontend (C GTK4/Adwaita):** The graphical user interface is natively implemented in C utilizing GTK4 and Libadwaita (`src/gui.c` and the modular files under `src/gui/`), ensuring smooth desktop integration, theme compatibility (e.g., ADW style manager), and hardware-accelerated rendering.
- **FFI Bridge:** Both layers communicate seamlessly through a bidirectional Foreign Function Interface (FFI) without any middle layer affecting processing speed.



## 2. Technology Stack & Justifications

The technical architecture of Qirtas is designed to integrate low-level systems for performance and native layout capabilities. Architecture decisions were made to guarantee that memory consumption remains under the 30MB RAM budget.

### 2.1 Backend: Zig (`src/main.zig` & `src/sync.zig`)
Zig is the primary engine for file management, background routines, and data logic.
- **Manual Memory Management:** Allows precise allocation and deallocation of memory. Volatile stack buffers containing sensitive text are zeroed immediately after processing to prevent memory leakages and safeguard data.
- **Seamless C Interoperability:** Zig provides native support for C headers and libraries, allowing highly efficient bidirectional FFI calls with GTK4/Adwaita without wrapper overhead.
- **Independent Build Tooling:** Standard `build.zig` and `build.zig.zon` manage compilation of both Zig and C sources into a single optimized executable, eliminating the need for CMake or Make.

### 2.2 Frontend: C GTK4 & Libadwaita (`src/gui.c` & `src/gui/gui_sync.c`)
The graphical layout, windows, and editor controls are natively implemented using GTK4 and Libadwaita.
- **Native UI Performance:** Renders hardware-accelerated layouts directly, avoiding heavy web-based frameworks (like Electron) that consume hundreds of megabytes of RAM.
- **Advanced GtkSourceView Engine:** Leverages `GtkSourceView` for text editing features, syntax highlighting, and custom editor coloring schemes defined via standard XML (`src/ui/qirtas_markdown.lang`).
- **Adwaita Integration:** Uses the Adwaita Style Manager to enforce light/dark theme scheduling globally.

### 2.3 Local Cryptography: ChaCha20Poly1305
ChaCha20Poly1305 symmetric encryption is integrated into the Zig backend to protect user files offline and online.
- **High-Throughput Performance:** Designed to be highly efficient on standard CPU architectures (like typical laptops) without requiring specialized AES hardware acceleration, encrypting large documents in fractions of a second.
- **Master Key Architecture:** Files are encrypted with a random 32-byte Master Key. The Master Key is stored separately in `system_keys` and is normally unlocked silently with a `machine-id`-derived key. Recovery uses a 24-word BIP-39 mnemonic, with an optional passphrase layer reserved for advanced recovery.

### 2.4 Cloud Sync: On-Demand Event-Driven Sync
Qirtas uses an event-driven sync routine to communicate with cloud platforms (e.g., Dropbox, Google Drive, and GitHub Device Flow).
- **Avoiding Live Sync Overhead:** Constant character-by-character sync is purposely avoided to prevent network congestion, high RAM usage, and complex conflict resolution.
- **On-Demand Ticking:** Encrypted documents are uploaded in only three events: clicking "Save," closing the application, or clicking "Sync Now."
- **Automatic Version Control:** By uploading state updates, users benefit from cloud platforms' built-in version control and commit logs to restore historical document states if necessary.



## 3. Text Area & Typing Experience

The core editing area uses GtkSourceView embedded in the GTK4 window. This section documents the design solutions implemented to maintain a smooth typing experience.

### 3.1 Word Wrap & Virtual Scroll Solution
- **The Problem:** The editor previously computed vertical viewport offsets using a simple linear calculation: `Offset = Line Index * Fixed Line Height`. However, when word wrapping was enabled (`GTK_WRAP_WORD_CHAR`), wrapped lines expanded logical lines into multiple visual rows, leading to broken height calculations and cursor viewport jumps.
- **The Solution:** Switched to runtime layout coordinate mapping. The layout gets the real pixel position of the cursor using the `GtkTextIter` of the text buffer and calling:
  `gtk_text_view_get_iter_location(GTK_TEXT_VIEW(source_view), &iter, &rect);`
  The real vertical coordinate `rect.y` is used to adjust the viewport's `GtkAdjustment` value, allowing a stable word wrap layout with smooth scrolling.

### 3.2 Deferred Scroll & Immutable Offsets
- **The Problem:** Deferred scroll callbacks in the operation queue stored live pointers to the text cursor mark (`insert_mark`). Since the mark changes immediately when the user clicks or toggles menus, old queued scroll requests caused the viewport to jump unexpectedly.
- **The Solution:** The data structure `ScrollToCursorData` was refactored to store a fixed, immutable character offset (index) rather than a dynamic pointer. In the idle callback, the offset is clamped against the current buffer size before executing the scroll, eliminating layout jumps.

### 3.3 Text View Styling & Configurations
- **Smart Tabs:** Tab spacing is hardcoded to 4 spaces.
- **Auto-Indent:** Smart auto-indentation is enabled programmatically to automatically align new lines with the preceding paragraph indentation.
- **Margins & Focus Mode:** Spacing and text area margins are configured cleanly in `base.css` and isolated from cursor mouse events to preserve a clean drafting layout.



## 4. Repository Layout & Module Responsibilities

The layout of the project separates backend system routines from the graphical frontend:

| File / Directory | Language | Engineering Responsibility |
|---|---|---|
| `src/main.zig` | Zig | Executable entry point. Manages application state, file system watch (inotify), autosaving timer, and system hooks. |
| `src/bip39.zig` | Zig | BIP-39 mnemonic encode/decode helpers used for recovery phrase generation and validation. |
| `src/sync.zig` | Zig | Core logic for ChaCha20Poly1305 encryption, decryption, and cloud API integration. |
| `src/root.zig` | Zig | Zig module configuration package. |
| `src/gui.c` | C | UI engine; initializes and wires GTK4/Adwaita layout components. |
| `src/gui_internal.h` | C | Internal UI state, shared window pointers, and module hooks for the C frontend. |
| `src/gui_shared.h` | C | Zig-facing C FFI declarations and cross-language hooks. |
| `src/gui/gui_theme.c` | C | Theme loading, CSS variables/styles injection, and custom font management. |
| `src/gui/gui_cursor.c` | C | Smear pointer/cursor trail animations and trail persistence. |
| `src/gui/gui_hr.c` | C | Detection and custom formatting/rendering of horizontal rules. |
| `src/gui/gui_search.c` | C | Search overlay control and query matching inside active buffers. |
| `src/gui/gui_conceal.c` | C | Markdown concealment tags and custom header scaling. |
| `src/gui/gui_wiki.c` | C | Wiki-link parsing, navigation triggers, and document opening. |
| `src/gui/gui_popover.c` | C | Rich text formatting floating popover. |
| `src/gui/gui_explorer.c` | C | Directory hierarchy explorer drawer and vault management UI. |
| `src/gui/gui_tabs.c` | C | Tab control bar, document tabs lifecycle, and buffer cache mapping. |
| `src/gui/gui_editor.c` | C | Primary text editing area setup, event filters, word wrap coordinate mapping. |
| `src/gui/gui_sync.c` | C | Cloud credentials connection views, synchronization status bar widgets. |
| `build.zig` | Zig | Build script compiling Zig modules and C sources into a single executable. |



## 5. Bidirectional FFI Bridge Architecture

High-performance communication between the backend and frontend is achieved via a memory-mapped C-linkage Foreign Function Interface (FFI).

### 5.1 C Functions Called from Zig
- `gui_get_text(void)`: Fetches current text from the active GtkTextBuffer.
- `gui_free_text(char*)`: Safely frees string memory allocated in C.
- `gui_set_text(char*, int)`: Sets editor buffer text, blocks modification signals temporarily, and resets cursor.
- `gui_set_title(char*)`: Updates the window title and selects/adds the corresponding tab.
- `gui_set_sync_status(char*)`: Updates the status bar pill (e.g. "Synced", "Saving...").
- `gui_show_editor(void)`: Switches view stack to show the main editor workspace.
- `gui_show_recovery_dialog(void)`: Opens the recovery modal when the backend cannot unlock the master key.
- `gui_get_cursor_position(int*, int*)`: Gets current cursor line and offset.
- `gui_set_cursor_position(int, int)`: Restores editor cursor position.
- `gui_refresh_explorer(void)`: Requests directory tree explorer update.
- `gui_set_virtual_scroll_mode(int, int)`: Configures virtual scrolling mode.
- `gui_init_virtual_document(int, int, int)`: Wires virtual document scrolling.
- `gui_trigger_autosave(void)`: Triggers Zig autosaving routines.
- `gui_update_sync_status(int, const char*)`: Updates Google Drive sync status.
- `gui_update_dropbox_status(int, const char*)`: Updates Dropbox sync status.
- `gui_update_github_status(int, const char*)`: Updates GitHub sync status.
- `gui_update_local_sync_status(int, const char*)`: Updates local sync status.
- `gui_tabs_save_active_to_cache(void)`: Saves current active tab text/modified state to cache.
- `gui_tabs_restore_active_from_cache(void)`: Restores active tab text/modified state from cache.

### 5.2 Zig Functions Called from C
- `zig_on_gui_ready(void)`: Invoked when C UI construction is complete.
- `zig_has_active_master_key(void)`: Reports whether the backend has unlocked the master key.
- `zig_open_file(const char *filename)`: Opens a file, updates watch lists, and loads content.
- `zig_open_vault(const char *dir_path)`: Toggles active project directories.
- `zig_search_workspace(const char *query)`: Queries local files for matches.
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

### 5.3 Memory Safety Guidelines
Strings are passed using standard `const char*` C pointers. Zig reads these pointers, converts them to slices, and immediately zeroes any volatile stack memory after processing to keep the memory footprint optimized and secure.

---

## 6. Theme & Customization System

The user interface uses a two-layer CSS styling system coupled with Libadwaita for runtime customization.

### 6.1 Two-Layer Architecture
- **Structural Styles (`src/ui/themes/base.css`):** Controls layouts, margins, margins padding, tab structure, close buttons, and structural spacing.
- **Color Variables (`src/ui/themes/theme-*.css`):** Defines CSS custom variables for themes (Sepia, Midnight, Slate Dark, Things, etc.) controlling colors and text highlight rules.

### 6.2 Unsaved Changes Tab Indicator
Tabs monitor edits using the buffer's `"modified-changed"` signal:
- If a tab has unsaved changes, a small circular dot (`.tab-dirty-dot`) is rendered next to the tab name.
- The indicator utilizes the CSS custom variable `--dirty-dot-color`.
- In dark themes, the variable defaults to `#ffffff` (white circle). In light themes (Sepia, Modern Ink, Typewriter Light), it is overridden to `#000000` (black circle).
- Saving the file resets the buffer's modified state, automatically hiding the dot.

---

## 7. Data Persistence & Security

### 7.1 Offline Security
- Document blocks are encrypted using ChaCha20Poly1305.
- Document encryption uses a random Master Key, not `/etc/machine-id` directly.
- The Master Key is stored in `system_keys` and unlocked by a `machine-id`-derived key during normal startup.
- Recovery is possible with the 24-word mnemonic, and the optional passphrase layer is reserved for advanced users.
- Memory stack buffers are actively zeroed to guarantee memory security under the 30MB RAM constraint.

### 7.2 On-Demand Synchronization
- Sync runs on specific events (Save, Close, Manual trigger) to optimizeThinkPad battery usage and prevent networking conflicts.
- Host-level commit lists act as version control history, allowing users to restore previous versions easily.
