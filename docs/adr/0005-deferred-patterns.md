# ADR-0005: Deferred patterns — Command / Plugin / Dependency Injection

- **Status:** Accepted
- **Date:** 2026-06-15

## Context

A review raised four common architecture patterns as gaps: the **Command
pattern**, a **Plugin architecture**, formal **Dependency Injection**, and
broader extensibility. Each is genuinely useful *for some future Qirtas* but
risks over-engineering the current one (single maintainer, scope still
converging). This ADR records the deliberate decision **not** to adopt them yet,
and the concrete trigger that should make us revisit each — so the absence is a
choice, not an oversight.

## Decision

Defer all four. Adopt each only when its trigger fires.

| Pattern | Status | Trigger to adopt |
|---------|--------|------------------|
| **Command pattern** | Deferred | First of: Macros, Multi-cursor, Command Palette, or scripted/plugin-driven edits. At that point, wrap edits as command objects (which already pair naturally with the Zig undo snapshots from [ADR-0003](0003-single-undo-system-gtk-disabled.md)). |
| **Plugin architecture** | Deferred | A real third-party extension use-case **and** a stable internal API. Do not build a plugin API before there are users for it. |
| **Dependency Injection (framework)** | Rejected (for now) | None planned. We already use a lightweight DI style: the `AppGui *gui` handle threaded through C, with the Zig backend owning state ([ADR-0001](0001-zig-owns-document-state.md)). Testability is addressed by extracting pure-logic modules (e.g. `gui_index`, `gui_rtl`, `gui_i18n`), not by a container. |

## Consequences

- **Pro:** no speculative abstraction layers; the codebase stays direct and
  matches its current size and team.
- **Pro:** the triggers are written down, so the next person (or a future me)
  knows exactly when and why to introduce each, instead of re-litigating it.
- **Cost:** when a trigger does fire, some existing direct-call edit paths will
  need refactoring into command objects — accepted as deferred cost.
- **Rule:** if you find yourself reaching for one of these patterns, check the
  trigger here first. If the trigger has genuinely fired, supersede this ADR with
  a new one describing the chosen design.

## References

- [ADR-0001](0001-zig-owns-document-state.md) (the de-facto DI style)
- [ADR-0003](0003-single-undo-system-gtk-disabled.md) (undo model Commands would build on)
- `docs/conventions.md`
