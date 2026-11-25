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
# Bundle GTK icon resources and the relocatable SVG loader.
for theme in Adwaita hicolor; do
  [[ -r "/usr/share/icons/$theme/index.theme" ]] || { echo "Missing icon theme $theme" >&2; exit 1; }
  mkdir -p "$APPDIR/usr/share/icons/$theme"
  cp -a "/usr/share/icons/$theme/." "$APPDIR/usr/share/icons/$theme/"
done
# Preserve the application icon installed by Meson and avoid stale theme caches.
find "$APPDIR/usr/share/icons" -name icon-theme.cache -delete
for icon in view-refresh-symbolic window-minimize-symbolic window-maximize-symbolic window-close-symbolic; do
  [[ -n "$(find "$APPDIR/usr/share/icons/Adwaita" -name "$icon.svg" -print -quit)" ]] || { echo "Missing symbolic icon: $icon" >&2; exit 1; }
done
PIXBUF_LOADER_DIR="$(pkg-config --variable=gdk_pixbuf_moduledir gdk-pixbuf-2.0)"
PIXBUF_QUERY="$(pkg-config --variable=gdk_pixbuf_query_loaders gdk-pixbuf-2.0)"
if [[ ! -x "$PIXBUF_QUERY" ]]; then
  PIXBUF_QUERY="$(dirname "$PIXBUF_LOADER_DIR")/gdk-pixbuf-query-loaders"
fi
SVG_LOADER="$PIXBUF_LOADER_DIR/libpixbufloader-svg.so"
[[ -r "$SVG_LOADER" && -x "$PIXBUF_QUERY" ]] || { echo 'Missing GDK-Pixbuf SVG loader/query utility' >&2; exit 1; }
mkdir -p "$APPDIR/usr/share/pcble2gamepad"
"$PIXBUF_QUERY" "$SVG_LOADER" | sed "s|$SVG_LOADER|@APPDIR@/usr/lib/libpixbufloader-svg.so|g" > "$APPDIR/usr/share/pcble2gamepad/pixbuf-loaders.cache.in"
grep -q '@APPDIR@/usr/lib/libpixbufloader-svg.so' "$APPDIR/usr/share/pcble2gamepad/pixbuf-loaders.cache.in"
export OUTPUT="$ROOT/dist/pcble2gamepad-x86_64.AppImage"
"$LINUXDEPLOY" --appdir "$APPDIR" --desktop-file "$DESKTOP" --icon-file "$ICON" \
  --library "$SVG_LOADER" --custom-apprun "$ROOT/tools/appimage-apprun.sh" --output appimage
[[ -r "$APPDIR/usr/lib/libpixbufloader-svg.so" ]] || { echo 'Bundled SVG loader missing' >&2; exit 1; }
[[ -s "$OUTPUT" ]] || { echo 'linuxdeploy did not produce the expected AppImage' >&2; exit 1; }
chmod 755 "$OUTPUT"
sha256sum "$OUTPUT" > "$OUTPUT.sha256"
echo "Created: $OUTPUT"
