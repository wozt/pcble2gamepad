# WOZT GTK Standard integration

This integration follows `wozt/wozt-gtk-standard` at Git commit
`29b6fe12e7f14b9b4e115ac27c8be36b35fc3dfd` (November 23, 2026).

The emulation, pairing, HID reports and dual Joy-Con supervision are not
modified by this visual migration. The existing input profiles remain in
`~/.config/pcble2gamepad/profiles/`. Window appearance preferences are
separate: `~/.config/pcble2gamepad/wozt-ui.ini`.

The custom GTK4 window uses `GtkWindowHandle` for moving and double-click
maximize, and GDK toplevel resize gestures for all eight edges/corners.
Both the main page and navigation adapt to narrower windows; advanced
binding and diagnostics pages remain available to preserve functionality.

Themes are read from `data/wozt/themes/` and languages from
`data/wozt/languages/`. Users can override/add JSON files in
`~/.config/pcble2gamepad/themes/` and
`~/.config/pcble2gamepad/languages/`; click the reload icon to refresh
without restarting. Missing translations fall back to English. Bundled
WUP Stream dictionaries are retained in their original stable JSON format;
they share generic UI strings but do **not** automatically translate every
pcble2gamepad-specific diagnostic or profile name. New terms should be
translated in app-specific dictionaries before claiming full localization
compliance.

## AppImage and privileged backend

Run `LINUXDEPLOY=/path/to/linuxdeploy-x86_64.AppImage bash tools/build-appimage.sh`.
The build script compiles the current source, runs hardware-safe tests and
verifies that the standard assets and desktop icon are packaged. **The
AppImage GUI cannot elevate its own unpacked helper via Polkit.** Install
`pcble2gamepad-controller-backend` and the root-owned `run-classic.sh`
with the distro package or `sudo meson install -C build` first. The GUI
then uses that trusted, system-installed privileged helper; do not grant
passwordless Polkit access to an AppImage or untrusted user-writable file.

`.github/workflows/ci.yml` builds and tests on push/PR, packages an AppImage
after successful tests, and uploads it as an artifact. Releases are **opt-in**:
configure the repo variable `ENABLE_AUTO_RELEASE=true` and protect the
`release-approval` GitHub Environment for manual approval before enabling
publishing. The release script allocates non-overwriting, four-component
tags (`v1.0.0.0`, `v1.0.0.1`, ...), attaches the AppImage and SHA-256, and
skips already-published commits. Do not enable unattended releases until
AppImage smoke tests on your target Linux systems have passed.

## Manual acceptance tests

- Launch the development binary directly; verify Alt+Tab and taskbar icon.
- Drag from blank header area; double-click to maximize/restore.
- Resize from top/bottom/left/right and all four corners.
- Resize at minimum/default/maximized sizes and toggle all seven themes.
- Switch EN -> FR -> JA -> DE -> EN while a session is live.
- Check button mapping, profiles, color pickers, diagnostics and tooltips.
- Pair two Classic Joy-Con and confirm return to HOME still works.
- Verify the AppImage starts outside the checkout, finds themes, icons and
  controller SVGs, and interacts only with the system-installed privileged
  backend.
