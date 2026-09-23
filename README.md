# pcble2gamepad

Experimental **C11** software for emulating console controllers from a Linux PC.

The current application is a software-only **Switch 1 Pro Controller over Classic
Bluetooth HID**, tested against a real Switch 2. Its GTK Controller Studio maps a
keyboard or a PC gamepad to the emulated controller. No real Joy-Con or Pro
Controller is required in the chain. Detailed console evidence and limitations are
in [the experiment report](docs/classic-pro-poc.md).

Controller Studio includes remappable keyboard and SDL gamepad bindings, saved
profiles, radial stick dead zones, sensitivity, axis inversion, stick swapping,
live input preview and session diagnostics. The Bluetooth backend stays in C and
is controlled through a private Unix socket. Keyboard input has been exercised
end to end against the backend simulation. A physical source gamepad remains to
be tested when one is available.

## Earlier Joy-Con 2 BLE work

The daemon, CLI and `pcble2gamepad-ble-lab` retain the earlier Joy-Con 2 discovery
experiment. This path is separate from Controller Studio.

The first milestone is deliberately small: advertise one **Joy-Con 2 R** and
observe whether a real Switch 2 attempts to connect. **This milestone was observed
on the user's console on 2026-09-22: an incoming BLE connection completed successfully.**
This is not a working controller or a completed pairing implementation; the console
does not yet display a usable Joy-Con.

## Earlier BLE status

Implemented:

- BlueZ D-Bus peripheral advertisement using the observed Nintendo manufacturer
  data, right-controller PID `0x2066`, and general-discoverable flags.
- Two vendor GATT services, 14 characteristics and six vendor descriptors for
  observation. BlueZ creates the six CCCDs. This is an approximation of the real
  controller's database, with different handles and BlueZ's standard services.
- Connection/property observation, raw GATT reads/writes, notification subscription
  logs and basic Nintendo command-header diagnostics.
- Headless daemon, documented Unix socket JSON API, C CLI, GTK4/libadwaita GUI,
  peer addresses and explicit targeted disconnection for repeatable tests.
- An explicit mock backend for local lifecycle/UI tests without radio activity.

Locally checked on Debian 13 / BlueZ 5.82 / Realtek `hci0`: GATT registration,
connectable advertising, and clean removal work. HCI monitoring confirms a public
address and the expected manufacturer payload and flags. **BlueZ reorders the AD
structures**; the tested console nevertheless completed a connection. See
[local validation](docs/validation.md). Nintendo pairing, buttons, sticks, mouse
reports, IMU, rumble and the proprietary reconnect procedure remain unimplemented.

## Build

```sh
sudo apt install build-essential pkg-config meson ninja-build \
  libglib2.0-dev libjson-glib-dev libgtk-4-dev libadwaita-1-dev \
  libsdl2-dev libbluetooth-dev bluez dbus-daemon pkexec
meson setup build -Dgui=enabled
meson compile -C build
meson test -C build --print-errorlogs
```

For a headless-only build, omit the GTK development packages and use
`meson setup build-headless -Dgui=disabled`. The daemon and CLI never link GTK.
There is no Python application code or runtime dependency; Meson is a build tool.
For optional installation in your user account, configure the prefix first:

```sh
meson configure build --prefix="$HOME/.local"
meson install -C build
```

## Run Controller Studio

```sh
./build/pcble2gamepad
```

Choose the Bluetooth adapter by address and click **Connect to Switch**. The
application invokes its narrow backend with `pkexec`, temporarily restarts BlueZ
in Classic HID compatibility mode, and restores the normal service when the
session stops. This pauses other Bluetooth services for the duration. On first
pairing, open **Controllers -> Change Grip/Order** on the console.

Select **Keyboard** or **PC controller**, configure the corresponding bindings,
then enable input. Escape immediately pauses keyboard input. The backend returns
buttons and sticks to neutral if updates stop for 500 ms. Closing Controller
Studio stops the session and restores BlueZ. Profiles are INI files under
`~/.config/pcble2gamepad/profiles`.

