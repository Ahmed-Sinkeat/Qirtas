# ADR-0004: Split `gui.c` into per-concern modules

- **Status:** Accepted
- **Date:** 2026-06-15

## Context

`gui.c` had grown past 5700 lines — one translation unit holding the FFI layer,
the `activate()` window builder, and almost every editor subsystem. It was hard
to navigate, and the original modularization pass (commit `9220bfd`) had left
**dead duplicate copies** of functions behind in `gui.c` that were already live
in their extracted modules.

## Decision

Each cohesive UI concern lives in its own `src/gui/gui_<concern>.c` translation
unit, compiled separately and linked. `gui.c` is reserved for its legitimate
role: the **FFI/entry layer** and the `activate()` window construction.

Mechanics (the pattern every extraction follows):

1. One concern → one `src/gui/gui_<concern>.c`, registered in `build.zig`
   (`modular_gui_files`, or the explicit list for `gui.c` / `gui_sync.c`).
2. File-private helpers stay `static`. Anything called across translation units
   is non-`static` and **declared in `src/gui_internal.h`** (UI-internal) or
   `src/gui_shared.h` (Zig FFI surface).
3. Shared types/macros/structs used by more than one module move to
   `gui_internal.h` (e.g. `AddPopoverWidgets`, `ReadModeScrollData`, paper-card
   geometry macros).
4. A `static` function in one `.c` **cannot** be called from another — that is
   the boundary check. If module B needs A's helper, A must be exported.

Extractions done under this ADR (2026-06-15): `gui_index`, `gui_sync_status`,
`gui_layout`, `gui_i18n`, `gui_rtl`, `gui_dialogs`, `gui_statusbar`. Plus removal
of ~960 lines of dead duplicates left by the original split. Net: `gui.c`
5703 → ~3230 lines. Each step was committed separately and verified with
`zig build` + `zig build test`.

## Consequences

- **Pro:** smaller, navigable units; pure-logic modules (`gui_index`, `gui_rtl`,
  `gui_i18n`) carry no GTK widget code and are the natural seams for future unit
  tests.
- **Cost:** cross-file calls need a header declaration, so moving a function has
  a small bookkeeping tax; the `static`-vs-exported decision must be made per
  symbol.
- **Rules going forward:**
  - Do not let `gui.c` reaccumulate subsystem logic — new subsystems get a module.
  - Never reintroduce a duplicate copy of a function that already lives in a
    module; if `activate()` needs it, export it from its module.
  - Keep `src/STRUCTURE.md` (tree + module table) in sync with every add/move.
- The remaining `theme/cursor-color` callbacks were intentionally **left** in
  `gui.c` because they depend on the `current_theme` file-static; extracting them
  needs that promoted to a shared global first. Tracked as future work.

## References

- `src/STRUCTURE.md` (module table, sync workflow note)
- `docs/conventions.md` (the rules in concrete form)
- Original split: commit `9220bfd`; repair: `82d78e0`
