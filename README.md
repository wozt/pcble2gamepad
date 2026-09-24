# pcble2gamepad

Experimental **C11** software for emulating console controllers from a Linux PC.

The current focus is Nintendo Classic Bluetooth HID. The GTK Controller Studio can emulate a **Switch Pro Controller** or a **Joy-Con pair** and route keyboard or SDL gamepad input to a real Nintendo Switch.

No physical Nintendo controller is required in the chain.

The Pro Controller path is validated on a real **Switch 2**, including pairing, input, persistent reconnect on compatible Bluetooth hardware and custom controller colors.

## Current status

### Pro Controller

Validated on a real Switch 2:

* Classic Bluetooth HID over L2CAP PSM 17/19
* Pairing from **Controllers -> Change Grip/Order**
* Nintendo initialization and report mode `0x30`
* Buttons and D-pad
* Analog stick input
* Input from Controller Studio
* Persistent Link Key storage
* Outbound reconnect after a complete backend restart using the CSR adapter
* Custom body, button, left-grip and right-grip colors
* Clean backend shutdown and BlueZ restoration

Persistent reconnect depends on the Bluetooth adapter implementation. Some adapters can complete fresh pairing but fail when the PC later initiates an authenticated reconnect.

A USB adapter based on **CSR8510 A10** (`0a12:0001`) has been validated successfully: the saved BR/EDR Link Key is restored, authentication and encryption complete, both HID channels reconnect and Nintendo initialization repeats normally.

Other adapters may disconnect before authentication even though first pairing works. Adapter compatibility should therefore be tested before relying on persistent reconnect.

See [docs/classic-pro-poc.md](docs/classic-pro-poc.md) for the detailed experiments and adapter comparison.


### Joy-Con pair

The protocol and Controller Studio support separate Joy-Con L/R identities and two Bluetooth adapters.

Wire behavior and dual-backend routing are implemented, but a complete Joy-Con pair session has not yet been validated on a real console.

### Earlier Joy-Con 2 BLE experiment

The repository also keeps the earlier Joy-Con 2 BLE discovery prototype.

A real Switch 2 successfully established an incoming BLE connection to the emulated Joy-Con 2 advertisement on 2026-09-22, but proprietary pairing and usable Joy-Con 2 input are not implemented.

This BLE path is separate from the working Classic HID Pro Controller path.

## Controller Studio

`pcble2gamepad` provides a GTK4/libadwaita interface with:

* Pro Controller or Joy-Con-pair selection
* Bluetooth adapter selection by stable MAC address
* Pair / Sync and Reconnect actions
* Keyboard bindings
* SDL gamepad bindings
* WASD and ZQSD presets
* Dead zone and sensitivity settings
* Axis inversion
* Stick swapping
* Nintendo face-button layout option
* Background gamepad input
* Live controller preview
* Detailed HID diagnostics
* Saved profiles
* Pro Controller color customization

The Pro Controller preview uses SVG artwork and supports independent RGB values for:

* body
* buttons
* left grip
* right grip

These colors are also sent to the Switch through the emulated controller SPI data.

## Profiles

All non-secret persistent Controller Studio state is stored in readable INI profiles:

```text
~/.config/pcble2gamepad/profiles/
```

Profiles include:

* bindings
* stick/input settings
* input source
* Enable input state
* preferred gamepad
* automatic gamepad selection
* Bluetooth adapter selection
* paired Switch association
* diagnostics preference
* Pro Controller colors

One profile is marked active and restored on the next launch.

The old:

```text
~/.config/pcble2gamepad/settings.ini
```

is migration-only and is removed after its values have been imported.

Bluetooth Link Keys are never stored in user profiles. They remain root-private under:

```text
/var/lib/pcble2gamepad/pairings/
```

## Pro Controller colors

Switch 2 was observed reading:

```text
0x6050 size 13
```

during controller initialization.

The relevant SPI layout is:

```text
0x6050..0x6052  body RGB
0x6053..0x6055  button RGB
0x6056..0x6058  left grip RGB
0x6059..0x605B  right grip RGB
0x605C          design variation
```

A later Switch 2 request reads 25 bytes starting at `0x603D`.

An earlier implementation accidentally allowed the factory-stick table starting at `0x603D` to extend through `0x6055`. This shadowed the body and button color bytes while leaving the grip colors intact.

That overlap is now fixed and all four configurable colors have been confirmed on the real Switch 2.

A regression test covers both the `0x6050` color read and the `0x603D` boundary.

## Build

Dependencies on Debian:

