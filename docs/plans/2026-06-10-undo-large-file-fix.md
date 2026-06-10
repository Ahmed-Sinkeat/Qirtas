# Undo and Large File Crash Fix Plan

**Date:** 2026-06-10

## Goal

Fix large-file undo storm and navigation crash. Undo snapshots must be debounced, and mmap-backed document bytes must never be read after remap.

## Reality Check

1. Undo snapshots were pushed from GTK `changed` signal on every internal buffer mutation.
2. `captureUndoEntry()` used raw mmap slice directly, which is unsafe once remap can occur during reload.
3. `gui_reload_viewport()` lacked a reentry guard.

## Done

1. `zig_undo_push()` now only marks pending state.
2. `zig_undo_commit()` now captures and pushes snapshot once per gesture.
3. `captureUndoEntry()` now copies bytes out of mmap before any remap can invalidate source pointer.
4. `gui_reload_viewport()` now bails if viewport is already loading.

## Follow-up

1. Wire `zig_undo_commit()` at every edit gesture boundary.
2. Validate large delete, paste, and undo/redo paths with a real UI run.
3. Remove any remaining stale snapshot calls if they exist outside normal edit flow.
