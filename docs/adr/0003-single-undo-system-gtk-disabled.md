# ADR-0003: Single undo system — GTK undo disabled

- **Status:** Accepted
- **Date:** 2026-06-13

## Context

GTK4's `GtkTextBuffer` ships with built-in, delta-based undo, enabled by default.
Qirtas also has its own undo: full-document heap snapshots owned by the Zig
backend (see [ADR-0001](0001-zig-owns-document-state.md)).

Running both at once means **two parallel histories**. They double memory cost
and, worse, make Ctrl+Z behaviour depend on which system catches the keystroke
first — and GTK's delta undo can desync from Zig's snapshot model after
programmatic edits (conceal, formatting, file reloads).

## Decision

There is **exactly one undo system: the Zig snapshot stack.**

- GTK's built-in undo is disabled immediately after the buffer is created:
  `gtk_text_buffer_set_enable_undo(buf, FALSE)` in `gui.c`. Do not re-enable it.
- `captureUndoEntry` (Zig) snapshots full-document state on commit boundaries;
  100 entries max, 64 MB total per stack, oldest evicted first.
- Edits are committed at **word-grain** boundaries (typing pause, whitespace,
  punctuation, deletions); `zig_undo()` seals pending edits before undoing.
- The editor key handler intercepts Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y, routes them
  to `zig_undo()` / `zig_redo()`, and returns `TRUE` so GTK's (still-bound but
  no-op) `text.undo` action never fires.
- Undo / full reload restores the viewport **by line** (reflow-stable), not by
  raw offset.

## Consequences

- **Pro:** one predictable history; no focus-dependent Ctrl+Z; no double memory.
- **Cost:** snapshots are whole-document, so the 64 MB cap + eviction matter for
  very large files; word-grain sealing logic must be maintained.
- **Standing rule:** any **new editable `GtkTextView` that shows document
  content** must disable its GTK undo and route through the Zig stack. Throwaway
  entry widgets in dialogs may keep GTK's default undo — they don't touch the
  document.

## References

- `src/As-Built Specification Document.md` §4a (Undo Architecture)
- Commit `0df35cf` "Document single-undo-system invariant (GTK undo disabled by design)"
- `src/gui/gui_buffer.c` (word-grain undo, commit boundaries), `src/main.zig`
  (`captureUndoEntry`, `zig_undo`/`zig_redo`)
