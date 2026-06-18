#!/usr/bin/env bash
# Build a portable Qirtas-x86_64.AppImage.
#
# Run this on a real machine (needs network + FUSE). It:
#   1. builds qirtas with zig
#   2. lays out an AppDir
#   3. uses linuxdeploy + the GTK plugin to bundle GTK4/libadwaita/gtksourceview
#      and all their runtime bits (pixbuf loaders, GIO modules, schemas, icons)
#   4. manually bundles libharfbuzz (excluded by linuxdeploy's built-in list but
#      required at the version the bundled GTK was linked against)
#   5. emits Qirtas-x86_64.AppImage
#
# Requirements on the build host:
#   - zig (0.16.x), pkg-config, and the GTK4/libadwaita/gtksourceview/sqlite -dev
#     packages (same as a normal build)
#   - wget, FUSE (for the AppImage runtime)
#   - ImageMagick (magick) for icon resizing
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

# ── Package icon (used for AppImage thumbnail / desktop integration) ─────────
# qirtas-package-icon.png is the Arabic-calligraphy app icon supplied for
# packaging. The in-app logo (src/ui/icons/qirtas-logo.png) is intentionally
# left untouched.
# linuxdeploy requires exactly 512x512 for the --icon-file argument.
icon_512="$here/qirtas-512.png"
package_src="$here/qirtas-package-icon.png"
if [ -f "$package_src" ]; then
    # Resize the supplied package icon to 512x512 (it is 1254x1254).
    magick "$package_src" -resize 512x512^ -gravity center -extent 512x512 "$icon_512"
else
    # Fallback: pad the in-app logo if the package icon is missing.
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
# linuxdeploy's GTK plugin hard-forces GDK_BACKEND=x11 (a generic workaround for
# an old GTK Wayland crash). On modern GTK4 under Wayland that shoves the app
# onto XWayland, where GtkPopover menus (dropdowns, right-click) fail to grab and
# never open. Prefer Wayland, fall back to x11. This hook is sourced AFTER the
# GTK plugin hook, so it overrides the forced value.
export GDK_BACKEND=wayland,x11
HOOK

# ── 3. Fetch linuxdeploy, GTK plugin, and AppImageKit runtime ───────────────
echo "==> Fetching linuxdeploy + GTK plugin + AppImageKit runtime"
cd "$work"
dl() { [ -f "$1" ] || wget -q "$2" -O "$1"; chmod +x "$1"; }
dl linuxdeploy-x86_64.AppImage \
   https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
dl linuxdeploy-plugin-gtk.sh \
   https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh
# Download the classic AppImageKit runtime (~190 KB). The newer type2-runtime
# (~944 KB, downloaded automatically by appimagetool) is incompatible with
# AppImageLauncher's binfmt-bypass: the bypass binary was built against the
# smaller format and segfaults (exit 11) when it encounters the larger one.
dl runtime-x86_64 \
   https://github.com/AppImage/AppImageKit/releases/download/continuous/runtime-x86_64

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

# ── 4. Deploy into AppDir (GTK plugin, no packing yet) ──────────────────────
echo "==> Running linuxdeploy deploy phase (GTK plugin, no AppImage output yet)"
export DEPLOY_GTK_VERSION=4
# linuxdeploy bundles an old binutils `strip` that chokes on RELR relocations
# (section type 0x13, .relr.dyn) emitted by modern Arch toolchains, aborting the
# build with "Unable to recognise the format of the input file". Skip stripping.
export NO_STRIP=true
./linuxdeploy-x86_64.AppImage \
  --appdir "$appdir" \
  --plugin gtk \
  --desktop-file "$appdir/usr/share/applications/org.qirtas.notebook.desktop" \
  --icon-file "$icon_512"
# NOTE: --output appimage is intentionally omitted here; we pack below after
# manually injecting libraries that linuxdeploy's built-in excludelist skips.

# ── 5. Bundle libharfbuzz before packing ────────────────────────────────────
# linuxdeploy's excludelist deliberately skips libharfbuzz (it's listed as a
# "system" lib). On modern Arch (harfbuzz 14+) the symbols differ from what the
# bundled GTK/Pango were linked against at build time, causing a SIGSEGV in
# ld-linux during relocation (exit code 11). We must inject the exact harfbuzz
# that matches the bundled GTK *before* the squashfs is packed.
echo "==> Bundling libharfbuzz into AppDir (crash-fix for modern Arch harfbuzz 14+)"

bundle_lib() {
    local lib="$1"
    local real
    real="$(readlink -f "$lib")"
    local dest="$appdir/usr/lib/$(basename "$real")"
    if [ ! -f "$dest" ]; then
        cp "$real" "$dest"
        # Patch rpath so the bundled lib finds its siblings via $ORIGIN
        patchelf --set-rpath '$ORIGIN' "$dest" 2>/dev/null || true
    fi
    # Ensure the versioned soname symlink exists (e.g. libharfbuzz.so.0 -> libharfbuzz.so.0.61420.0)
    local soname
    soname="$(objdump -p "$real" 2>/dev/null | awk '/SONAME/{print $2}' | head -1)"
    if [ -n "$soname" ] && [ "$soname" != "$(basename "$real")" ] \
       && [ ! -e "$appdir/usr/lib/$soname" ]; then
        ln -sf "$(basename "$real")" "$appdir/usr/lib/$soname"
    fi
}

