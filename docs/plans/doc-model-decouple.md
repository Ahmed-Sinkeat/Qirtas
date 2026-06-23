# Refactor: decouple the doc model from the view (Option C)

**Branch:** `refactor/doc-model-decouple`
**Status:** COMPLETE. Stages 1–4 done & committed (the view/doc decouple + table
fold + the decorator corruption fixes that were the whole point). Stage 4b and
Stage 5 were deliberately dropped as disproportionate cosmetics (see below).
**Read this first** if you're resuming cold — it's the whole picture in one file.

---

## 0. What this app is (one paragraph)

Qirtas is a markdown editor: Zig core (`src/main.zig`, `src/markdown.zig`,
`src/sync.zig`) + a GTK4/libadwaita C front-end (`src/gui.c`, `src/gui/*.c`),
glued by a C↔Zig FFI (`src/gui_shared.h`). It does Obsidian-style *live preview*:
the markdown source lives in a `GtkSourceView`, and decorations (hidden `*`/`#`
markers, rendered tables, code-block pills, horizontal rules, todo checkboxes)
are drawn **in place** over that same editable buffer.

---

## 1. The four bug fixes already landed (commit `936e20e`)

These were the entry point; all verified, all on this branch.

1. **Conceal ate characters** ("with \*asterisks\*" rendered as "wit \*asteriss\*").
   Root: the conceal pass read the buffer with `gtk_text_buffer_get_text()`, which
   **omits child-anchor characters** (rendered tables/code/todos), so offsets
   drifted left by one per preceding widget and the conceal tag shrank real text
   instead of the markers. Fix: `gtk_text_buffer_get_slice()` (keeps anchors as
   U+FFFC, offsets stay aligned). `src/gui/gui_conceal.c`.
2. **Read mode wasn't read-only** (format/insert menu still mutated the doc).
   `set_editable(FALSE)` only blocks typing. Fix: guard `gui_buffer_replace`
   (`gui_buffer.c`) + `apply_paragraph_alignment` (`gui_popover.c`) on `read_mode`.
3. **Horizontal over-scroll** into empty space (touchpad pan past content).
   Fix: h-scroll policy `NEVER` while wrapping; reset h-adjustment to 0 on
   read-mode entry. `src/gui/gui_layout.c`.
4. **Sidebar started open.** Now always starts collapsed (F9/logo opens it,
   never persisted open). `src/gui.c` (after `apply_focus_mode`).

---

## 2. The architecture audit — why bugs keep coming back

The user asked: "all my problems are gtk-c-buffer related — find everything
designed wrong." Findings, ranked:

### TIER 1 — the root tension (what this refactor fixes)
- **The view buffer is the source of truth, mirrored 1:1 to the Zig `doc_buf`
  by line number.** `Position = {view_line, col}`; the C↔Zig mirror
  (`on_insert_text_after`, `on_delete_range_before`, `iter_to_position`) feeds
  raw view line numbers to Zig. So **every decoration must preserve line count.**
  Tables/code/HR therefore can't *remove* their extra lines — they hide them with
  a `scale:0.01` + transparent-ink tag. That single constraint causes:
  - **compressed gutter line numbers** (hidden lines are ~0px tall but still
    real lines, so they still get numbered → the numbers stack into a few px), and
  - **the "1px dot"** (a `scale:0.01` line isn't truly invisible — it leaves a
    selectable sub-pixel sliver).
- **The decoration re-entrancy web.** Because decorations mutate the same buffer
  that signal-handlers observe and mirror to `doc_buf`, every decorator blocks
  2–5 signal handlers and defers work through ~10 dirty/guard flags
  (`in_conceal_update`, `*_dirty`, `buffer_generation`, `*_queued`). 29 comments
  document crashes these guards exist to prevent. Fragile; grows per feature.

### TIER 2 — independent, worth doing
- Undo is **full-document snapshots** reimplemented in Zig (native
  `GtkTextBuffer` undo is disabled). Wasteful; → delta undo.
- **Open-tab contents duplicated** as C strings (`gui->tabs.contents[]`) separate
  from `doc_buf`. Two copies, sync points, memory.
- **Single-window globals** (`global_gui`, `global_source_view`) but GApplication
  can spawn extra windows → latent corruption. Enforce single-window (route opens
  to tabs).

### TIER 3 — bloat / non-idiomatic (ponytail)
- `sync.zig` is 3522 lines: three hand-rolled OAuth backends (Drive/Dropbox/
  GitHub, all wired) with heavy duplication. Factor common OAuth/provider; split.
- Busy-wait mutex (`while(!tryLock()) usleep(10)`) → `Mutex.lock()`.
- Document is a flat `ArrayList(u8)` → O(n) per keystroke. YAGNI for notes; flag.

### TIER 4 — cosmetic
- `active_mmap_ptr/size` no longer an mmap (misleading name). Several C files
  >600 lines (the build lint warns).

**Decision:** the user chose **Option C** — fix Tier 1 at the root by decoupling
view lines from doc lines. Tiers 2–4 are deferred (see §6).