The session stores a private HCI capture and backend log under
`/var/lib/pcble2gamepad/UID` when installed, or `artifacts/` when run from a
checkout. These files can contain Bluetooth addresses and pairing material.
See [Controller Studio](docs/controller-studio.md) for the UI and architecture.

## Run the earlier BLE lab

Start the daemon as your regular desktop user, in one terminal:

```sh
./build/pcble2gamepadd --adapter hci0 --verbose 2>&1 | tee daemon.log
```

In a second terminal, open the BLE lab or use the CLI:

```sh
./build/pcble2gamepad-ble-lab
# Or:
./build/pcble2gamepadctl status
./build/pcble2gamepadctl sync
./build/pcble2gamepadctl status
./build/pcble2gamepadctl logs
./build/pcble2gamepadctl stop
```

The BLE lab is a client of the running daemon. Closing it does not stop the daemon.
`start` and `sync` currently both request standard discovery advertising; `sync`
**does not perform Nintendo pairing**. Start/stop are asynchronous: an initial
`transitioning` response means the request was accepted, not that registration has
succeeded. Query `status` again and inspect `error`.

The adapter must already be powered and its global discoverability must be off.
The daemon checks this and does not change these settings automatically. Inspect
with `bluetoothctl show`; if needed, explicitly use `bluetoothctl power on` and
`bluetoothctl discoverable off`. Do not use `bluetoothctl pair` for this protocol.

No automatic advertising, adapter address rewriting, firmware/NVM writes, bond
creation, Bluetooth service restart, or raw HCI control is performed. Stop removes
this application's advertisement and GATT registration. Exiting the daemon closes
its private D-Bus connection, allowing BlueZ to release its registrations, including
after abnormal process termination. Other adapter connections are not disconnected.

The separate C diagnostic reads adapter capabilities using only two fixed HCI
read commands; it does not advertise, disconnect peers or change adapter settings:

```sh
sudo ./build/pcble2gamepaddiag hci0
```

