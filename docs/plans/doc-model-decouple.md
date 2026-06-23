# Refactor: decouple the doc model from the view (Option C)

**Branch:** `refactor/doc-model-decouple`
**Status:** Stages 1–2 done & committed; Stage 3 next.
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
| 3 | Convert **HR** to a true fold (proof case) | high | ⏳ next |
| 4 | Convert tables, code blocks, todos | medium | ⬜ |
| 5 | Custom gutter + delete `scale:0.01` hacks & dead guards | medium | ⬜ |

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

## 5. Stage 3 — the next concrete task (HR fold)

HR is the simplest decorator (`src/gui/gui_hr.c`): today it replaces a `---`
line's text with a rule child-anchor and (for multi-line cases) hides the rest
with `scale:0.01`. Convert it to a **true fold**:

1. On render: `gtk_text_buffer_delete` the source line(s), insert ONE anchor line
   with the rule widget, then `foldmap_register(buf, anchor_view_line, doc_count)`.
   Keep the existing signal-handler blocking so `doc_buf` is NOT touched (the
   source `---` stays in `doc_buf`).
2. On reveal-at-cursor: re-insert the raw source line(s) and
   `foldmap_unregister_at(buf, view_line)`.
3. Drop the `scale:0.01` hide tag for HR.

**Verify hard (this is the first non-identity fold):**
- HR shows as a rule; **gutter line number is correct, not compressed**.
- Type text on the line *before* and *after* the HR → it mirrors to the **right
  doc line** (check by saving and diffing).
- **Save round-trips byte-identical** (the `---` is restored in the file).
- Cursor into the HR → raw `---` returns; cursor out → re-folds.
- No crash on click / arrow / vertical motion across the fold.

Then Stage 4 repeats the pattern for `gui_table.c`, `gui_codeblock.c`,
`gui_todo.c`, and Stage 5 adds a custom gutter renderer + deletes the now-dead
`scale:0.01` hide tags and the guard flags that only existed to keep line count
stable.

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