---

## 3. The plan — Option C in 5 stages

Make the view free to have **fewer lines than the doc** (a rendered region =
one anchor line, source lines gone from the view, kept only in `doc_buf`). A
**fold-map** translates line numbers between the two coordinate spaces.

| # | Stage | Risk | State |
|---|-------|------|-------|
| 1 | Pure fold-map translation core (test-first) | none | ✅ `baa53ef` |
| 2 | Live registry + wire the mirror seam (inert/identity) | high | ✅ `eff8d0a` |
| 3 | Convert **tables** to a true fold (proof case) | high | ✅ `fbfc24d`+`3cfbbd4`, live-verified |
| 4 | Decorator corruption fix (todo/codeblock/hr) | medium | ✅ `e43e33f`, todos live-verified |
| 4b | Code-block closing-fence fold (cosmetic) | medium | ❌ declined — per-block re-fold too much machinery (§5b) |
| 5 | Custom gutter to skip hidden lines | medium | ❌ DROPPED (2026-06-24) — see below |

**Stage 5 dropped.** With tables folded, the only artifact left is one squished
gutter-number pair per code block (the `scale:0.01` closing fence). Fixing it
means replacing GtkSourceView's built-in line-number renderer with a custom
`GtkSourceGutterRendererText` subclass and re-implementing renumber + current-line
highlight + stable width across 8 themes — ~100+ lines of fragile GObject for one
cosmetic squish. Not worth it. Accepted as-is; revisit only if it annoys in real
use. (Any future code-block "delete the scale:0.01 hide" cleanup would also need
the per-block re-fold from §5b, which was likewise declined.)

Stage 4 finding: **todos and HR are single-line decorations** (marker text →
anchor on the *same* line, no hidden lines) — they need NO fold, only the
delete-mirror fix. The only decorator left with a hidden line is the **code-block
closing fence** (one `scale:0.01` line per block); see §5b.

---

## 4. How the fold-map works (so the code makes sense)

A **fold** collapses a contiguous run of `doc_count` doc lines onto **one** view
line (the anchor widget). Type `Fold {view_line, doc_line, doc_count}` in
`src/gui_shared.h`.

**Pure translators** (`src/gui/gui_foldmap.c`, no GTK, unit-tested in
`tests/test_c_behavior.c` → `zig build test-c`):
- `foldmap_view_to_doc(folds, n, view_line)` — anchor line → its first doc line;
  lines after a fold are pushed down by its `(doc_count-1)` hidden lines.
- `foldmap_doc_to_view(folds, n, doc_line)` — a doc line *inside* a fold maps to
  that fold's anchor line; lines past it are pulled up.
- Input array MUST be sorted by `view_line` and self-consistent
  (`doc_line[i] = view_line[i] + Σ(doc_count[j]-1, j<i)`).

**Live registry** (same file, GTK part):
- Each fold pins a **`GtkTextMark`** (left gravity) on its anchor line, so edits
  above/below shift it automatically — we never store a raw line number that
  goes stale. `doc_count` is stored alongside.
- `foldmap_snapshot(buf,…)` rebuilds a sorted `Fold[]` from the live mark
  positions on demand; the `_live_` wrappers call the pure translators on it.
- API: `foldmap_register(buf, view_line, doc_count)`,
  `foldmap_unregister_at(buf, view_line)`, `foldmap_clear(buf)`.
- Empty registry ⇒ every translation is the **identity** (fast-path returns the
  input), so the whole layer is inert until a decorator folds.

**The invariant:** *every C-side `Position` is a DOC-buffer line.* Conversions
happen only at the GTK boundary — these 6 sites are already wired (Stage 2):
- view→doc (reading an iter): `iter_to_position`, `gui_get_cursor_position`,
  `on_insert_text_after`, `on_delete_range_before`.
- doc→view (placing caret/selection): `gui_set_cursor_position`,
  `select_position_range`.
- `foldmap_clear` is called in `gui_set_text` (file load / tab switch).

---

## 5. Stage 3 — the next concrete task (TABLE fold)

NOTE: HR (`gui_hr.c`) is NOT a fold — it already swaps `---` for an anchor on the
**same** line (line count preserved, no hidden lines, no artifact). Leave it.

