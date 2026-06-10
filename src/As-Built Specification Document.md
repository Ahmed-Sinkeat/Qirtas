# As-Built Specification — Qirtas v0.9.1

## Architecture Invariants

1. Zig owns canonical document content and all edit mutations.
2. GTK renders only the visible viewport slice plus interaction surfaces.
3. `request_viewport_position(gui, abs_line)` is the **single entry point** for all viewport requests (scroll, cursor, open-file). Nothing else initiates a page reload.
4. `load_viewport_page(gui, start, end)` performs raw slice retrieval and viewport range update — no policy decisions inside.
5. `loading_viewport` blocks re-entrant refresh while slice data is changing.
6. `gui_set_cursor_position(line, col)` routes through `request_viewport_position` and clamps `col` to `gtk_text_iter_get_bytes_in_line()` before setting the iterator — preventing `Byte index N is off the end of the line` aborts.
7. `gui_set_text()` increments `buffer_generation` before touching the `GtkTextBuffer`. Every deferred idle callback checks this counter and self-cancels if it has changed.

---

## Subsystem: Virtual Viewport (v0.9.1 stable)

### What Was Fixed in v0.9.0 → v0.9.1

#### Bug: Stale `GtkTextIter` → `Byte index N is off the end of the line` crash

**Root cause:** Modules (`gui_conceal.c`, `gui_wiki.c`, `gui_popover.c`) scheduled `g_idle_add` callbacks that captured a `GtkTextBuffer` pointer. Between schedule and execution, `load_viewport_page` could replace the buffer's text entirely via `gui_set_text`. The old iters then pointed into invalidated content — GTK's Pango renderer aborted.

**Fix — `buffer_generation` counter:**
- `gui_internal.h`: Added `guint buffer_generation` to `AppGui`.
- `gui.c / gui_set_text`: Increments `buffer_generation` before every buffer text replacement.
- All deferred idle callbacks (`ConcealData`, `WikiData`, `ScrollToCursorData`) now store the generation at schedule time. If `d->generation != d->gui->buffer_generation` when the callback fires, it frees itself and returns `G_SOURCE_REMOVE` silently.

Affected call sites:
- `gui_conceal.c` — `idle_global_conceal_cb`, `idle_local_conceal_cb`, `idle_scroll_to_cursor`
- `gui_wiki.c` — `idle_wiki_global_cb`, `idle_wiki_local_cb`
- `gui_popover.c` — `idle_scroll_to_cursor`

#### Bug: Scroll jumps backward one page (2191 → 2075 regression)

**Root cause:** When the bottom spacer collapsed to 0 height at end-of-file, GTK's layout engine re-fired `on_scroll_changed` with a slightly different `vadj` value. The 60ms debounce had already committed to line 2191, but the spacer-triggered re-fire computed a different start (2075), passing the `last_loaded_start != new_start` guard and triggering a redundant backward load.

**Fix — direction reversal lock in `fire_scroll`:**
- Added `last_load_direction` (+1 / -1) and `last_load_time_ms` static locals.
- If an incoming scroll request contradicts the direction of the immediately preceding load AND less than 200ms have passed, the request is dropped.
- This prevents the spacer-collapse feedback loop from triggering useless reverse page fetches.

#### Bug: `Segmentation fault at 0x100000018` — corrupted pointer crash

**Root cause:** Spacer widgets were being set to 0px height, causing GTK to schedule a layout invalidation. This invalidation could fire while a viewport load was in progress, resulting in a GObject method call on a partially-freed widget handle (`0x100000018` = small integer + garbage high bits).

**Fix — 1px minimum spacer:**
- `get_spacer_heights()` enforces a floor of 1px for both top and bottom spacers.
- Spacers are never destroyed and recreated — only resized.

#### Bug: Cursor column crash after page swap

**Root cause:** Cursor restore used a character-offset `col` saved against the old buffer page. After a page swap, the new page's corresponding line could be shorter in bytes, causing `gtk_text_buffer_get_iter_at_line_offset` to abort.

**Fix — byte-clamped cursor restore:**
- `gui_set_cursor_position` now calls `gtk_text_buffer_get_iter_at_line`, then reads `gtk_text_iter_get_bytes_in_line(&iter)` and clamps `safe_col` to that value before advancing.

#### Optimization: Adaptive page size

- `get_page_size(total_lines)` returns 400 / 300 / 250 lines depending on document scale.
- Reduces GTK buffer churn on very large files without slowing small documents.

#### Optimization: 60ms scroll debounce

- `on_scroll_changed` queues a `g_timeout_add(60, fire_scroll, gui)` instead of loading immediately.
- `queued_line` always holds the most recent target — intermediate positions during fast scrolls are discarded.

---

## Scroll Signal Path

```
User scrolls / cursor moves
        │
        ▼
on_scroll_changed(vadj)
  │  [if in_scroll_update: skip]
  │  [if loading_viewport: skip]
  ▼
queued_line = computed_target_line
  ▼
g_timeout_add(60ms) → fire_scroll(gui)
  │  [if direction reversal within 200ms: drop]
  │  [if new_start == last_loaded_start: skip]
  ▼
request_viewport_position(gui, line)
  │  [if within safe margins: skip]
  ▼
load_viewport_page(gui, start, end)
  ├─ buffer_generation++
  ├─ cancel pending conceal/highlight idles  (via generation counter)
  ├─ signal_handler_block(vadj)
  ├─ viewport_set_range(gui, start, end)    (spacers ≥ 1px)
  ├─ gui_set_text(content, len)
  └─ g_timeout_add(10ms) → unblock vadj signal
```

---

## Invariants: What Must Never Happen

| Forbidden | Consequence |
|---|---|
| `gui_set_text` called off the main thread | Asserted via `g_assert(g_main_context_is_owner(...))` |
| Spacer height set to 0 | GTK layout invalidation → corrupted widget pointer crash |
| `GtkTextIter` used after `gui_set_text` returns | Undefined behaviour — use generation counter to cancel |
| Any module calling `load_viewport_page` directly | Bypasses safe-margin guard and re-entrancy check |
| Cursor col set without byte-clamping | `Byte index N is off the end of the line` abort |

---

## Notes

1. Scroll, cursor, and open-file paths must not each decide viewport reload independently. All go through `request_viewport_position`.
2. `request_viewport_position` is the only place that tunes safe margins and centering offsets.
3. The `buffer_generation` pattern is the contract for all deferred buffer access — every new `g_idle_add` that touches the buffer must follow it.
