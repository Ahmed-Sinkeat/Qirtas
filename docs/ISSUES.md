# Qirtas — Known Issues

**Last audited:** 2026-06-17 (branch `full-buffer-editor-v2`).

This file tracks correctness/robustness issues found by code audit. It also
records where the prose docs had drifted from the actual source so the drift
isn't re-introduced.

Method: each finding below was located in source, then a second pass tried hard
to *refute* it against the current tree. Items under "Confirmed" survived that
refutation pass. Items under "Unverified candidates" were located but the
refutation pass did not complete (session limit) — treat as leads, not facts,
until checked.

---

## Confirmed issues

### 1. 🔴 CRITICAL — decrypt-failure loads ciphertext as plaintext, then autosave overwrites it (data loss on machine migration)

> **✅ FIXED 2026-06-17.** Added a magic/version header to the encrypted on-disk
> format (`ENC_MAGIC` + `ENC_VERSION`, `src/main.zig`). `load_file_mmap` now: a
> file carrying the header **must** decrypt — on no-key/wrong-key it sets a new
> `active_load_failed` read-only flag, loads empty, and never writes the
> ciphertext back as plaintext. Both save paths (`zig_save_active_page`,
> `zig_save_document`) refuse while `active_load_failed`. Headerless (legacy)
> files keep loading via tag-verified trial decrypt; they self-upgrade to the
> header format on next save. GUI surfaces a "Cannot decrypt — wrong key"
> toast + status. Regression test: *"integration: encrypted file with wrong key
> is read-only, save refuses, ciphertext survives."*

**Where:** `src/main.zig` — `load_file_mmap` (~1304-1338), save paths
`zig_save_active_page` (~1565,1598-1605) and `zig_save_document` (~2083-2090);
key derivation `src/sync.zig` `deriveKey` (~1828-1839); `initMasterKey`
(`src/main.zig` ~425).

**Chain (verified end-to-end):**

1. The vault master key is wrapped by a key derived purely from
   `SHA256(/etc/machine-id)`. If that anchor changes — vault copied to another
   machine, restored backup on different hardware, OS reinstall, container with
   a fresh machine-id — `decryptToken` fails and `active_master_key` stays
   `null`. The key is **not** regenerated, because the new-key branch only runs
   when the `system_keys` row is *absent*, not when decrypt fails.
2. With a null key, `load_file_mmap` skips decryption and loads the raw
   `nonce+ciphertext+tag` bytes into the editor as plaintext, setting
   `active_file_is_encrypted = false`. The encrypted format has **no
   magic/version header**, so nothing can tell "this should have decrypted."
3. Session-restore reopens every previously-open tab on boot unguarded, so
   encrypted files load as garbage automatically.
4. The save guard only checks the inverse case (`active_file_is_encrypted &&
   key == null`). Because step 2 set the flag false, the guard passes and the
   2.5 s autosave calls `atomicWriteFile`, atomically overwriting the original
   ciphertext with the garbage-as-plaintext. Clean, complete, unrecoverable.

**Impact:** copy an encrypted vault to a second machine, open it, type one
character → ~2.5 s later every encrypted note is permanently destroyed. The
atomic-write "safety" guarantees the destruction is total. No test covers the
decrypt-failure path.

**Fix:** add a magic/version header to the encrypted on-disk format. When a file
carries the header but decrypt fails, refuse to load it as plaintext **and**
refuse to save over it — surface a read-only "cannot decrypt — wrong key"
state. Minimum stopgap: when `active_master_key != null` and a non-empty file
fails to decrypt, set a load-failed/read-only flag that blocks save.