```sh
sudo apt install build-essential pkg-config meson ninja-build \
  libglib2.0-dev libjson-glib-dev libgtk-4-dev libadwaita-1-dev \
  librsvg2-dev libsdl2-dev libbluetooth-dev bluez dbus-daemon pkexec
```

Build:

```sh
meson setup build -Dgui=enabled
meson compile -C build
meson test -C build --print-errorlogs
```

Install:

```sh
meson configure build --prefix=/usr/local
sudo meson install -C build
```

The system installation provides the privileged Classic Bluetooth launcher and its narrowly scoped Polkit policy.

## Run

```sh
./build/pcble2gamepad
```

For first pairing:

1. Select **Nintendo Switch Pro Controller**.
2. Select the Bluetooth adapter.
3. Click **Pair / Sync new Switch**.
4. Open **Controllers -> Change Grip/Order** on the Switch.
5. Wait for initialization.
6. Enable input.

The Switch initiates SSP and opens HID PSM 17 and 19.

During Change Grip/Order, the backend deliberately reduces report frequency. After initialization, pressing A, B or HOME marks the transition out of that screen and normal input cadence resumes shortly afterward.

## Reconnect

After successful pairing, Controller Studio records the Switch address and adapter identity in the active profile.

The privileged backend separately saves the generated BR/EDR Link Key.

**Reconnect paired Switch** then:

1. starts the isolated Classic Bluetooth environment;
2. restores the Pro Controller identity;
3. reloads the stored Link Key;
4. creates an outbound ACL connection;
5. authenticates and enables encryption;
6. opens PSM 17 and 19;
7. repeats Nintendo initialization.

This complete sequence is validated with the CSR adapter.

## Bluetooth isolation

Controller Studio itself runs unprivileged.

For a Classic HID session it invokes the installed launcher through `pkexec`. The launcher temporarily runs BlueZ in the compatibility configuration required by the controller backend.

During the session the backend owns:

* Agent1
* HID SDP registration
* L2CAP PSM 17
* L2CAP PSM 19
* Link Key handling
* Nintendo HID reports

Normal BlueZ is restored when the session stops.

Other Bluetooth services are temporarily unavailable while the isolated session is active.

## Safety and recovery

The backend automatically returns input to neutral if updates stop for 500 ms.

If Controller Studio disappears completely, a backend lease also terminates the session so normal BlueZ can be restored.

Session logs and HCI captures are stored in a private temporary directory such as:

```text
/tmp/pcble2gamepad-UID-XXXXXXXX
```

These captures may contain Bluetooth addresses or pairing material and should not be published without inspection.

## Diagnostics

High-frequency HID traffic is hidden by default.

Enable **Detailed HID traffic** in Controller Studio when investigating protocol behavior.

Useful events include:

```text
pairing_ssp_complete
pairing_key_saved
l2cap_connected
hid_rx
hid_reply
initialization_observed
reconnect_initialized
```

## Architecture

```text
keyboard / SDL gamepad
          |
          v
 GTK Controller Studio
          |
          | private Unix socket
          v
 Classic HID backend
          |
          | Bluetooth L2CAP PSM 17/19
          v
      Nintendo Switch
```

Main components:

* `src/gui.c` — Controller Studio
* `src/input-model.c` — input composition and profiles
* `poc/pro-controller.c` — Classic Bluetooth transport and lifecycle
* `poc/protocol.c` — Nintendo Classic HID protocol
* `poc/run-classic.sh` — isolated privileged launcher
* `src/protocol.c` — earlier Joy-Con 2 BLE experiment
* `src/core.c` / `src/bluez.c` — BLE daemon lifecycle
* `src/ipc.c` / `src/client.c` — local IPC

## Documentation

* [Controller Studio](docs/controller-studio.md)
* [Classic Pro Controller experiments](docs/classic-pro-poc.md)
* [Protocol research](docs/protocol.md)
* [BlueZ notes](docs/bluez.md)
* [Validation](docs/validation.md)
* [Local API](docs/api.md)
* [Classic HID backend](poc/README.md)

## Project direction

Current priorities:

1. Keep the Pro Controller path stable.
2. Investigate the Realtek reconnect difference.
3. Validate a complete two-adapter Joy-Con pair.
4. Extend Controller Studio with additional console/controller profiles.
5. Integrate the controller backend with projects such as capture2cloud.

The earlier Joy-Con 2 BLE work remains available for future research.

## License

MIT. See [LICENSE](LICENSE).

Third-party protocol references and imported assets retain their respective attribution and licenses.
