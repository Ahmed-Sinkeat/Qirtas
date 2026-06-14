# Manual smoke checklist

Run before every push that touches the editor core (`src/main.zig` edit paths,
`src/gui.c` buffer handlers, `src/gui/gui_conceal.c`, undo, save). ~10 minutes.

## Performance
- [ ] Open empty file, don't touch anything: CPU near 0% (cursor trail on AND off)
- [ ] Open `test_large.md` (3,000+ lines Arabic), type a sentence mid-file: no visible lag
- [ ] Delete word-by-word (Ctrl+Backspace) in the big file: no lag, no screen jump

## Undo
- [ ] Type three words, Ctrl+Z three times: removes word-by-word back to start
- [ ] Ctrl+Z to the original loaded state works (baseline snapshot)
- [ ] Ctrl+Shift+Z / redo replays
- [ ] Undo after paste restores pre-paste state

## Save / recovery
- [ ] Type, wait 3s: status flips to Synced (debounced save fired)
- [ ] Type, kill the app (`kill -9`) within 2s, reopen: at most ~2.5s of typing lost
- [ ] `file_history` table in vault DB gains a row after editing + pause (≥5 min since last)
- [ ] Encrypted vault: same two checks pass

## Arabic
- [ ] Type Arabic in a fresh line: paragraph flips RTL immediately
- [ ] Search نقية finds نقيه (and vice versa)
- [ ] Eastern numerals render in word/char counts when UI language is Arabic

## General
- [ ] Switch tabs with unsaved changes, switch back: content intact
- [ ] New file: click places cursor without needing right-click first
- [ ] Print/PDF export one Arabic page: RTL, title present