> This is the inverse of what the earlier review feared. History is *not*
> stored plaintext (it's encrypted, see "Doc-vs-reality fixes"); the real crypto
> risk is that a failed decrypt silently destroys data.

---

### 2. 🟠 HIGH — cross-thread use-after-free of the GitHub poll `cancelled` flag

> **✅ FIXED 2026-06-17.** Replaced the widget-lifetime-tied `cancelled` bool and
> the raw dialog pointer with a refcounted `GithubAuthState`
> (`src/gui/gui_sync.c`): worker and dialog each hold a ref, last drop frees it.
> `cancelled` is now read/written with `g_atomic_int`; the dialog pointer is
> touched only on the main thread (set at creation, nulled in the destroy
> handler, read in the idle callback), so the worker never dereferences a
> finalized GObject.

**Where:** `src/gui/gui_sync.c` — alloc/bind ~272-274, thread handoff ~288,
worker reads ~154,160; dangling dialog pointer ~141,189.

The `cancelled` bool is heap-allocated and bound to the dialog GObject via
`g_object_set_data_full(..., g_free)` (GTK frees it when the dialog finalizes).
The **same raw pointer** is handed to a detached worker thread that polls for up
to 900 s, reading `*cancelled` every 5 s. Close the auth dialog before
authorizing → GTK frees the bool → the worker's next wakeup dereferences freed
heap (UAF) and the access is unsynchronized (no atomic/mutex). The success path
also stashes the raw dialog pointer (`ud->dialog_to_close = pd->dialog`) and
later derefs it via `GTK_IS_WINDOW(...)`, reading a possibly-finalized GObject.
The thread is never joined, so it can outlive the dialog and the GUI.

**Impact:** linking a GitHub account then closing/auto-closing the dialog before
auth completes can crash or corrupt the heap minutes later — exactly the
hard-to-reproduce crash class flagged in review.

**Fix:** don't tie the cancel flag's lifetime to the widget. Share ownership via
an atomic-refcounted (or mutex-guarded) struct that both `on_dialog_destroy` and
the worker drop a ref on; free only on last ref. Signal cancellation through
`GCancellable`/`g_atomic`. Marshal the dialog close back to the main thread; never
deref the raw dialog pointer from the worker.

---

### 4. 🔴 CRITICAL — autosave serializes the *decorated view*; child-anchor markers (HR / table header / code-fence) are dropped → silent loss of those lines

> **✅ FIXED 2026-06-17.** `gui_trigger_autosave` (`src/gui.c`) no longer reads
> the GTK view via `gtk_text_buffer_get_text`; it now calls `zig_save_document()`,
> which serializes `doc_buf` (the source of truth, with raw markdown intact).
> `zig_save_document` gained a `gui_index_file` call so autosave still refreshes
> the search index. This also closes the two-save-paths-disagree smell.

**Where:** `src/gui.c` `gui_trigger_autosave` (~3382-3416); `src/main.zig`
`zig_save_active_page` (~1563-1622); decoration renderers `parse_and_render_hrs`
(`gui_hr.c` ~10-54), `parse_and_render_tables` (`gui_table.c` ~171-212,
`render_one_table`), `parse_and_render_code_pills` (`gui_codeblock.c` ~144).

**Chain (verified end-to-end against the tree):**

1. The architecture says Zig's `doc_buf` is the source of truth and the GTK
   buffer is a presentation layer (STRUCTURE.md "Document Ownership Model"). The
   view-only decorations honor this by **replacing the marker text with a
   `GtkTextChildAnchor`** while leaving the raw markdown in `doc_buf`: HR deletes
   `---` and inserts an anchor (`gui_hr.c:40-50`), tables delete the header line
   text and insert a grid anchor (`gui_table.c:188-195`), code blocks replace the
   opening fence line with a pill anchor. Handlers are blocked during these edits
   so `doc_buf` is intentionally *not* updated — by design the view now differs
   from `doc_buf` on those lines.
2. The **debounced autosave path does not read `doc_buf`.** It reads the *view*:
   `page_text = gtk_text_buffer_get_text(buf, &start, &end, FALSE)`
   (`gui.c:3396`) and passes it to `zig_save_active_page(0, line_count,
   page_text)`.
3. `gtk_text_buffer_get_text()` **omits the object-replacement character for
   embedded child anchors** (GTK contract: "Does not include characters
   representing embedded images" — only `gtk_text_buffer_get_slice()` returns the
   `U+FFFC`). So every anchored line comes back as the **empty string**.
4. `zig_save_active_page` with `start_line=0, end_line=line_count` computes an
   empty prefix/suffix and **replaces the entire `doc_buf` with `page_text`**,
   writes it to disk (re-encrypted), then `remap_active_file()` reloads `doc_buf`
   from that file. The raw `---` / table-header / ```` ``` ```` lang fence in
   `doc_buf` is now overwritten by an empty line.

Result after one autosave with any rendered decoration present:
- a rendered `---` HR → blank line (the rule silently disappears);
- a rendered table → header row markdown gone, leaving an orphan delimiter+body
  (the scale-hidden delimiter/body lines *are* returned by `get_text` because
  they use a `scale 0.01`/transparent tag, **not** the `invisible` property);
- a fenced code block → opening ```` ```python ```` fence (with its language)
  gone, leaving dangling code + closing fence.

**Why it has hidden so far:** the decorations are deferred to idle and re-render
on every reload, so a fresh open *looks* fine; corruption lands only after the
2.5 s autosave fires while a decoration is on screen — i.e. the default editing
path. The **other** save path, `zig_save_document` (used on tab close,
`gui_tabs.c:423`), reads `doc_buf` directly and is correct — so the two save
paths disagree on correctness, which is itself the smell that points at the bug.

**Impact:** silent, unrecoverable loss of horizontal rules, table headers and
code-fence openings on the default autosave path. Combines with issue #1 (the
atomic write makes the corruption total) and #5/#6 below.

