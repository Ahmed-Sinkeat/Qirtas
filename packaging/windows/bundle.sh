#!/usr/bin/env bash
#
# bundle.sh — assemble a portable Windows build of Qirtas.
#
# Run this from an MSYS2 MINGW64 shell after `zig build -Dtarget=x86_64-windows-gnu`.
# It collects the executable, every mingw DLL it links against, the GTK runtime
# data (settings schemas, pixbuf loaders, icon themes) and the app's own
# resources into dist/windows/qirtas, which can be zipped and shipped as-is.
#
# The DLL set is discovered with ldd, so it tracks whatever the binary actually
# links — no hand-maintained list to drift out of date.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/dist/windows/qirtas"
EXE="$ROOT/zig-out/bin/qirtas.exe"
MINGW="${MINGW_PREFIX:-/mingw64}"

if [ ! -f "$EXE" ]; then
    echo "error: $EXE not found — run 'zig build -Dtarget=x86_64-windows-gnu' first" >&2
    exit 1
fi

echo "==> Clean output dir"
rm -rf "$OUT"
mkdir -p "$OUT"

echo "==> Copy executable"
cp "$EXE" "$OUT/"

echo "==> Copy app resources (themes, schemes, syntax, icons)"
# resolve_resource_path() and the GtkSourceView search paths both look for
# src/ui directly beside the executable, so mirror that layout here.
mkdir -p "$OUT/src"
cp -r "$ROOT/src/ui" "$OUT/src/ui"

echo "==> Copy linked DLLs (via ldd)"
# ldd prints "name => /mingw64/bin/foo.dll (0x...)"; keep the mingw ones.
ldd "$EXE" | awk '{print $3}' | grep -iE "^${MINGW}|/mingw64" | sort -u | while read -r dll; do
    if [ -f "$dll" ]; then
        cp -u "$dll" "$OUT/"
    fi
done

echo "==> GLib settings schemas"
mkdir -p "$OUT/share/glib-2.0/schemas"
if [ -f "$MINGW/share/glib-2.0/schemas/gschemas.compiled" ]; then
    cp "$MINGW/share/glib-2.0/schemas/gschemas.compiled" "$OUT/share/glib-2.0/schemas/"
else
    glib-compile-schemas "$MINGW/share/glib-2.0/schemas" \
        --targetdir "$OUT/share/glib-2.0/schemas"
fi

echo "==> GDK-Pixbuf loaders (PNG logo / icons)"
if [ -d "$MINGW/lib/gdk-pixbuf-2.0" ]; then
    mkdir -p "$OUT/lib"
    cp -r "$MINGW/lib/gdk-pixbuf-2.0" "$OUT/lib/"
fi

echo "==> Icon themes (Adwaita symbolic + hicolor)"
mkdir -p "$OUT/share/icons"
for theme in Adwaita hicolor; do
    if [ -d "$MINGW/share/icons/$theme" ]; then
        cp -r "$MINGW/share/icons/$theme" "$OUT/share/icons/"
    fi
done

echo "==> Done: $OUT"
du -sh "$OUT" 2>/dev/null || true
