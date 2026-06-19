# Building Qirtas on Windows

Qirtas is a GTK4 app, so the Windows build uses the MSYS2 MINGW64 toolchain —
that's where the gnu-ABI GTK4, GtkSourceView, libadwaita and SQLite packages
live. The Zig core is the same code as on Linux; the platform-specific bits
(file paths, randomness, the machine-bound key, the executable lookup) are
selected at compile time with `builtin.os.tag`, so there's nothing to configure.

## Prerequisites

1. Install [MSYS2](https://www.msys2.org/).
2. Open the **MINGW64** shell (not the plain MSYS or UCRT64 shell) and install
   the runtime stack:

   ```bash
   pacman -S --needed \
     mingw-w64-x86_64-gtk4 \
     mingw-w64-x86_64-gtksourceview5 \
     mingw-w64-x86_64-libadwaita \
     mingw-w64-x86_64-sqlite3 \
     mingw-w64-x86_64-pkgconf
   ```

3. Install Zig 0.16.0 and make sure `zig` is on the PATH the MINGW64 shell sees
   (the project pins `minimum_zig_version = "0.16.0"`).

## Build

From the repo root, inside the MINGW64 shell:

```bash
zig build -Dtarget=x86_64-windows-gnu -Doptimize=ReleaseSmall
```

The `-Dtarget=...-gnu` matters: it pins the gnu ABI so the toolchain matches the
MSYS2 mingw libraries. The msvc ABI expects a different GTK build and won't link.
The binary lands at `zig-out/bin/qirtas.exe`.

`zig build` shells out to `pkgconf` to find GTK's include and link flags, which
is why this has to run inside the MINGW64 shell — that's where the `.pc` files
point at the mingw GTK install.

## Run from the build tree

`qirtas.exe` finds its resources (themes, syntax files, icons) relative to the
executable, so running it straight from `zig-out/bin` while the repo is checked
out works — it walks up to `src/ui`. For a clean run, point it explicitly:

```bash
QIRTAS_DATA_DIR="$(pwd)" ./zig-out/bin/qirtas.exe
```

## Package a portable build

`packaging/windows/bundle.sh` collects the exe, every mingw DLL it links
against (discovered with `ldd`, so the list never goes stale), the GTK runtime
data (settings schemas, pixbuf loaders, icon themes) and the app's own
resources into `dist/windows/qirtas`:

```bash
bash packaging/windows/bundle.sh
```

Zip that folder and it runs on a machine with no MSYS2 installed. CI does
exactly this on every push — see `.github/workflows/windows.yml` — and uploads
the result as the `qirtas-windows-x86_64` artifact, which is the easiest way to
get a build without a local Windows setup.

## What's different on Windows

- **Config / data** live under `%APPDATA%\qirtas` (`vault.db`, settings),
  matching `~/.config/qirtas` on Linux.
- **The at-rest encryption key** is machine-bound. On Linux that binding comes
  from `/etc/machine-id`; on Windows it's the registry `MachineGuid`
  (`HKLM\SOFTWARE\Microsoft\Cryptography`). Same idea: an encrypted vault copied
  to a different machine won't decrypt. There's no recovery if the MachineGuid
  changes (OS reinstall) — same as on Linux.
- **Browser-based sign-in** for cloud sync opens the default browser with
  `cmd /c start` instead of `xdg-open`.

## Known gaps

- **No live file-watching yet.** On Linux an inotify thread reloads a note when
  another program changes it on disk and refreshes the file explorer. That's
  Linux-only right now; on Windows the editor still works, it just won't notice
  external changes until you reopen. The replacement is a `ReadDirectoryChangesW`
  backend (tracked in [`PORTABILITY.md`](PORTABILITY.md)).
- **No installer.** The output is a portable folder/zip. An NSIS or Inno Setup
  installer can wrap it later.