It emits JSON with the raw LE feature/state bytes, LE 2M and data-length support,
and the connection/advertising combinations checked by Linux 6.12. The local
Realtek adapter lacks LE 2M. The reference Joy-Con pairing capture switches to 2M
before ATT, but this does not establish that the console requires 2M. See the
[passive GATT experiment](docs/validation.md#passive-gatt-and-radio-capabilities).

For mock mode:

```sh
./build/pcble2gamepadd --mock
```

The default socket is `$XDG_RUNTIME_DIR/pcble2gamepad/control.sock`. Use
`PCBLE2GAMEPAD_SOCKET` for all three programs or `--socket PATH` for daemon/CLI.
A custom socket's parent must be a private directory owned by you (mode `0700`).
The socket is `0600`, checks peer credentials, and accepts only the daemon's UID.

## First console test and debugging

1. Start a passive HCI capture **before** starting advertising:

   ```sh
   sudo btmon -i hci0 -w switch2-discovery.btsnoop | tee btmon.log
   ```

2. Start the daemon with `--verbose`, then open the Switch 2 controller pairing
   screen and run `pcble2gamepadctl sync` (or click **Sync**).
3. Leave discovery active for about 30 seconds. Save CLI `status`, `logs`, the daemon
   log, and the HCI capture. Record the exact console screen and visible behavior.
4. Run `pcble2gamepadctl stop`, then stop the daemon and `btmon` with Ctrl+C.

A successful registration is only a Linux-side check. A peer connection event is
not proof that the peer is a Switch; correlate its address and timing with the
console test. Failed attempts before BlueZ creates a `Device1` object, ATT discovery,
connection interval, encryption transitions and HCI disconnect reasons require
`btmon`. They are **not available through all of the D-Bus callbacks**. Raw ATT
traffic is especially important because the console may use fixed handles that
land in BlueZ's existing database instead of our exported characteristics.

`journalctl -u bluetooth --since '5 minutes ago'` can add BlueZ errors. If D-Bus
returns `AccessDenied`, check the distribution's Bluetooth access policy and user
groups rather than running the GUI as root. Captures and verbose command logs may
contain peer addresses and, in future pairing tests, key material: redact those
before publishing. Use `btmon -r switch2-discovery.btsnoop` to decode a saved capture.

### Advertising fails after a previous connection

An existing BLE link can prevent this Realtek adapter from advertising another
connectable instance. Locally, this produced BlueZ's generic `Failed to register
advertisement` with management `Invalid Parameters (0x0d)`. Five advertising
instances do not guarantee simultaneous connection/advertising support.

Use `status` or GTK's peer address display to identify the link. A
`peer_already_connected` event is a snapshot, not a new console connection.
Switching the console fully off and observing the peer disappear can identify it;
a Nintendo address prefix alone cannot. After identifying the intended test peer:

```sh
./build/pcble2gamepadctl stop
# Wait until status is no longer transitioning, then select the exact test peer:
./build/pcble2gamepadctl disconnect AA:BB:CC:DD:EE:FF
# Wait for completion, then restart discovery:
./build/pcble2gamepadctl sync
```

GTK offers **Disconnect peer** when exactly one peer address is available. For
multiple peers, use the CLI with an explicit address. Nothing automatically
disconnects another Bluetooth device. A failed advertising registration now rolls
back the probe GATT application and preserves the diagnostic. Detailed evidence:
[connection coexistence investigation](docs/validation.md#connection-coexistence-and-first-console-link).

## Architecture and API

```text
keyboard / gamepad -> Controller Studio -> private input API -> Classic HID backend

BLE lab / C CLI -----------------------> discovery API -> Joy-Con 2 BLE daemon
```

- `poc/protocol.*`: Switch 1 Pro Controller reports and subcommand responses.
- `poc/pro-controller.c`: Agent1, SDP, L2CAP transport and input watchdog.
- `src/gui.c` and `src/input-model.*`: Controller Studio and input composition.
- `src/protocol.*`: observed Joy-Con 2 bytes and vendor GATT schema.
- `src/core.*`: lifecycle, status, bounded structured event history and API dispatch.
- `src/bluez.*`: D-Bus objects, advertisement/GATT registration, adapter events.
- `src/ipc.*`: bounded asynchronous local control transport.
- `src/client.*`: shared unprivileged client code.

[Controller Studio](docs/controller-studio.md) · [API specification](docs/api.md) · [Protocol research](docs/protocol.md) ·
[BlueZ feasibility](docs/bluez.md) · [Validation](docs/validation.md).

## Joy-Con 2 roadmap

1. Discovery and an incoming connection on a real Switch 2: **observed 2026-09-22**.
2. Measure ATT/handle behavior; retain BlueZ where it works, document any blocker.
3. Implement exact GATT behavior and Nintendo proprietary pairing, then initialization.
4. Add buttons, both sticks, Home/Capture/C, stick clicks and side buttons as applicable.
5. **Keep Joy-Con 2 R mouse data and buttons/stick in the same report. Mouse support
   is a required goal**, not an optional replacement for controller input.
6. Add reconnect, persisted host pairing, then Joy-Con 2 L and dual controllers.
   Two adapters are acceptable; five advertising instances do not imply five identities.
7. Add input test controls, IMU/gyro and rumble where understood, then capture2cloud.
   Replacing capture2cloud's Titan One is a future integration, not this milestone.

## References

- [ndeadly/switch2_controller_research](https://github.com/ndeadly/switch2_controller_research):
  primary community research, wire formats and captures.
- [Misaka10571/joycon2-connector](https://github.com/Misaka10571/joycon2-connector)
  and [FingerlessCoder/joycon2pc](https://github.com/FingerlessCoder/joycon2pc):
  real-controller-to-PC implementations; their direction is the opposite of this project.
- [BlueZ source and API documentation](https://github.com/bluez/bluez/tree/master/doc).
- [Independent nRF52 prototype report](https://www.reddit.com/r/switch2/comments/1w69red/connecting_a_bluetooth_mouse_to_the_switch_2_w/):
  contextual evidence only; no dependency on private firmware or an author's reply.

Community reverse-engineering findings are provisional until reproduced locally.
The code is original; reference repositories are not vendored. Project source,
comments, diagnostics, UI and documentation are written in English.

License: [MIT](LICENSE).
