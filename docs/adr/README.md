# Architecture Decision Records (ADR)

This folder records the **big, hard-to-reverse decisions** behind Qirtas and—
more importantly—*why* they were made. When a choice looks surprising later
("why is GTK's own undo turned off?", "why isn't there virtual scrolling?"),
the answer should live here instead of in someone's memory or a commit message.

## What goes here

Write an ADR when a decision is:

- **Hard to reverse** (changes the data-ownership boundary, the editing model,
  the module layout), or
- **Counter-intuitive** (we deliberately did the opposite of the obvious thing),
  or
- **A standing constraint** future code must respect.

Do *not* write an ADR for routine bug fixes, refactors, or small feature work —
those belong in commit messages and `src/STRUCTURE.md`.

## Format

Each record is one file: `NNNN-short-title.md`, numbered in order. Keep it short.
Use this skeleton:

```markdown
# ADR-NNNN: Title

- **Status:** Proposed | Accepted | Superseded by ADR-XXXX
- **Date:** YYYY-MM-DD

## Context
What forced a decision. The problem, the constraints, the evidence.

## Decision
What we chose, stated plainly.

## Consequences
What this buys us, what it costs, and the rules future code must follow.

## References
Files, commits, evidence docs.
```

A decision is never deleted. If it changes, add a new ADR and mark the old one
`Superseded by ADR-XXXX`.

## Index

| ADR | Title | Status |
|-----|-------|--------|
| [0001](0001-zig-owns-document-state.md) | Zig backend owns document state | Accepted |
| [0002](0002-full-buffer-over-virtual-scroll.md) | Full-buffer editing model (virtual scroll removed) | Accepted |
| [0003](0003-single-undo-system-gtk-disabled.md) | Single undo system — GTK undo disabled | Accepted |
| [0004](0004-gui-modularization.md) | Split `gui.c` into per-concern modules | Accepted |
| [0005](0005-deferred-patterns.md) | Deferred patterns: Command / Plugin / DI | Accepted |
