# Single Joy-Con 2 over Bluetooth LE

Controller Studio has two profiles, `Nintendo Switch 2 Joy-Con 2 (L) · BLE` and
`(R) · BLE`. Each emulates one Joy-Con 2 on one adapter. This is a separate
code path. The Classic HID Pro Controller and Joy-Con pair profiles are
unchanged.

## Status

| Level | Result |
|---|---|
| Compiled | yes, `-Werror` |
| Automated tests | `tests/test-jc2.c`: GATT discovery, pairing flow with capture vectors, SPI read, report layout; also passes under ASan/UBSan |
| Local adapter | not run yet |
| Switch 2 | not validated by this project |

## Sources

- ndeadly, [switch2_controller_research](https://github.com/ndeadly/switch2_controller_research):
  GATT layout, command framing, pairing (community).
- [XD2Joy](https://github.com/desktopsetup/XD2Joy) (GPL-3.0): nRF52840 firmware
  whose author reports working left/right Joy-Con 2 emulation on a Switch 2.
  The following come from that project: the left-side UUIDs, the response to
  every command, the SPI flash regions, a neutral input report per side, and
  the need for a 5 ms connection interval. See `src/jc2-data.h`.

## Why a separate BLE stack

`pcble2gamepad-joycon2-backend` takes the adapter through an exclusive HCI
user channel. It implements L2CAP and ATT itself, because:

1. The console expects the real handle layout (`docs/protocol.md`). BlueZ
   always registers its own GAP/GATT services first.
2. The console requests a 5 ms connection interval. Linux answers LE Remote
   Connection Parameter Requests below 7.5 ms negatively in
   `hci_check_conn_params()`. XD2Joy had to patch Zephyr for the same reason:
   without the patch, the controller pairs but disconnects after leaving
   Change Grip/Order. Whether the RTL8761BUV accepts 5 ms is unverified.
3. Encryption uses the LTK derived from command `0x15` (`A1 XOR B1`). The
   backend answers the LTK request with it. BlueZ only knows SMP bonds.

While the session runs, BlueZ cannot use that adapter. When the backend
exits, the kernel hands the adapter back to BlueZ. Nothing is written to the
controller's firmware or NVM. The controller uses the adapter's public
address.

## Pairing record

After subcommand `03`, the console address and LTK are stored in
`/var/lib/pcble2gamepad/joycon2-<adapter>-<side>.key` (root, mode 0600). If
the same console connects again, its encryption request is answered with
that key. Key material and `0x15` payloads are never logged.

## Input mapping

The Studio sends a Pro Controller frame. The backend maps it for a Joy-Con
held sideways. The console rotates sideways input itself, so the backend
sends the physical button that sits at the requested on-screen position.

| Studio input | Left Joy-Con 2 | Right Joy-Con 2 |
|---|---|---|
| L / R | SL / SR | SL / SR |
| ZL / ZR | L / ZL | R / ZR |
| A, B, X, Y | Down, Left, Right, Up | X, A, Y, B |
| D-pad | D-pad | same positions as A/B/X/Y |
| Minus or Plus | Minus | Plus |
| Capture / Home | Capture | C / Home |
| Either stick | rotated stick | rotated stick |

Change Grip/Order: press L+R. These are sent as SL+SR.

## Manual run

```bash
sudo meson install -C build
sudo /usr/local/libexec/pcble2gamepad/pcble2gamepad-joycon2-backend \
  60:15:6F:CD:49:E5 --side right --verbose
```

The log shows the connection interval requested by the console, PHY, MTU,
each Nintendo command, pairing, encryption and the start of input reports.
