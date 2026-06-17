# Packaging Qirtas

Four ways to ship Qirtas:

| Channel | Who it's for | Files |
|---------|--------------|-------|
| **Arch (local PKGBUILD)** | build + install from a checkout | `PKGBUILD`, `qirtas.desktop` |
| **AUR** | Arch users: `yay -S qirtas-git` | `aur/PKGBUILD`, `aur/README.md` |
| **Flatpak** | any distro, via Flathub / software center | `org.qirtas.notebook.yml` |
| **AppImage** | one portable file, no install | `appimage/build-appimage.sh` |

All four install the binary plus the runtime assets under a data dir the binary
resolves via (in order) `$QIRTAS_DATA_DIR`, the build tree, then a system path —
so the same code works in every layout.

## Arch Linux (local PKGBUILD)

Build and install from a local checkout:

```sh
cd packaging
makepkg -si
```

This compiles the Zig/C source with `zig build --release=safe` and installs:

| Path | Contents |
|------|----------|
| `/usr/bin/qirtas` | the executable |
| `/usr/share/qirtas/src/ui/` | runtime assets (CSS themes + bundled icon set) |
| `/usr/share/applications/org.qirtas.notebook.desktop` | desktop entry |
| `/usr/share/icons/hicolor/512x512/apps/org.qirtas.notebook.png` | app icon |

**Dependencies:** `gtk4 gtksourceview5 libadwaita sqlite curl adwaita-icon-theme`
(plus `zig` to build).

### How the installed binary finds its assets

The binary reads CSS themes and icons from disk at runtime. `resolve_resource_path()`
(`src/gui/gui_theme.c`) and the icon-path setup (`src/gui.c`) search, in order:

1. `$QIRTAS_DATA_DIR/<rel>` — explicit override
2. `<exe>/../../<rel>` — running from the build tree (`zig-out/bin/qirtas`)
3. `<exe>/<rel>` — assets next to the binary
4. `/usr/share/qirtas/<rel>` and `/usr/local/share/qirtas/<rel>` — system install

So the installed `/usr/bin/qirtas` resolves assets from `/usr/share/qirtas/...`.
The per-theme icon-alias generation is skipped when the icons dir isn't writable
(a read-only system install) — icons still resolve via the bundled `hicolor/` set.

To test an uninstalled build against packaged assets:

```sh
QIRTAS_DATA_DIR=/usr/share/qirtas ./zig-out/bin/qirtas
```

> The PKGBUILD lives in this `packaging/` subdirectory on purpose: makepkg's
> `$srcdir` is `$startdir/src`, so a root-level PKGBUILD would collide with the
> project's own `src/` (and `makepkg -C` would delete it).

## AUR (Arch User Repository)

The easiest path for Arch users — they install with `yay -S qirtas-git`. See
`aur/README.md` for the one-time AUR account setup and the publish flow.

## Flatpak

Cross-distro, publishable to Flathub. See `org.qirtas.notebook.yml` (build
instructions are in its header comment). You must pin the Zig tarball sha256
before building.

## AppImage

A single portable file. See `appimage/README.md` and run
`appimage/build-appimage.sh` on a real machine.
