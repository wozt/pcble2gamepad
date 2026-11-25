#!/usr/bin/env bash
set -euo pipefail
export APPDIR="${APPDIR:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)}"
export PATH="$APPDIR/usr/bin:$PATH"
export LD_LIBRARY_PATH="$APPDIR/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export XDG_DATA_DIRS="$APPDIR/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export PCBLE2GAMEPAD_BUNDLED_ICONS="$APPDIR/usr/share/icons"
cache="$(mktemp "${XDG_RUNTIME_DIR:-/tmp}/pcble2gamepad-pixbuf.XXXXXXXX")"
trap 'rm -f -- "$cache"' EXIT
# Escape sed replacement characters in the actual mount path.
escaped="$(printf '%s' "$APPDIR" | sed 's/[\&|]/\\&/g')"
sed "s|@APPDIR@|$escaped|g" "$APPDIR/usr/share/pcble2gamepad/pixbuf-loaders.cache.in" > "$cache"
export GDK_PIXBUF_MODULE_FILE="$cache"
export GDK_PIXBUF_MODULEDIR="$APPDIR/usr/lib"
"$APPDIR/usr/bin/pcble2gamepad" "$@" &
child=$!
trap 'kill -TERM "$child" 2>/dev/null || true' TERM
trap 'kill -INT "$child" 2>/dev/null || true' INT
wait "$child"
