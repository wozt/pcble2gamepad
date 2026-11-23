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
# linuxdeploy may require the GTK plugin for a fully portable GTK4 runtime.
# Keep plugin provisioning explicit rather than silently declaring success.
export OUTPUT="$ROOT/dist/pcble2gamepad-x86_64.AppImage"
"$LINUXDEPLOY" --appdir "$APPDIR" --desktop-file "$DESKTOP" --icon-file "$ICON" --output appimage
[[ -s "$OUTPUT" ]] || { echo 'linuxdeploy did not produce the expected AppImage' >&2; exit 1; }
chmod 755 "$OUTPUT"
sha256sum "$OUTPUT" > "$OUTPUT.sha256"
echo "Created: $OUTPUT"