**Fix:** autosave must serialize the source of truth, not the view. Either route
`gui_trigger_autosave` through `zig_save_document` (which already saves
`doc_buf`), or have it fetch text via `zig_get_document_text()` instead of
`gtk_text_buffer_get_text`. If the view must be the source, use
`gtk_text_buffer_get_slice` and map each `U+FFFC` back to its stashed markdown
(tables already stash it on the anchor as `table-md`). Add a behavioral test:
render an HR, mark modified, run the autosave serializer, assert the on-disk
bytes still contain `---`.

---

### 5. 🟠 HIGH — second GitHub-auth use-after-free: `verification_uri` freed while still bound to the "Open Activation Page" button

> **✅ FIXED 2026-06-17.** The button now gets its own copy with a destroy
> notify: `g_signal_connect_data(btn_open, "clicked", cb, g_strdup(dd->verification_uri),
> (GClosureNotify)g_free, 0)` (`src/gui/gui_sync.c`). The closure owns the copy
> and frees it on button finalize, so freeing `dd->verification_uri` at the end
> of the idle callback no longer leaves a dangling reference.

**Where:** `src/gui/gui_sync.c` `show_github_dialog_idle` — button wiring ~262-265,
free ~293-296.

Separate from issue #2's `cancelled`/dialog UAF. The dialog builds an "Open
Activation Page" button and binds the **raw** `dd->verification_uri` pointer as
its click user-data:

```c
g_signal_connect(btn_open, "clicked", G_CALLBACK(on_open_activation_clicked), dd->verification_uri);
```

At the end of the same idle callback the owning struct is torn down and the
string is freed:

```c
g_free(dd->verification_uri);   // ~295
```

The button outlives the callback, so its closure now holds a dangling pointer.
Clicking "Open Activation Page" passes freed heap to `gtk_show_uri`
(`on_open_activation_clicked`, ~211-215) — UAF read. (The auto-open at ~280 fires
before the free and is fine; only the manual button click trips it. The polling
thread gets its own `g_strdup` copy at ~287, so this is strictly the button's
dangling reference.)

**Fix:** hand the button its own copy with a destroy notify —
`g_signal_connect_data(btn_open, "clicked", cb, g_strdup(dd->verification_uri),
(GClosureNotify)g_free, 0)` — or stash the URI on the dialog GObject with
`g_object_set_data_full(..., g_free)` and read it in the handler.

---

## Open architectural issue (mechanism confirmed, no code fix yet)

### 3. 🟡 App writes into its own source tree when `src/` is opened as a vault

