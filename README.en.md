<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="src/ui/icons/qirtas-logo-dark.png">
  <img src="src/ui/icons/qirtas-logo.png" width="150" alt="Qirtas">
</picture>

# Qirtas · قِرطاس

A markdown notebook for Linux that tries to stay a notebook.

**English** · [العربية](README.md)

<sub>Zig core · GTK4 / Libadwaita · no Electron · no accounts · no AI in the editor</sub>

</div>

---

## What it is

*Qirtas* is the Arabic word for a sheet of paper. That's the whole idea.

You write in plain Markdown. Instead of a wall of `#`, `**` and bracket noise,
Qirtas shows you a clean page — the syntax steps out of the way while you type
and comes back the moment your cursor lands on it. A `# heading` looks like a
heading. A `- [ ] task` becomes a checkbox you can tick. A code block turns into
a card you can copy from. None of that touches your file: what's on disk is still
ordinary `.md` you can open in anything, sync with anything, grep, and back up.
Nothing is trapped inside Qirtas.

Arabic is built in from the start, not bolted on: proper right-to-left layout,
per-paragraph direction that markdown can't confuse, search that ignores alef
forms and tashkeel, Eastern numerals, and an Arabic *matn* style for PDF export.

No subscription. No login. No assistant watching you write. It opens fast and
gets out of the way.

## Screenshots

<div align="center">

| Editor | Read mode |
|:---:|:---:|
| ![Editor](assets/screenshots/editor.png) | ![Read mode](assets/screenshots/read-mode.png) |
| **Paper & Ink themes** | **Arabic, right-to-left** |
| ![Themes](assets/screenshots/themes.png) | ![Arabic RTL](assets/screenshots/arabic-rtl.png) |

</div>

## Install

### AppImage — easiest, any Linux, nothing to install

1. Download `Qirtas-x86_64.AppImage` from the [Releases](https://github.com/Ahmed-Sinkeat/Qirtas/releases) page.
2. Make it runnable:
   ```sh
   chmod +x Qirtas-x86_64.AppImage
   ```
3. Run it:
   ```sh
   ./Qirtas-x86_64.AppImage
   ```

No root, nothing copied into your system. (Needs FUSE, which almost every distro
already has.)

### Arch Linux

```sh
yay -S qirtas-git
```

### From source

You'll need Zig, GTK4, Libadwaita, GtkSourceView 5 and SQLite.

```sh
git clone https://github.com/Ahmed-Sinkeat/Qirtas.git
cd Qirtas
zig build run
```

Your notes and settings live in `~/.config/qirtas/`.

## Current state

Pre-1.0, written and maintained by one person, and used every day to take real
notes. It works — but expect the occasional rough edge. Because your notes are
plain `.md` files, the worst case is still just Markdown sitting on your disk.
Known issues are tracked openly in [docs/ISSUES.md](docs/ISSUES.md).

## What I plan to add

- A better Arabic writing experience
- Spell check
- Better export and templates
- Stronger table support
- A phone version
- DOCX import / export
- A plugin system

## Contributing

Start with [docs/STRUCTURE.md](docs/STRUCTURE.md) — it explains how the project
is laid out and where things live (Zig owns the document text; the C/GTK side
draws it).

Then there's one question every feature has to answer first:

> **Does this feature make Qirtas a better notebook?**
>
> If yes — it might belong in the project.
> If no — it's a separate add-on, or it doesn't get added at all.

That line keeps a notes app a notes app. Open an issue before a big change so we
can check it against that question together.

---

<div align="center"><sub>GPL-3.0 · made for writing, not for platforms</sub></div>
