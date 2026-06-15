# ADR-0001: Zig backend owns document state

- **Status:** Accepted
- **Date:** 2026-06-12

## Context

Qirtas is a GTK4/libadwaita editor with a Zig backend. Two layers can plausibly
own the "truth" of a document: the GTK `GtkTextBuffer` (UI side, in `gui.c` and
the `gui_*.c` modules) or the Zig backend (`main.zig`).

Letting the GTK buffer be the source of truth pulls file I/O, undo history,
autosave, file watching, and crypto into the UI layer — the hardest layer to
test and the one most exposed to GTK's own lifecycle quirks (idle callbacks,
buffer swaps, Pango layout). It also means the editing model and the storage
model are entangled.

## Decision

The **Zig backend owns the full document text** and all state derived from it:

- the canonical document text (`zig_get_document_text()`)
- the undo/redo stack (see [ADR-0003](0003-single-undo-system-gtk-disabled.md))
- the autosave timer
- `inotify` file watches
- the crypto vault and sync tokens

The GTK side is a **view + input layer**. Edits flow GTK signal → Zig mutator
(`zig_insert_text`, `zig_delete_range`, …). The C side never treats the
`GtkTextBuffer` as authoritative storage; it mirrors what Zig owns.

The C↔Zig boundary is the FFI surface: `gui_*` functions exported from C and
called from Zig (`main.zig` / `sync.zig`), and `zig_*` functions exported from
Zig and called from C.

## Consequences

- **Pro:** storage logic is testable in Zig without a display; the UI can be
  rebuilt/reswapped without losing document state; one place to reason about
  memory and persistence.
- **Pro:** this is also the project's de-facto Dependency Injection style — the
  shared `AppGui *gui` handle is threaded through C functions, and Zig holds the
  backend state. No DI framework is needed (see
  [ADR-0005](0005-deferred-patterns.md)).
- **Cost:** every document mutation must round-trip through FFI; pure-C
  convenience edits on the buffer are forbidden because they would desync Zig.
- **Rule:** new document operations add a `zig_*` mutator and call it; they do
  **not** mutate the `GtkTextBuffer` directly as the system of record.

## References

- `src/As-Built Specification Document.md` §1 (File I/O Ownership), §3.1
- `src/main.zig` (FFI exports), `src/gui_shared.h` (Zig-facing decls)
- `src/STRUCTURE.md` — "Full-buffer editing model"
