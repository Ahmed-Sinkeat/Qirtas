<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="src/ui/icons/qirtas-logo-dark.png">
  <img src="src/ui/icons/qirtas-logo.png" width="150" alt="Qirtas">
</picture>

# Qirtas · قِرطاس

A note-taking application for Linux that tries to stay a note-taking application.

**English** · [العربية](README.md)

<sub>Zig core · GTK4 / Libadwaita · no Electron · no accounts · no AI in the editor</sub>

</div>

---

## What it is

*Qirtas* is the Arabic word for a sheet of paper.

idea: notes should remain notes.

Write your notes, organize them, read them, export them, and keep them as ordinary Markdown files that belong to you.

No subscriptions.

No accounts.

No proprietary file format.

No vendor lock-in.

If you want sync, use the service you trust. If you want backups, your files are already on your machine. If you decide to stop using Qirtas tomorrow, your notes remain ordinary Markdown files that can be opened anywhere.

Arabic support is a first-class goal of the project and continues to improve with every release.

Qirtas opens quickly, stays out of your way, and focuses on writing.

## Screenshots

<div align="center">

|                  Editor                  |                     Read mode                    |
| :--------------------------------------: | :----------------------------------------------: |
| ![Editor](assets/screenshots/editor.png) |  ![Read mode](assets/screenshots/read-mode.png)  |
|          **Paper & Ink themes**          |             **Arabic, right-to-left**            |
| ![Themes](assets/screenshots/themes.png) | ![Arabic RTL](assets/screenshots/arabic-rtl.png) |

</div>

## Install

### AppImage — easiest, any Linux, nothing to install

1. Download `Qirtas-x86_64.AppImage` from the [Releases](https://github.com/Ahmed-Sinkeat/Qirtas/releases) page.

2. Make it executable:

```sh
chmod +x Qirtas-x86_64.AppImage
```

3. Run it:

```sh
./Qirtas-x86_64.AppImage
```

No root privileges required and nothing is copied into your system.

### Arch Linux

```sh
yay -S qirtas-git
```

### From source

You'll need Zig, GTK4, Libadwaita, GtkSourceView 5, and SQLite.

```sh
git clone https://github.com/Ahmed-Sinkeat/Qirtas.git
cd Qirtas
zig build run
```

Your notes and settings live in:

```text
~/.config/qirtas/
```

## Current state

version 1.0, maintained by a single developer and used daily for real notes.

It works, but expect occasional rough edges.

Because your notes are plain Markdown files, the worst case is still just Markdown sitting on your disk.

Known issues are tracked openly in [docs/ISSUES.md](docs/ISSUES.md).

## What I plan to add

* Better Arabic writing support
* Spell checking
* Better export templates
* Improved table support
* Split view
* Mobile version
* DOCX import and export
* Plugin system

## What will not be added to the core

* AI features
* Features that do not directly improve writing, reading, or note organization

## Contributing

Start with [docs/STRUCTURE.md](docs/STRUCTURE.md).

Before proposing a feature, there is one question:

> **Does this feature make Qirtas a better note-taking application?**
>
> If the answer is yes, it may belong in the project.
>
> If the answer is no, it should probably be a plugin—or stay outside Qirtas entirely.

That question is what keeps a note-taking application focused on note-taking.

Open an issue before large changes so they can be evaluated against that principle.

---

<div align="center"><sub>GPL-3.0 · made for writing, not for platforms</sub></div>
