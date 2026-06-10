# Qirtas Project Status

**Date:** 2026-06-10

## Current Goal

Make Zig the single source of truth for document content, edits, save state, sync state, and eventually undo/redo. GTK is a viewport and interaction surface only.

## Current Plan

1. Keep document edits Zig-owned.
2. Keep `GtkTextBuffer` limited to visible viewport slice.
3. Preserve viewport reload and cursor restoration after Zig edits through one viewport request entry point.
4. Move undo/redo ownership from GTK to Zig.
5. Keep large-file support and virtual scrolling stable during migration.

## Progress So Far

1. Viewport state consolidation is in place.
2. Zig edit APIs exist:
   - `zig_insert_text(...)`
   - `zig_delete_range(...)`
   - `zig_replace_range(...)`
3. Live user edit paths were migrated to Zig-first flow in:
   - [src/gui.c](/home/sinkeat/projects/Qirtas/src/gui.c)
   - [src/gui/gui_editor.c](/home/sinkeat/projects/Qirtas/src/gui/gui_editor.c)
   - [src/gui/gui_popover.c](/home/sinkeat/projects/Qirtas/src/gui/gui_popover.c)
4. Docs were updated to reflect Zig-owned document model in:
   - [src/STRUCTURE.md](/home/sinkeat/projects/Qirtas/src/STRUCTURE.md)
   - [src/As-Built Specification Document.md](/home/sinkeat/projects/Qirtas/src/As-Built%20Specification%20Document.md)
5. `zig build` passes after the latest edit migration work.

## Open Issues

1. Undo/redo still uses GTK buffer history.
   - `Ctrl+Z` and `Ctrl+Shift+Z` still call `gtk_text_buffer_undo()` / `gtk_text_buffer_redo()`.
   - GTK history does not match canonical Zig document history after viewport reloads.
2. Runtime GUI validation was blocked in the sandbox.
   - App failed to open display, so typing, paste, IME, and viewport smoke tests could not be completed here.
3. Legacy snapshot files still contain old `gui_get_text()` usage.
   - These are not live runtime paths, but they remain in the tree.
4. One debug path still uses direct GTK delete in `src/gui.c`.
   - This is not part of normal editing flow, but it is still present.
5. Some runtime behavior remains unverified.
   - IME composition
   - rapid typing
   - large paste
   - selection restoration after reload
   - viewport-boundary edits

## Current Architecture State

- Zig owns document content and edit operations.
- GTK renders visible slice and handles input surface behavior.
- `request_viewport_position(...)` is the single viewport request entry point.
- `load_viewport_page(...)` refreshes the GTK slice from Zig-owned content.
- `loading_viewport` guards suppress signal feedback during slice reload.

## Next Step

Design Zig-owned undo/redo history. Do not depend on `GtkTextBuffer` history for document state.
