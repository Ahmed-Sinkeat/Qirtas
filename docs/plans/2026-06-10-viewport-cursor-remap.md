# Viewport Cursor Remap

## Issue

Viewport reload used to preserve preferred-x, then cursor drifted to stale local rows after slice swaps. Current work added transient absolute cursor snapshot + remap inside `load_viewport_page()` and instrumentation in `request_viewport_position()`.

## Progress

- Preferred-x reset removed from viewport reload path.
- Cursor preservation restored during viewport reload with local-only temp state.
- `request_viewport_position()` now logs requested line, current cursor, scroll state, and chosen viewport window.

## Open Check

- Verify logs show correct `saved_abs_line` and `resulting_abs_line`.
- Verify viewport selection at `667` is driven by current abs cursor or scroll target, not stale local row.
- Verify no reload loop returns.
