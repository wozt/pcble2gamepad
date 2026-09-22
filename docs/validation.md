# Local validation — 2026-09-22

These checks do **not** establish Switch 2 interoperability.

## Environment

- Debian 13, Linux `6.12.107+deb13-amd64`, BlueZ `5.82`.
- Realtek USB `hci0`, public address `E0:AD:47:40:70:D9`, hardware manufacturer `0x005d`.
- Five advertising instances, 31-byte legacy advertisement and scan-response limits.
- GCC 14.2, GLib 2.84.4, GTK 4.18.6, libadwaita 1.7.6, JSON-GLib 1.10.6.

## Adapter checks

The daemon ran as the desktop user using the system D-Bus. Only passive `btmon`
monitoring required sudo. GATT registration and advertisement registration both
succeeded. `status` moved from `idle` through `transitioning` to `advertising`,
with `gatt_registered=true` and `advertising_registered=true`.

Initial capture showed BlueZ's default advertising interval of 1280 ms. Explicit
MinInterval/MaxInterval properties were then added. A second HCI capture confirmed:

```text
LE Set Advertising Parameters:
  Min advertising interval: 20.000 msec (0x0020)
  Max advertising interval: 20.000 msec (0x0020)
  Type: Connectable undirected - ADV_IND (0x00)
  Own address type: Public (0x00)
  Channel map: 37, 38, 39
  Filter policy: Allow Scan Request from Any, Allow Connect Request from Any
Command Complete: Success
LE Set Advertise Enable: Enabled
Command Complete: Success
```

The first capture's LE Set Advertising Data command contained 31 bytes with these
AD structures in **BlueZ's order** (manufacturer first):

```text
1b ff 53 05 01 00 03 7e 05 66 20 00 01 00 00 00 00 00 00 00 0f 00 00 00 00 00 00 00
02 01 06
```

There was no scan-response payload. The next start reused the controller's already
configured advertising data, so the interval-validation capture did not need to
repeat LE Set Advertising Data. These observations establish what the host asked
the adapter to transmit and the controller's successful command responses. They
are not an independent over-the-air reception test.

Stopping produced successful LE advertising disable, Remove Advertising and GATT
unregistration. After daemon exit `bluetoothctl show` reported ActiveInstances=0.
The adapter's public address and global discoverability were unchanged. No
connection to a Switch was observed during these short local registration checks.

## Automated and UI checks

- Meson builds daemon, CLI, GTK application and C tests with `-Wall -Wextra -Wpedantic -Werror`.
- Protocol tests compare full advertisement bytes to a captured fixture, validate
  a captured pairing-command offset and truncated frames, and check R GATT identities.
- Core tests reject malformed API requests, serialize lifecycle changes and validate
  bounded log retention/cursors.
- IPC integration exercises start/status/sync/logs/stop, unknown-method errors,
  duplicate daemon exclusion, permissions, graceful cleanup and stale-socket recovery.
- All three suites pass in the normal build and with AddressSanitizer plus
  UndefinedBehaviorSanitizer (`-Db_sanitize=address,undefined`). The malformed-JSON
  tests caught an empty-root edge case, which was fixed before the final run.
- A separate `-Dgui=disabled` build succeeds without GTK dependencies in its targets.
- GTK application launched under Xvfb, connected to the daemon and displayed adapter,
  state and live logs. A screenshot was inspected. KDE's existing GTK configuration
  emitted theme/settings warnings; no application crash was observed.
- GTK Start/Stop buttons were clicked under Xvfb against a mock daemon; independent
  CLI status queries confirmed advertising state became true, then false.

## Still required on hardware

Run the README's console test and provide daemon logs plus HCI capture. Determine:

1. Does the Switch accept the manufacturer-first AD order and the PC's public OUI?
2. Does it attempt/complete a link, and with which interval/address type/status?
3. Does it discover attributes or write cached/fixed handles? Which ATT errors occur?
4. Does the failure precede GATT, occur during initialization, or reach proprietary pairing?

Do not mark the first console milestone complete until an actual Switch attempt
is correlated with the capture. Button/stick/mouse and pairing validation follow later.
