#!/usr/bin/env bash
# Compatibility wrapper for the legacy command. No git tag -a / git push.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 "$ROOT/tools/publish_release.py" "${1:-dist/pcble2gamepad-x86_64.AppImage}"
