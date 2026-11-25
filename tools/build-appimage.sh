#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
[[ "$(uname -m)" == x86_64 ]] || { echo 'Only Linux x86_64 is supported' >&2; exit 2; }
command -v meson >/dev/null || { echo 'Install Meson first' >&2; exit 2; }
command -v ninja >/dev/null || { echo 'Install Ninja first' >&2; exit 2; }
LINUXDEPLOY="${LINUXDEPLOY:-$(command -v linuxdeploy || true)}"
[[ -n "$LINUXDEPLOY" && -x "$LINUXDEPLOY" ]] || {
  echo 'Set LINUXDEPLOY=/path/to/linuxdeploy-x86_64.AppImage' >&2; exit 2;
}
BUILD_DIR="${BUILD_DIR:-build-appimage}"
[[ -f "$BUILD_DIR/build.ninja" ]] || meson setup "$BUILD_DIR" --prefix=/usr -Dgui=enabled
meson compile -C "$BUILD_DIR"
meson test -C "$BUILD_DIR" --print-errorlogs
APPDIR="$ROOT/dist/pcble2gamepad.AppDir"
rm -rf -- "$APPDIR"
mkdir -p "$APPDIR"
DESTDIR="$APPDIR" meson install -C "$BUILD_DIR" --no-rebuild
# Bundled AppImage GUI uses the separately installed privileged helper: never
# authorize a user-writable AppImage as a Polkit root command.
chmod -R a+rX "$APPDIR"
DESKTOP="$APPDIR/usr/share/applications/io.github.wozt.pcble2gamepad.desktop"
ICON="$APPDIR/usr/share/icons/hicolor/scalable/apps/io.github.wozt.pcble2gamepad.svg"
[[ -f "$DESKTOP" && -r "$ICON" ]] || { echo 'Desktop/icon assets missing' >&2; exit 1; }
for id in 01-graphite 02-light 03-ocean 04-emerald 05-rose 06-lime 07-steam-legacy; do
  [[ -r "$APPDIR/usr/share/pcble2gamepad/wozt/themes/$id.json" ]] || { echo "Missing theme $id" >&2; exit 1; }
done
for id in 01-fr 02-de 03-it 04-es 05-pt 06-ru 07-ja 08-ko 09-zh-cn; do
  [[ -r "$APPDIR/usr/share/pcble2gamepad/wozt/languages/$id.json" ]] || { echo "Missing language $id" >&2; exit 1; }
done
mkdir -p "$ROOT/dist"
# Bundle the complete GTK icon and image-loader runtime.
for theme in Adwaita hicolor; do
  [[ -r "/usr/share/icons/$theme/index.theme" ]] || { echo "Missing icon theme $theme" >&2; exit 1; }
  mkdir -p "$APPDIR/usr/share/icons/$theme"
  cp -a "/usr/share/icons/$theme/." "$APPDIR/usr/share/icons/$theme/"
done
find "$APPDIR/usr/share/icons" -name icon-theme.cache -delete
PIXBUF_LOADER_DIR="$(pkg-config --variable=gdk_pixbuf_moduledir gdk-pixbuf-2.0)"
PIXBUF_QUERY="$(pkg-config --variable=gdk_pixbuf_query_loaders gdk-pixbuf-2.0)"
[[ -x "$PIXBUF_QUERY" ]] || PIXBUF_QUERY="$(dirname "$PIXBUF_LOADER_DIR")/gdk-pixbuf-query-loaders"
[[ -x "$PIXBUF_QUERY" && -r "$PIXBUF_LOADER_DIR/libpixbufloader-svg.so" ]] || { echo 'Missing image loader runtime' >&2; exit 1; }
shopt -s nullglob
loaders=("$PIXBUF_LOADER_DIR"/*.so)
(( ${#loaders[@]} > 0 ))
libs=()
for loader in "${loaders[@]}"; do libs+=(--library "$loader"); done
"$PIXBUF_QUERY" "${loaders[@]}" | sed "s|$PIXBUF_LOADER_DIR/|@APPDIR@/usr/lib/|g" > "$APPDIR/usr/share/pcble2gamepad/pixbuf-loaders.cache.in"
grep -q '@APPDIR@/usr/lib/libpixbufloader-svg.so' "$APPDIR/usr/share/pcble2gamepad/pixbuf-loaders.cache.in"
export OUTPUT="$ROOT/dist/pcble2gamepad-x86_64.AppImage"
"$LINUXDEPLOY" --appdir "$APPDIR" --desktop-file "$DESKTOP" --icon-file "$ICON" \
  "${libs[@]}" --custom-apprun "$ROOT/tools/appimage-apprun.sh" --output appimage
for loader in "${loaders[@]}"; do
  [[ -r "$APPDIR/usr/lib/${loader##*/}" ]] || { echo "Missing bundled loader: $loader" >&2; exit 1; }
done
# Exercise the final squashfs, not the host executable or staging directory.
if [[ "${APPIMAGE_TEST_ICONS:-0}" == 1 ]]; then
  testdir="$(mktemp -d)"
  (cd "$testdir"; "$OUTPUT" --appimage-extract >/dev/null)
  env -u APPDIR -u LD_LIBRARY_PATH -u GDK_PIXBUF_MODULE_FILE -u GDK_PIXBUF_MODULEDIR \
    GTK_A11Y=none G_DEBUG=fatal-warnings GSK_RENDERER=cairo \
    xvfb-run -a "$testdir/squashfs-root/AppRun" --self-test-icons
  rm -rf "$testdir"
fi
[[ -s "$OUTPUT" ]] || { echo 'linuxdeploy did not produce the expected AppImage' >&2; exit 1; }
chmod 755 "$OUTPUT"
sha256sum "$OUTPUT" > "$OUTPUT.sha256"
echo "Created: $OUTPUT"
