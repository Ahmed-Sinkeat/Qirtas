# Qirtas — Documentation

Developer and design docs. New here? Start with **STRUCTURE.md**, then the
**As-Built Specification**. The product overview lives in the
[root README](../README.md) (Arabic) / [README.en.md](../README.en.md) (English).

## Start here

| Doc | What's in it |
|---|---|
| [STRUCTURE.md](STRUCTURE.md) | Repository layout — where to edit what, and the invariants to keep |
| [As-Built Specification Document.md](As-Built%20Specification%20Document.md) | Architecture decisions, crash post-mortems, the Zig↔C FFI bridge, testing status |
| [conventions.md](conventions.md) | Coding conventions |
| [LAYOUT.md](LAYOUT.md) | UI layout map — the widget tree and what sits where on screen |

## Subsystems

| Doc | What's in it |
|---|---|
| [SYNC.md](SYNC.md) | Sync setup per provider, architecture, and a troubleshooting matrix (every error → cause → fix); per-backend conflict behavior |
| [SECURITY.md](SECURITY.md) | Honest crypto threat model and the roadmap to a real one |
| [PORTABILITY.md](PORTABILITY.md) | Plan to grow the Zig core (parse in Zig, render per-platform) for Windows + Android/Kotlin |

## Quality

| Doc | What's in it |
|---|---|
| [ISSUES.md](ISSUES.md) | Known issues from code audit + the running fix log |
| [SMOKE-CHECKLIST.md](SMOKE-CHECKLIST.md) | Manual smoke test to run before a release |

## Architecture Decision Records

Why the big structural choices were made.

- [0001 — Zig owns document state](adr/0001-zig-owns-document-state.md)
- [0002 — Full buffer over virtual scroll](adr/0002-full-buffer-over-virtual-scroll.md)
- [0003 — Single undo system (GTK undo disabled)](adr/0003-single-undo-system-gtk-disabled.md)
- [0004 — GUI modularization](adr/0004-gui-modularization.md)
- [0005 — Deferred patterns](adr/0005-deferred-patterns.md)
- [0006 — AppGui substructs](adr/0006-appgui-substructs.md)

See [adr/README.md](adr/README.md) for the ADR process.

## Plans

Dated design and work notes (point-in-time; not always current).

- [2026-06-08 — Recovery layers](plans/2026-06-08-recovery-layers.md)
- [2026-06-10 — Project status](plans/2026-06-10-project-status.md)
- [2026-06-10 — Undo large-file fix](plans/2026-06-10-undo-large-file-fix.md)
- [2026-06-10 — Viewport cursor remap](plans/2026-06-10-viewport-cursor-remap.md)
