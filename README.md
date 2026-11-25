# pcble2gamepad

Experimental **C11** software that emulates console controllers from a Linux PC over Bluetooth, so keyboard or SDL gamepad input can drive a real Nintendo Switch / Switch 2 without physical Nintendo controllers.

GTK4/libadwaita **Controller Studio** orchestrates privileged backend processes through private Unix sockets. No physical Nintendo controller is required in the chain.

## Controller profiles

| Profile | Transport | Status |
|---|---|---|
| Nintendo Switch Pro Controller | Classic HID (L2CAP PSM 17/19) | Validated on Switch 2 |
| Nintendo Joy-Con Pair (1 L + 1 R) | Classic HID, two adapters | Implemented; console validation in progress |
| Joy-Con 2 (L) / (R) · BLE | BLE, own HCI backend | Validated on Switch 2 |
| Joy-Con 1 (L) Classic + Joy-Con 2 (R) BLE | Mixed | Validated on Switch 2 |
| Joy-Con 2 Pair (L/R) · BLE | BLE, two adapters | Implemented |
| Joy-Con 2 (L) BLE + Joy-Con 1 (R) Classic | Mixed | Implemented |

Pair profiles require two distinct Bluetooth adapters selected by stable MAC address. The GUI sends L+R automatically during **Change Grip/Order**.

**Mouse mode** (Joy-Con 2 profiles): captures the PC mouse and routes it to the Joy-Con 2 optical sensor — left click = R, right click = ZR, wheel = stick. In the Joy-Con 2 pair, a selector chooses which half receives mouse input; in the reverse hybrid it is always the Joy-Con 2 (L).

## Controller Studio

Single-page dashboard with:

* Controller profile and adapter selection
* Pair / Sync, Reconnect and Finish actions
* Keyboard and SDL gamepad bindings, WASD/ZQSD presets
* Dead zone, sensitivity, axis inversion, stick swap, button layout
* Live SVG controller preview
* Pro Controller body/button/grip RGB customization (sent over SPI)
* Detailed HID diagnostics, per-adapter status for Joy-Con pairs
* INI profiles under `~/.config/pcble2gamepad/profiles/`

## Appearance

Built on the WOZT GTK standard (`src/wozt-ui.c`):

* 7 bundled themes, 9 languages — user JSON overrides in `~/.config/pcble2gamepad/{themes,languages}/`
* KDE taskbar integration (desktop file identity, multi-size icons)
* Preferences stored separately in `wozt-ui.ini`

## Tools

* `pcble2gamepad` — Controller Studio GUI
* `pcble2gamepadctl` — CLI status/start/stop/sync/logs/disconnect
* `pcble2gamepadd` — BLE daemon core
* `pcble2gamepad-ble-lab` — BLE session GUI
* `pcble2gamepaddiag`, `pcble2gamepad-dual-adv-diag` — adapter/BLE diagnostics
* `tools/build-appimage.sh` — AppImage build (privileged backend still requires a system install)

## Build

```sh
sudo apt install build-essential pkg-config meson ninja-build \
  libglib2.0-dev libjson-glib-dev libgtk-4-dev libadwaita-1-dev \
  librsvg2-dev libsdl2-dev libbluetooth-dev bluez dbus-daemon pkexec

meson setup build -Dgui=enabled
meson compile -C build
meson test -C build --print-errorlogs
sudo meson install -C build
```

CI runs tests on every push, builds an AppImage artifact, and can cut incremental GitHub releases.

## Architecture

```text
keyboard / SDL gamepad
        |
        v
 GTK Controller Studio  --(Unix socket)-->  Classic HID backend (pkexec)
        |                                      or Joy-Con 2 BLE backend
        v                                            |
   Switch / Switch 2  <---- Bluetooth ---- adapter(s)
```

* `src/gui.c` — Controller Studio UI
* `src/wozt-ui.c` — themes, languages, window chrome
* `src/input-model.c` — input composition and INI profiles
* `poc/pro-controller.c` + `poc/protocol.c` — Classic HID transport and Nintendo reports
* `src/jc2-backend.c` + `src/jc2.c` — Joy-Con 2 BLE backend (own L2CAP/ATT stack, proprietary pairing)
* `poc/run-classic.sh` — isolated privileged launcher (temporary BlueZ compatibility mode)

Pairing keys stay root-private under `/var/lib/pcble2gamepad/`; session logs and HCI captures live in `/tmp/pcble2gamepad-UID-XXXXXXXX` and may contain addresses — do not publish them unreviewed.

## Documentation

* [Controller Studio](docs/controller-studio.md)
* [Classic Pro Controller experiments](docs/classic-pro-poc.md)
* [Joy-Con 2 BLE](docs/joycon2-ble.md)
* [Dual Joy-Con 2 single-dongle research](docs/dual-joycon2-single-dongle.md)
* [WOZT standard](docs/wozt-standard.md)
* [Validation](docs/validation.md) · [API](docs/api.md) · [Protocol](docs/protocol.md)

## License

GNU GPL v3.0 — see [LICENSE](LICENSE).
