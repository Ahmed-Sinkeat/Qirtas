# ADR-0006: Group AppGui fields into per-domain sub-structs

- **Status:** Accepted
- **Date:** 2026-06-15

## Context

`AppGui` (in `src/gui_internal.h`) is the single application-context struct,
passed as `AppGui *gui` to nearly every UI function — it is effectively the
codebase's `this`/dependency-injection handle (see
[ADR-0001](0001-zig-owns-document-state.md),
[ADR-0005](0005-deferred-patterns.md)). Over time it grew to ~160 flat fields
accessed ~1200 times, which a review flagged as a "god object."

Two clarifications shaped the decision:

- It holds **pure data, no behaviour** — the worst form of the god-object
  anti-pattern (a class accreting methods) does not apply.
- True encapsulation (opaque per-module structs behind accessors) would be a
  large rewrite for little gain, because essentially every module legitimately
  needs the shared context.

## Decision

Keep one `AppGui` context, but **organize its fields into nested, named
sub-structs grouped by the module that owns them.** A field cluster that is
driven mostly by one module becomes `gui-><domain>.<field>`.

- Field names drop the now-redundant domain prefix
  (`cursor_target_x` → `cursor.target_x`, `enable_cursor_trail` →
  `cursor.enable_trail`).
- Broadly-shared shell widgets (window, sidebar, top-level buttons, stack) stay
  top-level — grouping them would gain nothing.
- This is **organization, not enforcement**: C does not prevent any code from
  reaching `gui->cursor.x`. The win is a smaller top-level namespace, explicit
  ownership, and a navigable struct — not access control.

Rollout is **staged**, one domain per commit, each verified with `zig build` +
`zig build test`. First domain: `cursor` (trail animation + caret/pointer
colour, owned by `gui_cursor.c`).

## Consequences

- **Pro:** the struct documents which module owns which state; new fields have an
  obvious home; the top-level field count drops with each domain moved.
- **Cost:** each domain move is a mechanical rename of every `gui->field` access
  for that domain (the compiler flags any miss as "no member named …").
- **Rule:** new state goes into the sub-struct of its owning domain, not the top
  level, unless it is genuinely shell-wide.
- Planned domains (move as they prove worthwhile): `cursor` (done), `tabs`,
  `sync`, editor `prefs`, `layout`/card. No ADR supersession needed per domain —
  this ADR covers the convention.

## References

- [ADR-0004](0004-gui-modularization.md) (module split this complements)
- `src/gui_internal.h` (the `AppGui` definition)
- `docs/conventions.md`