The real multi-line fold — and the source of the compressed gutter numbers
(Image #6) — is the **table** (`src/gui/gui_table.c`). Today `render_one_table`
replaces the header line's text with a `GtkGrid` child anchor (same line) and
**scale:0.01-hides** the delimiter + body rows. Those hidden rows are the
compressed line numbers and the clickable "1px" lines. Convert to a true fold:

1. `render_one_table`: keep header→grid anchor, but **`gtk_text_buffer_delete` the
   delimiter+body lines** (don't hide them) so the table is ONE view line. Then
   `foldmap_register(buf, header_view_line, N)` where N = total table lines. The
   raw markdown is already stashed on the anchor (`"table-md"`). Handlers stay
   blocked so `doc_buf` keeps all N lines.
2. `gui_table_reveal_at_cursor`: cursor on the anchor line → delete the 1 anchor
   line, re-insert the raw N lines, `foldmap_unregister_at(buf, view_line)`.
3. `rerender_revealed_if_left`: re-fold when the cursor leaves.
4. Cursor-in-table detection now just checks the single anchor line (the
   `md-table-region` tag spans one line, or drop it for an anchor check).

**Verify hard (first non-identity fold) — DONE, live-verified on a 2-table doc:**
- ✅ Both tables render; **gutter numbers contiguous, not compressed**; no 1px dot.
- ✅ Type after the folds → mirrors to the **right doc line**; **save round-trips
  byte-identical** (all `| ... |` rows restored).
- ✅ Cursor into a table → raw markdown returns; cursor out → re-folds; the other
  table stays folded. No crash on arrow / vertical motion across the fold.

**Bug found by this verify pass (`3cfbbd4`) — READ BEFORE STAGE 4.** Stage 2 moved
the doc_buf delete mirror (`zig_delete_range`) to **`on_delete_range_before`** (it
needs pre-delete iters to foldmap-translate). But every decorator's
`block_handlers` still blocked only `on_delete_range_after`. The moment a fold
*deletes* lines during normal editing, the delete leaks to doc_buf while the
re-insert (correctly blocked) does not → **rows silently dropped from the saved
file**. File-load folds escaped it (`on_delete_range_before` early-returns under
`loading_viewport`); only cursor-driven reveal/re-fold triggered it. Fixed for
tables by also blocking `on_delete_range_before`.

→ **Stage 4 MUST do the same** in `gui_codeblock.c` / `gui_todo.c` (and `gui_hr.c`
if it ever deletes): their `block_handlers` block `on_delete_range_after`, not
`_before`. They don't corrupt *yet* (they don't delete multi-line while editing),
but converting them to delete-based folds without this fix will repeat the bug.

Stage 4 applies the fold pattern to the code-block closing fence
(`gui_codeblock.c`) and todos (`gui_todo.c`); Stage 5 adds a custom gutter
renderer (if still needed) + deletes the dead `scale:0.01` hide tags and the
guard flags that only existed to keep line count stable.

---

## 5b. Code-block closing-fence fold (optional, undecided)

The only `scale:0.01`-hidden line left is the code-block **closing fence** (one
per block; `gui_codeblock.c` `fence_hide_tag`). Folding it (delete from view +
`foldmap_register`) would remove the last compressed gutter number / selectable
1px sliver.

**Why it's NOT a clean copy of the table fold:** a table is *atomic* — the whole
thing reveals to raw on cursor-enter and re-folds on leave, so its fold lifetime
is simple. A code block's **body is edited in place** (it stays a visible,
editable region; only the fences are decoration). So a closing-fence fold has no
natural reveal cycle, and the fold mark would desync the moment the user
adds/removes a body line (the hidden closing fence's doc position drifts). Making
it correct means re-locating + re-folding the closing fence on every body edit
(a `code_pill_dirty`-driven re-render). Doable, but real complexity for one
hidden line per block.

Options: (a) implement the re-fold-on-body-edit; (b) leave the closing fence
`scale:0.01` and let **Stage 5's custom gutter** stop numbering hidden lines
(kills the compressed number but not the clickable sliver); (c) leave as-is.

**DECISION (2026-06-24): option (b).** No per-block re-fold — too much machinery
for one hidden line. Stage 5's gutter handles the compressed number uniformly;
the faint clickable sliver is accepted for now.

---

## 6. Out of scope (deferred Tier 2–4 — do NOT start without asking)
Delta undo; tab-content single-source-of-truth; single-window enforcement;
`sync.zig` de-duplication/split; busy-wait mutex; rope/gap-buffer; renames.

---

## 7. How to build / test / run
- Build app: `zig build`  (warns on >600-line files; harmless)
- Pure C logic tests: `zig build test-c`  (no display needed — fold-map lives here)
- Zig unit + integration: `zig build test`
- Run for live testing: `./zig-out/bin/qirtas <file.md>` (single-instance; a 2nd
  launch forwards to the primary). For an isolated test instance that won't touch
  a running one: `dbus-run-session -- ./zig-out/bin/qirtas <file>`.
  **Kill test instances by PID only — never `pkill -f qirtas`** (it has matched
  and killed the user's own running editor; autosave saved the files but the open
  tabs were lost).
- Plain-text (unencrypted) scratch file for save round-trip checks:
  `/tmp/foldtest.md` pattern used during Stage 2.

## 8. Commit trail on this branch
- `936e20e` — the 4 bug fixes (§1)
- `baa53ef` — Stage 1 (fold-map pure core + tests)
- `eff8d0a` — Stage 2 (live registry + wire seam, inert)
- `fbfc24d` — Stage 3 (table → true fold; loop-index fix; drop dead hide tag)
- `3cfbbd4` — Stage 3 fix: block `on_delete_range_before` (table reveal/re-fold)
- `e43e33f` — Stage 4: block `on_delete_range_before` in todo/codeblock/hr
