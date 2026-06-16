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

install -Dm644 "$here/../qirtas.desktop" \
  "$appdir/usr/share/applications/org.qirtas.notebook.desktop"
# linuxdeploy wants a desktop file + icon at the AppDir root too; it copies them.
install -Dm644 "$root/src/ui/icons/qirtas-logo.png" \
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

# ── 4. Bundle + produce the AppImage ────────────────────────────────────────
echo "==> Running linuxdeploy (GTK plugin)"
export DEPLOY_GTK_VERSION=4
./linuxdeploy-x86_64.AppImage \
  --appdir "$appdir" \
  --plugin gtk \
  --desktop-file "$appdir/usr/share/applications/org.qirtas.notebook.desktop" \
  --icon-file "$appdir/usr/share/icons/hicolor/512x512/apps/org.qirtas.notebook.png" \
  --output appimage

mv -f Qirtas*.AppImage "$here/" 2>/dev/null || mv -f ./*.AppImage "$here/"
echo "==> Done: $(ls "$here"/*.AppImage)"
