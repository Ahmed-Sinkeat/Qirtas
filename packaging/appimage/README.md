# AppImage build

A single portable file that runs Qirtas on most x86-64 Linux distros without
installing anything — it bundles GTK4, libadwaita, gtksourceview and their
runtime dependencies inside.

## Build

```sh
packaging/appimage/build-appimage.sh
```

Run it on a real machine (not a minimal container): it needs network access (to
fetch `linuxdeploy` + the GTK plugin), FUSE (the AppImage runtime), and the same
build toolchain as a normal build (`zig`, `pkg-config`, and the GTK4 / libadwaita
/ gtksourceview5 / sqlite development packages).

Output: `packaging/appimage/Qirtas-x86_64.AppImage`.

## Run

```sh
chmod +x Qirtas-x86_64.AppImage
./Qirtas-x86_64.AppImage
```

## How it finds its assets

The binary reads CSS themes + icons from disk. Inside the AppImage they live at
`$APPDIR/usr/share/qirtas/src/ui`. The script drops an AppRun hook
(`apprun-hooks/qirtas-data-dir.sh`) that exports
`QIRTAS_DATA_DIR=$APPDIR/usr/share/qirtas`, which `resolve_resource_path()`
checks first — so the bundled assets are found wherever the AppImage is mounted.

## Caveats (why Flatpak is usually the better bet for GTK)

- **Theming/fonts:** the GTK plugin bundles a baseline theme; host GTK themes may
  not fully apply. Qirtas ships its own CSS, so this is mostly cosmetic.
- **Wayland:** falls back to X11 if the bundled GTK and the host compositor
  disagree; usually fine via `--socket=fallback-x11` equivalents.
- **`curl`:** the GitHub *device-flow* connect shells out to `curl`. If the host
  lacks it, use the Personal Access Token path (native HTTP, always works).

For broad, reliable distribution prefer the **Flatpak**
(`packaging/org.qirtas.notebook.yml`) → Flathub. The AppImage is the
"one file, no install" option when that's specifically what you want.