`zig_open_vault` lets the user open any directory as a vault, and all vault
writes resolve relative to the vault root (`zig_create_folder` → `c.mkdir` on a
bare name; `zig_move_path` drops files at the bare basename; saves call
`atomicWriteFile` on the relative path). Running the binary with `src/` as the
working dir deposited scratch/test files (`test.md` — an encrypted blob —,
`sa.md`, `Wiki link.md`, `ىى.md`, and a clobbered `english.txt` full of Zig
code) into `src/`, where they got committed. These have now been deleted
(see "Doc-vs-reality fixes"), but the **root cause is still open**: there is no
separation between "app source tree" and "vault directory." Real fix =
external-files / vault separation so the app can never write into its own repo.

---

## Lower-severity / hygiene

- **No behavioral C tests.** ~14,750 lines of C/GTK (27 files) are compiled and
  link-checked by `zig build test` but never *exercised* — not one test calls a
  `gui_*` function. All 35 test blocks are Zig. Most historical crashes lived in
  this layer. Highest-value first tests: the conceal pass, the HR renderer, and
  buffer-replace paths.
- **Flatpak manifest is a non-building placeholder.** `packaging/org.qirtas.notebook.yml`
  still has `sha256: REPLACE_WITH_REAL_SHA256`. Arch PKGBUILD + AUR + AppImage
  recipes exist and build, but there are **no prebuilt binaries and no CI/release
  pipeline** — CI only builds + tests. README front door advertises only
  `zig build run`.
