<div align="center">

<img src="logog.png" alt="Qirtas logo" width="180"/>

# قرطاس · Qirtas

**A native markdown notebook for Linux that feels like ink on paper.**

Zig backend · C GTK4/Libadwaita frontend · no Electron

</div>

---

## What it is

Qirtas (Arabic for *scroll of paper*) is a fast, distraction-light markdown
editor and notebook. The editor floats like a sheet of paper on a desk; the
two signature **Paper & Ink** themes are drawn from the app's light and dark
logos — dark ink on bright paper, bone-white ink on black.

### Features

- **Markdown live editing** — syntax highlighting, concealed markers
  (`**`, `#`, links fade away outside the cursor line), heading scaling,
  task/bullet/numbered list continuation, horizontal rules, wiki-links
  (`[[note]]`) with hover navigation
- **First-class Arabic** — full RTL layout mode, per-paragraph direction by
  first strong character (markdown syntax doesn't fool it), Arabic font
  selection, normalized search (alef forms, ة/ه, ى/ي, tashkeel-insensitive —
  searching مدرسه finds المدرسة), Eastern Arabic numerals, shortcuts that
  work under the Arabic keyboard layout
- **Three Paper & Ink themes** — Qirtas Light (warm paper, navy ink),
  Qirtas Dark, and Paper & Ink Navy — plus custom CSS themes. Each theme's
  editor scheme colors every markdown element distinctly.
- **Animated ink-smear cursor trail** (toggleable; uses the theme color)
- **File management** — file-tree sidebar with a header toolbar (new file,
  new folder, open vault in file manager), right-click row menu (open, open in
  file manager, new folder), `Ctrl+N` new file / `Ctrl+Shift+N` new folder
- **Tabs** with per-file unsaved-changes dot, plus a save-before-close prompt
- **Quick switcher (Ctrl+P, Arabic-normalized fuzzy match), outline panel,
  find & replace, overview minimap**
- **Themed PDF export** — متن (classical Arabic matn typesetting: Amiri naskh,
  rubrication, framed pages, Eastern folios), Paper & Ink, Academic,
  Typewriter — with PDF outline bookmarks from your headings
- **Full-workspace session restore, compact layout mode, English/Arabic UI, two icon sets**
- **On-demand sync** to Google Drive, Dropbox, GitHub, or a local folder —
  all four backends do 3-way conflict detection and keep both versions as
  `_conflict` files instead of silently losing edits ([details](docs/SYNC.md))
**Your notes are plain `.md` files on your disk** — grep them, back them up,
point Syncthing at them. No proprietary format, no lock-in.

## Build & run

Requires Zig (recent), GTK4, Libadwaita, GtkSourceView 5, and SQLite3
(`curl` is only needed for the GitHub connect dialog).

```sh
zig build          # build
zig build run      # run
zig build test     # test suite
```

Config and the vault DB live in `$XDG_CONFIG_HOME/qirtas/`
(default `~/.config/qirtas/`).

## Documentation

| Doc | What's in it |
|---|---|
| [docs/SYNC.md](docs/SYNC.md) | Sync setup per provider, troubleshooting matrix (every error → cause → fix), **per-backend conflict behavior** |
| [docs/SECURITY.md](docs/SECURITY.md) | Honest crypto threat model and the roadmap to a real one |
| [src/STRUCTURE.md](src/STRUCTURE.md) | Source layout, where to edit what, invariants |
| [src/As-Built Specification Document.md](src/As-Built%20Specification%20Document.md) | Architecture decisions, crash post-mortems, FFI bridge, testing status |

## An honest note on "encryption"

Vault content is ChaCha20Poly1305-encrypted, **but** the unlock key currently
derives from the machine's world-readable `/etc/machine-id` on the same disk —
so today this protects against casual file browsing, not a stolen laptop.
Working-directory `.md` files are plaintext. Read
[docs/SECURITY.md](docs/SECURITY.md) before relying on it; the fix roadmap
(passphrase/keyring unlock) is laid out there.

## Status

Pre-1.0, single-developer project under active development on the
`full-buffer-editor-v2` branch. Expect sharp edges; file issues with the
terminal output (`zig build run` prints sync and crash diagnostics to stdout).