for lib in \
    /usr/lib/libharfbuzz.so.0 \
    /usr/lib/libharfbuzz-subset.so.0 \
    /usr/lib/libgraphite2.so.3 \
    /usr/lib/libfreetype.so.6 \
    /usr/lib/libfribidi.so.0 \
    /usr/lib/libfontconfig.so.1 \
; do
    [ -f "$lib" ] && bundle_lib "$lib" && echo "  bundled $(basename "$lib")"
done

# ── 6. Patch AppRun: fix squashfuse FUSE mmap crash ─────────────────────────
# squashfuse (used by AppImageLauncher's binfmt-bypass) does not support
# mmap(PROT_EXEC|MAP_SHARED) on files inside the FUSE mount. When the dynamic
# linker tries to mmap the qirtas ELF segments it gets SIGBUS/SIGSEGV (exit 11).
# Fix: AppRun copies the binary to a real tmpfs before exec-ing it.
echo "==> Patching AppRun (squashfuse FUSE mmap workaround)"
cat > "$appdir/AppRun" << 'APPRUN'
#! /usr/bin/env bash

# autogenerated by linuxdeploy, patched for squashfuse FUSE mmap compatibility

# make sure errors in sourced scripts will cause this script to stop
set -e

this_dir="$(readlink -f "$(dirname "$0")")"

source "$this_dir"/apprun-hooks/"linuxdeploy-plugin-gtk.sh"
source "$this_dir"/apprun-hooks/"qirtas-data-dir.sh"

# squashfuse does not support mmap(PROT_EXEC|MAP_SHARED) for binaries inside
# the FUSE mount, causing a SIGSEGV (exit 11) at dynamic linker startup.
# Detect FUSE mount and copy the binary to a real tmpfs before exec.
_bin="$this_dir/usr/bin/qirtas"
if [[ -n "$APPIMAGE" ]] || [[ "$this_dir" == /tmp/.mount_* ]] || \
   { stat -f -c%T "$_bin" 2>/dev/null | grep -q fuse; }; then
    _tmp_bin="$(mktemp /tmp/qirtas-XXXXXX)"
    cp "$_bin" "$_tmp_bin"
    chmod +x "$_tmp_bin"
    trap "rm -f '$_tmp_bin'" EXIT
    exec "$_tmp_bin" "$@"
else
    exec "$_bin" "$@"
fi
APPRUN
chmod +x "$appdir/AppRun"

# ── 6b. Create .DirIcon at AppDir root ──────────────────────────────────────
# AppImage spec requires .DirIcon at the root. appimagetool normally creates it
# during the output phase, but we pack squashfs by hand (below) and never run
# appimagetool. Without .DirIcon, AppImageLauncher's libappimage registration
# aborts: "appimage_register_in_system : Entry doesn't exists: .DirIcon".
# linuxdeploy's deploy phase already placed the top-level icon symlink; point
# .DirIcon at the bundled hicolor icon, which always exists.
echo "==> Creating .DirIcon (AppImageLauncher registration fix)"
ln -sf usr/share/icons/hicolor/512x512/apps/org.qirtas.notebook.png "$appdir/.DirIcon"

# ── 7. Repack squashfs with gzip and assemble AppImage ──────────────────────
# linuxdeploy's embedded appimagetool downloads the new type2-runtime (~944KB)
# and packs with zstd compression. Both are incompatible with the version of
# AppImageLauncher's binfmt-bypass built for Arch. Instead we:
#   a) manually pack squashfs with gzip (AppImageKit runtime supports it)
#   b) prepend the classic AppImageKit runtime (~188KB, same as LosslessCut)
#      which AppImageLauncher was built and tested against.
echo "==> Repacking squashfs with gzip compression"
squashfs_out="$work/qirtas.squashfs"
mksquashfs "$appdir" "$squashfs_out" -comp gzip -noappend 2>&1 | tail -3

echo "==> Assembling AppImage (AppImageKit runtime + gzip squashfs)"
runtime="$work/runtime-x86_64"
python3 - "$squashfs_out" "$runtime" "$here/Qirtas-x86_64.AppImage" << 'PY'
import sys, os
sq = open(sys.argv[1], 'rb').read()
rt = open(sys.argv[2], 'rb').read()
out = rt + sq
with open(sys.argv[3], 'wb') as f:
    f.write(out)
os.chmod(sys.argv[3], 0o755)
sq_hdr = out[len(rt):len(rt)+4]
print(f"  Runtime: {len(rt)} bytes, squashfs: {len(sq)//1024//1024}MB, header: {sq_hdr}")
PY

echo "==> Done: $(ls -lh "$here"/*.AppImage)"


