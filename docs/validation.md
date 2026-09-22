# Local validation — 2026-09-22

The initial checks below establish adapter behavior only. A later user-assisted
test reached a real Switch 2 connection; see the final section. Pairing and input
interoperability remain unimplemented.

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

## Questions recorded before the console test

Run the README's console test and provide daemon logs plus HCI capture. Determine:

1. Does the Switch accept the manufacturer-first AD order and the PC's public OUI?
2. Does it attempt/complete a link, and with which interval/address type/status?
3. Does it discover attributes or write cached/fixed handles? Which ATT errors occur?
4. Does the failure precede GATT, occur during initialization, or reach proprietary pairing?

Do not mark the first console milestone complete until an actual Switch attempt
is correlated with the capture. Button/stick/mouse and pairing validation follow later.

## Connection coexistence and first console link

### Registration failure at 19:04 UTC

The user's log showed GATT registration succeeding while advertisement
registration failed. A peer with Nintendo OUI `38:C6:CE` was already connected
before the daemon started. It was not evidence of a new connection during those
failed advertisement attempts. Re-enumeration also incorrectly repeated the
`peer_connected` log; that diagnostic bug is now fixed.

The failure was reproduced locally without disconnecting the peer. The system
journal exposed `Invalid Parameters (0x0d)`, hidden by the generic D-Bus error.
Passive `btmon` observation showed MGMT Add Extended Advertising Parameters
(`0054`) succeeding, then Add Extended Advertising Data (`0055`) returning status
`0d`, without a new LE Set Advertising Parameters command reaching the radio.

Read-only HCI inspection showed an existing **LE peripheral** link (PC peripheral)
and LE Supported States bytes `ff ff ff ff 00 00 00 00`. Bits above 31, including
bit 38, are absent. Linux 6.12.107's `is_advertising_allowed()` requires bits 38
and 21 for connectable advertising alongside a peripheral link. It returns false
here; `hci_enable_advertising_sync()` returns `-EINVAL`, translated to MGMT `0d`.
This is a restriction based on the controller's advertised capabilities, not a
malformed Nintendo payload or proof of an intrinsic radio limitation.

Primary sources:
[Linux 6.12.107 hci_sync.c](https://github.com/gregkh/linux/blob/v6.12.107/net/bluetooth/hci_sync.c),
[MGMT error mapping](https://github.com/gregkh/linux/blob/v6.12.107/net/bluetooth/mgmt.c),
[BlueZ 5.82 generic error response](https://github.com/bluez/bluez/blob/5.82/src/advertising.c),
and [IEEE's OUI registry](https://standards-oui.ieee.org/oui/oui.txt).
Only a standard read-supported-states command was issued for diagnosis; no raw
HCI advertising control, address change or firmware/NVM write was used.

### User-assisted Switch test at 19:30 UTC

The user fully powered off the Switch; the connected Nintendo address disappeared
from both BlueZ and the kernel connection list. Advertising then succeeded again.
On powering on and opening the controller screen, the same peer connected:

```text
2026-09-22T19:29:58.231560Z advertising_started
2026-09-22T19:30:24.409044Z peer_connected initial=false advertising_registered=true

HCI LE Connection Complete: Success (0x00)
Role: Peripheral (PC)
Peer address type: Public
Connection interval: 15.00 ms
Latency: 0
Supervision timeout: 2000 ms
```

**The first milestone is observed: the Switch accepts the advertisement and
establishes the BLE link.** The manufacturer-first AD order and PC public address
did not prevent this connection. The user reported that only their wired controller
was visible on the console; no usable Joy-Con was displayed. Link establishment
must not be reported as proprietary pairing or working input emulation.

The captured ATT traffic then came from **BlueZ's client toward the console**:
MTU 517 request (rejected as unsupported), server-feature query, primary-service
discovery, and reads of the console's GAP name and appearance (`810a`). The console
answered these requests. The PC also requested new connection parameters; the
link changed from 15 ms to 30 ms. No Nintendo command write into this application's
vendor GATT callbacks was observed in this capture. These responses are not the
console discovering our vendor services. We cannot yet attribute the initialization
stall specifically to handle mismatch, BlueZ client activity or changed timing.

Next protocol work must compare this sequence with the public pairing capture
before inventing command replies or switching transport. Exact GATT handles and
BlueZ's automatic client procedures remain questions for the next experiment.

### Recovery and regression checks

- GTK now displays peer addresses and distinguishes existing snapshot peers.
- CLI/API can disconnect exactly one explicitly selected connected address; GTK
  exposes this when only one peer is listed. Stop still does not disconnect peers.
- Failed advertisement registration rolls back the observation GATT application,
  retains the error and includes a connection-coexistence troubleshooting hint.
- A private D-Bus fixture reproduces failure with an existing peer, checks cleanup,
  duplicate snapshot logging, targeted disconnect and successful subsequent Sync.
  All four suites pass normally and under AddressSanitizer/UBSan.
- The updated GTK peer display was visually checked under Xvfb; clicking Disconnect
  peer against the isolated D-Bus fixture removed that peer, confirmed via CLI status.
- The real console test link was explicitly disconnected through the new API;
  after its asynchronous completion, Sync succeeded again. The daemon, its
  advertisement and test link were then stopped rather than left running.

Local evidence is saved under `artifacts/2026-09-22-switch-discovery/` (ignored by
Git): the btsnoop capture, decoded btmon log and daemon log. Runtime API fields
`console_verified` and `console_identity_verified` remain false: the daemon does
not automatically authenticate a peer as Nintendo or infer identity from its OUI.
The console test result above is manually correlated evidence, not a per-peer
authentication capability.
