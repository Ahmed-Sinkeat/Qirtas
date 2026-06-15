# Qirtas Conventions

Short guide to how this codebase is named and organized, so new code looks like
existing code and contributors know when to create a file or a module. For *why*
the big decisions were made, see [`docs/adr/`](adr/). For *what lives where*, see
[`src/STRUCTURE.md`](../src/STRUCTURE.md).

---

## Layers

- **Zig backend** (`src/main.zig`, `sync.zig`, …) owns document state, file I/O,
  undo, autosave, crypto. See [ADR-0001](adr/0001-zig-owns-document-state.md).
- **C/GTK UI** (`src/gui.c`, `src/gui/gui_*.c`) is the view + input layer.
- **The boundary is FFI:** `gui_*` functions exported from C and called by Zig;
  `zig_*` functions exported from Zig and called by C.

---

## Naming

| Thing | Convention | Examples |
|-------|-----------|----------|
| Source file | `gui_<concern>.c`, snake_case, in `src/gui/` | `gui_layout.c`, `gui_dialogs.c` |
| Function | snake_case | `apply_editor_prefs`, `tree_build_dir` |
| FFI: C called from Zig | `gui_*` | `gui_get_text`, `gui_set_title` |
| FFI: Zig called from C | `zig_*` | `zig_insert_text`, `zig_undo` |
| UI signal callback | `on_<subject>_<event>` | `on_open_dialog_response`, `on_status_menu_quit` |
| "Apply state to widgets" helper | `apply_*` | `apply_focus_mode`, `apply_compact_mode` |
| Widget builder | `*_build_*` / `build_*` / `*_item` | `tree_build_dir_row`, `status_menu_item` |
| Macro / constant | `QIRTAS_*`, uppercase | `QIRTAS_DESK_GAP_MIN`, `QIRTAS_CARD_CHROME` |
| `typedef struct` | PascalCase | `AppGui`, `AddPopoverWidgets`, `PrintTheme` |
| Shared widget globals | `global_*` (use sparingly) | `global_gui`, `global_source_view` |

Arabic UI strings are written inline as UTF-8 literals; user-facing English
strings go through `qirtas_tr()` (see `gui_i18n.c`) so they can switch to Arabic.

---

## When to create a new file / module

Create `src/gui/gui_<concern>.c` when a **cohesive concern** grows beyond a
handful of functions — a subsystem you'd describe in one phrase (indexing,
RTL direction, dialogs, status bar). Prefer pulling **pure logic** (no GTK
widget construction) into its own module; those become the unit-test seams.

Keep in `gui.c` only its two legitimate roles:

1. the **FFI / entry layer** (`run_gui`, `gui_get_text`, `gui_run_on_main_thread`, …), and
2. `activate()` — the top-level window construction.

Do **not** let `gui.c` reaccumulate subsystem logic, and never reintroduce a
duplicate of a function that already lives in a module. See
[ADR-0004](adr/0004-gui-modularization.md).

---

## Linkage rules (the boundary check)

- Default to `static`. A file-private function or variable stays `static`.
- A symbol used in **another** translation unit must be non-`static` **and**
  declared in a header:
  - `src/gui_internal.h` — UI-internal cross-module declarations + shared
    structs/macros (`AppGui`, `AddPopoverWidgets`, `QIRTAS_*`).
  - `src/gui_shared.h` — the Zig-facing FFI surface only.
- A `static` function in one `.c` **cannot** be called from another. If a module
  needs it, export it (un-`static` + header decl). This is the test for whether a
  cut is clean.
- Shared types/macros used by 2+ modules belong in `gui_internal.h`, not copied
  into each `.c`.

---

## Adding / moving a module — checklist

1. Create / edit `src/gui/gui_<concern>.c` (includes mirror siblings:
   `<gtk/gtk.h>`, plus `adwaita.h` / `gtksourceview` / `sqlite3.h` as needed,
   then `"gui_internal.h"`).
2. Register it in **`build.zig`** (`modular_gui_files`).
3. Move cross-file decls into `gui_internal.h` / `gui_shared.h`; keep helpers `static`.
4. Build clean: `zig build` (exit 0) **and** `zig build test`.
5. Update **`src/STRUCTURE.md`** (repo tree **and** module table).
6. One logical change per commit.

---

## Code style

- C, 4-space indent, K&R braces (`{` on the same line).
- Guard clauses first: `if (!gui || !gui->source_view) return;`.
- Null-check globals and widgets before use (many run before `activate()` builds them).
- Hooks / callbacks / filesystem paths that can legitimately fail should
  **silent-fail**, never crash the UI.
- Keep behavior-preserving refactors and behavior changes in separate commits.

---

## Commits

Conventional-style prefixes already used in history: `feat:`, `fix:`,
`refactor:`, `docs:`, `perf:`, `chore:`. Optional issue scope: `fix(#16): …`.
Subject concise (~50 chars); body explains *why* when not obvious. Sign off:

```
Co-Authored-By: <name> <email>
```

---

## Documentation map

- **`docs/adr/`** — big, hard-to-reverse decisions and their rationale.
- **`src/STRUCTURE.md`** — current file/module map (the "what lives where").
- **`src/As-Built Specification Document.md`** — engineering spec + profiling.
- **`docs/conventions.md`** — this file (the "how to add to it").

When code structure changes, update `STRUCTURE.md`. When a *decision* changes,
add an ADR (don't edit old ones — supersede them).
