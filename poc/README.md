# Nintendo Classic HID controller backend

A standalone C11 backend for Switch 1 Pro Controller, Joy-Con (L) and Joy-Con (R)
identities on Switch 2. No real controller is required in that chain. The validated
path is the Pro Controller; a Joy-Con pair requires two Bluetooth adapters.

## Build and test

From the repository root, as your normal user:

```sh
meson setup build-pro poc
meson compile -C build-pro
meson test -C build-pro --print-errorlogs
```

Use `-Db_sanitize=address,undefined` for an instrumented build. Dependencies are
`libglib2.0-dev`, `libbluetooth-dev`, Meson, a C compiler and the installed BlueZ tools.

## Real-console test

Identify the adapter by address each time: indices can change after reboot.
On 2026-09-23 Realtek `E0:AD:47:40:70:D9` is `hci1`.

```sh
sudo ./poc/run-classic.sh hci1
# Add --verbose only when raw HID packet logs are needed.
```

The runner refuses existing Bluetooth links, temporarily restarts Debian's BlueZ
with `--compat --noplugin=*`, captures HCI traffic, and restores the normal service
on exit. Other Bluetooth services are unavailable during this bounded test.
The root backend opens raw L2CAP sockets and changes the volatile device class;
Controller Studio stays unprivileged and talks to it over a private Unix socket.
No firmware/NVM is modified.
The backend logs both local and remote SSP IO authentication values and the Link
Key `store_hint`. Switch 2 currently requests No Bonding, so the working Pair / Sync
session is temporary and persistent reconnect remains unresolved. Captures are
local/private because they can contain Bluetooth link keys.

### Optional userspace-HCI bonding experiment

`build-btstack.sh` builds a separate C backend against a pinned BTstack revision.
It uses BTstack's Linux `HCI_CHANNEL_USER` transport, so the kernel `btusb` driver
continues to own the USB interface while BlueZ is stopped for the session. This
avoids resetting the complete Realtek Wi-Fi/Bluetooth USB device.

```sh
./poc/build-btstack.sh
sudo ./poc/run-btstack-classic.sh hci1 --reset-bond
```

The isolated BTstack patch preserves a local General Bonding AuthReq (`0x04`)
when a Switch-initiated SSP response requests No Bonding (`0x00`). General Bonding
is the GAP procedure for pairing during HID channel establishment; the earlier
Dedicated Bonding (`0x02`) experiment produced a key locally but the console did
not retain it. Link Keys are stored with mode `0600` in
`/var/lib/pcble2gamepad/btstack/hciN.tlv`. Run the
command again without `--reset-bond` to test a controller-initiated reconnect. Use
`--passive` to test whether a console currently on Change Grip/Order resumes the
stored bond from its side. Automatic reconnect is limited to four attempts. This
path is experimental until console retention and restart reconnect are observed.

BTstack is fetched into the ignored build directory and is not vendored under this
project's MIT license. Its personal, non-commercial license is reproduced in
`BTSTACK-LICENSE`; commercial distribution requires separate terms from BlueKitchen.

The POC saves/restores adapter alias, pairability, discoverability, their timeouts,
power state and device class. It powers the selected adapter temporarily when the
compatibility-mode BlueZ restart leaves it off. It publishes the HID SDP record and
binds PSM 17 and 19 specifically to the selected adapter. Its default pairing agent accepts requests only on that
adapter, for the duration of the experiment. Visibility/pairability expire after
180 seconds and the POC stops after 10 minutes. During initial Pro Controller
pairing, periodic reports are deliberately rate-limited: one report per second
before the first Switch interrupt packet, then 15 Hz while Change Grip/Order remains
open. A, B or HOME marks the menu-exit transition; normal cadence resumes one second
later.

Open the console's **Controllers -> Change Grip/Order** screen. Initialization
logs alone do not prove buttons/sticks work: confirm those on the console.

Commands on stdin (all presses/deflections automatically release after 500 ms):

```text
buttons 40 00 40        # L + R
buttons 08 00 00        # A
buttons 04 00 00        # B
buttons 00 00 04        # D-pad right
sticks 3000 1916 2070 2013  # Left stick right, right stick centered
sticks 2159 1916 3000 2013  # Right stick right
release
status
quit
```

Enter commands without the comments. Button bytes are right/shared/left in Switch
report order, in hex. Sticks are packed 12-bit raw values, in decimal. Neutral
centers match the emulated factory calibration. Controller Studio supplies the
normal keyboard and SDL gamepad input path; these stdin commands remain useful for
protocol diagnostics.

## Scope and provenance

`protocol.c` is independent of transport. `pro-controller.c` handles BlueZ Agent1,
Profile1, L2CAP and the bounded console experiment. Tests cover device identity,
calibration reads, neutral stick packing, input fields and malformed SPI reads.

SDP XML and protocol/calibration constants are adapted from NUXBT, MIT licensed;
the licenses are retained in `NUXBT-LICENSE` and `NXBT-LICENSE`. The C implementation is new. The report
supports initialization, button/stick inputs and neutral synthetic IMU samples.
NFC/IR replies are compatibility responses, not working NFC; no rumble actuator,
real motion sensing or SPI writes are implemented. Unknown subcommands are logged.

The reference-study and measured result are in
[the Classic POC report](../docs/classic-pro-poc.md).
