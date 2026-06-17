#!/usr/bin/env bash
# Build a portable Qirtas-x86_64.AppImage.
#
# Run this on a real machine (needs network + FUSE). It:
#   1. builds qirtas with zig
#   2. lays out an AppDir
#   3. uses linuxdeploy + the GTK plugin to bundle GTK4/libadwaita/gtksourceview
#      and all their runtime bits (pixbuf loaders, GIO modules, schemas, icons)
#   4. emits Qirtas-x86_64.AppImage
#
# Requirements on the build host:
#   - zig (0.16.x), pkg-config, and the GTK4/libadwaita/gtksourceview/sqlite -dev
#     packages (same as a normal build)
#   - wget, FUSE (for the AppImage runtime)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."
work="$here/build"
appdir="$work/AppDir"

rm -rf "$work"
mkdir -p "$work" "$appdir"

# ── 1. Build ────────────────────────────────────────────────────────────────
echo "==> Building qirtas (zig --release=safe)"
( cd "$root" && zig build --release=safe )

# ── 2. AppDir layout ────────────────────────────────────────────────────────
echo "==> Laying out AppDir"
install -Dm755 "$root/zig-out/bin/qirtas" "$appdir/usr/bin/qirtas"

install -d "$appdir/usr/share/qirtas/src/ui"
cp -r "$root/src/ui/themes" "$appdir/usr/share/qirtas/src/ui/"
cp -r "$root/src/ui/icons"  "$appdir/usr/share/qirtas/src/ui/"
# GtkSourceView .lang + .style-scheme.xml live at src/ui top level (not themes/).
# Without them the bundled app loses syntax highlighting at runtime.
cp "$root/src/ui/qirtas_markdown.lang" "$root/src/ui"/*.style-scheme.xml \
   "$appdir/usr/share/qirtas/src/ui/"

install -Dm644 "$here/../qirtas.desktop" \
  "$appdir/usr/share/applications/org.qirtas.notebook.desktop"
# linuxdeploy wants a desktop file + icon at the AppDir root too; it copies them.
# linuxdeploy requires exactly 512x512 (the source logo is 486x319).
# Generate a square padded version once, cached alongside the script.
icon_512="$here/qirtas-512.png"
if [ ! -f "$icon_512" ]; then
    magick "$root/src/ui/icons/qirtas-logo.png" \
        -background transparent -gravity center -extent 512x512 \
        "$icon_512"
fi
install -Dm644 "$icon_512" \
  "$appdir/usr/share/icons/hicolor/512x512/apps/org.qirtas.notebook.png"

# The binary resolves assets via $QIRTAS_DATA_DIR first; point it at the bundled
# copy. linuxdeploy sources every script in AppDir/apprun-hooks/ from its AppRun.
install -d "$appdir/apprun-hooks"
cat > "$appdir/apprun-hooks/qirtas-data-dir.sh" <<'HOOK'
export QIRTAS_DATA_DIR="${APPDIR}/usr/share/qirtas"
HOOK

# ── 3. Fetch linuxdeploy + the GTK plugin ───────────────────────────────────
echo "==> Fetching linuxdeploy + GTK plugin"
cd "$work"
dl() { [ -f "$1" ] || wget -q "$2" -O "$1"; chmod +x "$1"; }
dl linuxdeploy-x86_64.AppImage \
   https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
dl linuxdeploy-plugin-gtk.sh \
   https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh

# GTK4 on Arch: gdk-pixbuf external loaders directory no longer exists.
# realpath returns empty, making copy_lib_tree receive "", causing a fatal cp
# error. Guard the call so the plugin doesn't abort.
python3 - linuxdeploy-plugin-gtk.sh <<'PATCH'
import sys
f = sys.argv[1]
src = open(f).read()
# Guard copy of pixbuf loaders dir (doesn't exist on modern Arch GTK4).
src = src.replace(
    'copy_lib_tree "$gdk_pixbuf_binarydir"',
    '[ -n "$gdk_pixbuf_binarydir" ] && [ -d "$gdk_pixbuf_binarydir" ] && copy_lib_tree "$gdk_pixbuf_binarydir"'
)
# Guard cache-file write: create parent dir first.
src = src.replace(
    '"$gdk_pixbuf_query" > "$APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}"',
    'mkdir -p "$(dirname "$APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}")" && '
    '"$gdk_pixbuf_query" > "$APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}"'
)
# Guard sed on cache file.
src = src.replace(
    'sed -i "s|$gdk_pixbuf_moduledir/||g" "$APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}"',
    '[ -f "$APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}" ] && '
    'sed -i "s|$gdk_pixbuf_moduledir/||g" "$APPDIR/${gdk_pixbuf_cache_file/$LD_GTK_LIBRARY_PATH//usr/lib}"'
)
# Guard find on non-existent gdk_pixbuf_moduledir (GTK4 Arch: loaders are built-in).
src = src.replace(
    "    done < <(find \"$directory\" -name '*.so' -print0)",
    "    done < <([ -d \"$directory\" ] && find \"$directory\" -name '*.so' -print0)"
)
open(f, 'w').write(src)
PATCH

# ── 4. Bundle + produce the AppImage ────────────────────────────────────────
echo "==> Running linuxdeploy (GTK plugin)"
export DEPLOY_GTK_VERSION=4
# linuxdeploy bundles an old binutils `strip` that chokes on RELR relocations
# (section type 0x13, .relr.dyn) emitted by modern Arch toolchains, aborting the
# build with "Unable to recognise the format of the input file". Skip stripping.
export NO_STRIP=true
./linuxdeploy-x86_64.AppImage \
  --appdir "$appdir" \
  --plugin gtk \
  --desktop-file "$appdir/usr/share/applications/org.qirtas.notebook.desktop" \
  --icon-file "$icon_512" \
  --output appimage

mv -f Qirtas*.AppImage "$here/" 2>/dev/null || mv -f ./*.AppImage "$here/"
echo "==> Done: $(ls "$here"/*.AppImage)"
