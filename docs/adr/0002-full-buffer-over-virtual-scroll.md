# ADR-0002: Full-buffer editing model (virtual scroll removed)

- **Status:** Accepted
- **Date:** 2026-06-12

## Context

Large files (e.g. `main.zig`, ~1700 lines) need to open and scroll smoothly. The
obvious scalability move is **virtual scrolling**: load only a window of ~300
lines into the `GtkTextBuffer` and remap positions as the user scrolls.

That prototype was built (`feat: modularize GUI and implement jitter-free
3-phase virtual scrolling`, commit `9220bfd`; tagged `viewport-prototype`). It
required:

- buffer-generation guards on every deferred `GtkTextIter` callback,
- spacer widgets to fake full document height,
- complex position remapping between viewport-local and document-absolute
  offsets.

The remapping was a crash source. Cursor goal-column corruption and
buffer-generation races were traced through the viewport path (see
`cursor_corruption_evidence.md`). Meanwhile we found GTK already validates Pango
layout **lazily** when the `GtkSourceView` is the *direct* scrollable child of
the `GtkScrolledWindow` — so a full-document buffer opens fast anyway, as long as
the view is not re-wrapped in a sizing box.

## Decision

Use the **full-buffer model**: the entire document lives in the `GtkTextBuffer`,
and `GtkSourceView` is the direct `GtkScrollable` child so GTK lazily lays out
only visible lines. Virtual scrolling / paging was **fully removed** (commits
`78e92c5` "replace all viewport refresh with full-buffer refresh", `82d78e0`
"drop paging"). The prototype survives only as the `viewport-prototype` git tag.

## Consequences

- **Pro:** native GTK scroll-to-cursor works; no position remapping, no spacer
  widgets, far fewer crash surfaces. Big files open instantly.
- **Cost:** whole-document passes (conceal, word/char count) are O(document), so
  they are **debounced** (~220 ms typing-pause, `buffer_stats_timeout_cb`) and
  scoped to dirty lines where possible; local conceal around the cursor stays
  instant via `on_mark_set`.
- **Hard rule:** **do not re-wrap the `GtkSourceView` in a `GtkBox`.** That
  allocates it at full document height and forces whole-document layout on open
  (multi-second freeze, choppy pointer).
- **Safety net:** the buffer-generation guard pattern is kept even in full-buffer
  mode — deferred idle callbacks snapshot `buffer_generation` and self-discard if
  the buffer was swapped.
- If virtual scrolling is ever reconsidered, it must solve the goal-column /
  generation races documented in `cursor_corruption_evidence.md` first.

## References

- `src/As-Built Specification Document.md` §3.1, §3.2, §3.3
- `cursor_corruption_evidence.md`
- Commits: `9220bfd` (prototype), `78e92c5`, `82d78e0`, `c6b425a`
- Git tag: `viewport-prototype`