- **Conceal residual risk.** The documented SIGABRT ("Byte index N off the end
  of the line") is fixed (the `get_iter_at_position` paths were rewritten) and
  `scale 0.01` replaced the crashing invisible-tag approach. A heuristic skip
  (`conceal_line_hostile`: lines with `<`, `|`, or >2000 chars) and a kill-switch
  (`QIRTAS_NO_CONCEAL=1` + "Conceal Markdown Markers" pref) remain as
  defense-in-depth. The underlying GTK char-to-layout limitation is unfixed, so
  pathological lines still carry some risk — but the known reproducible crash is
  closed.
- **History keyed by basename.** Two same-named files in different folders share
  file-history; restore replaces current content and cannot be undone
  (`gui_history.c`).

- **🟡 Live HR insert diverges the view from `doc_buf` (line count + content).**
  ✅ FIXED 2026-06-17 (line-count skew). `check_and_insert_hr` (`gui_hr.c`) no
  longer injects a view-only `\n` after the HR anchor. The handlers are blocked
  during the swap, so that newline never reached `doc_buf` and left the view one
  line longer — every later edit then mapped view `(line,col)` onto the wrong
  `doc_buf` line (`iter_to_position` → `zig_replace_range`). Replacing the marker
  with a single-char anchor keeps the view's line count identical to `doc_buf`,
  so the mapping stays correct. (The data-loss aspect was already closed by #4:
  autosave now serializes `doc_buf`, which keeps the raw `---`.)
  **Residual (cosmetic, not fixed):** the per-line *content* still differs (anchor
  vs `---`), and a `--- ` trailing-space trigger leaves `--- ` in `doc_buf`, which
  `parse_and_render_hrs` (exact-`---` match) won't re-render after reload. No data
  loss, no mapping skew — the marker just shows as literal text until retyped.

- **🟢 LOW — `zig_get_text_for_line_range` missing the negative-line guard its
  sibling has.** ✅ FIXED 2026-06-17. Added the symmetric
  `if (start_line < 0 or end_line < 0) { out_len.* = 0; return ""; }` guard
  before the `@intCast`s in `main.zig`.

- **🟢 LOW — `zig_get_document_text`/`zig_free_document_text` mishandle an
  embedded NUL.** ✅ FIXED 2026-06-17. `zig_get_document_text` now replaces any
  interior NUL with a space in the returned copy, so `mem.len()` equals the
  allocated length at free time. (The C consumers treat the result as a
  NUL-terminated string anyway, so an interior NUL would already truncate them.)

- **🟢 LOW (perf) — undo push is O(N²) in the worst case.** `pushUndoEntry`
  (`main.zig:277`) calls `stackTotalBytes` (O(top)) inside a `while` eviction
  loop, and `dropOldestEntry` memmoves the whole 100-slot array on each eviction.
  With many large snapshots near the 64 MB cap this is quadratic per keystroke
  boundary. A running byte total + ring buffer makes it O(1).

---

## Unverified candidates (located, refutation pass incomplete)

Surfaced by the audit but **not** yet adversarially confirmed — verify before
acting:

- Undo-snapshot correctness across viewport reloads (`main.zig` `captureUndoEntry`
  / `restoreSnapshot` ~288-307, 1972-2015).
- Table / HR offset math under malformed input (`gui_table.c`, `gui_hr.c`) — see
  the confirmed live-HR divergence under "Lower-severity / hygiene" for one
  concrete case.

### Candidates resolved this pass

- ❎ **REFUTED — CR-stripping mismatch (`loadDocFromSlice` vs
  `populate_line_offsets`).** `loadDocFromSlice` (`main.zig:1355-1376`) normalizes
  every `\r` and CRLF to a single `\n` *before* calling
  `populate_line_offsets(doc_buf.items)`. `populate_line_offsets` therefore always
  runs on already-normalized text, and its `\r`-handling `else if` branch
  (~1389) is dead code. No offset drift on CRLF files. (Cosmetic: the dead branch
  can be removed.)
- ❎ **REFUTED — nonce reuse in `encryptToken` / `encryptWithMasterKey`.** Each
  call draws a fresh 96-bit nonce — `encryptToken` reads 12 bytes from
  `/dev/urandom` (`sync.zig:1845-1850`), `encryptWithMasterKey` uses
  `fillRandomBytes` (`main.zig:310-311`) — and prepends it to the ciphertext. No
  counter/static nonce, no reuse across messages; random-96-bit collision risk is
  negligible at this volume. ChaCha20-Poly1305 usage is standard.

---

## Doc-vs-reality fixes applied 2026-06-17

The prose docs had drifted from source. Corrected in this pass:

| Doc claim (stale) | Reality | Fixed in |
|---|---|---|
| `file_history` snapshots stored **plaintext** in encrypted vaults | Snapshots are **encrypted** (ChaCha20-Poly1305 under master key) before insert; null key = no row written | `STRUCTURE.md`, `As-Built §` |
| **No restore UI** — recovery is manual SQL | Full point-and-click restore UI exists (`show_file_history`, reachable from status-bar + explorer "File History") | `As-Built §` |
| `gui.c` is **~4900 lines** and **exempted** from the size check | `gui.c` is **3512 lines**; it is simply not enumerated in the `modular_gui_files` array the check iterates (no whitelist), and the check is advisory (WARNING only, never fails the build) | `As-Built §` |
| Scratch files in `src/` "untracked and gitignored (2026-06-12)" | They were **still git-tracked** despite being in `.gitignore`; the untrack never landed. Now deleted | `As-Built §`, `.gitignore` already had them |
| `.bak`/`.step*` files "still present in `src/`" | **None exist** anywhere in the tree | `As-Built §` |
| `test_large.md` "untracked (2026-06-12)" | Was still tracked; now `git rm --cached`'d, kept on disk as profiling scratch | `As-Built §` |

Junk deleted this pass: `src/sa.md`, `src/test.md`, `src/Wiki link.md`,
`src/ىى.md`, `src/english.txt`, `src/untitled.md`, `untitled.md`,
`cursor_corruption_evidence.md` (obsolete — documents the already-fixed
`get_iter_at_position` crash), `log.txt`, and the stray Claude session dump.
`test_large.md` untracked but kept on disk.

**Recommendation (not yet done):** move `src/As-Built Specification Document.md`
and `src/STRUCTURE.md` out of `src/` into `docs/`. They are real docs sitting in
the vault-clobber zone — `english.txt` was already destroyed there. Holding off
because the move would also need link updates in `docs/plans/`.
