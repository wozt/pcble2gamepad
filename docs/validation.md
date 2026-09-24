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

## Passive GATT and radio capabilities

### Reference capture comparison

In the public `btle_joycon2_pairing_decrypted.pcapng` capture from
[switch2_controller_research](https://github.com/ndeadly/switch2_controller_research/tree/a3306b473acff0d6844fb1e288883a3940df0baf/captures/nrf52840),
frames 502–515 establish the link, exchange data-length/features, then select
**LE 2M in both directions** (PHY update instant 12). The first console ATT MTU
request is frame 529, followed by vendor characteristic access. The local adapter
reports LE features `bd00000000000000`: data-length extension is supported, but
LE 2M is not. Its supported states are `ffffffff00000000`, also lacking the
connection/advertising combinations required by the local Linux kernel.

The new standalone C `pcble2gamepaddiag hci0` reproduces both capability reads and
prints JSON. It uses only LE Read Local Supported Features (`0x2003`) and LE Read
Supported States (`0x201c`). This narrowly scoped read-only HCI diagnostic addresses
capabilities absent from the application's D-Bus view; the controller backend still
uses BlueZ D-Bus. No settings, firmware or NVM are written. Opening the HCI socket
can succeed without permission to send the reads, so that failure is also reported.

### Real-console test at 19:41 UTC

With no connected peers, a temporary BlueZ runtime service override loaded a copy
of the system configuration with only `ReverseServiceDiscovery=false` changed.
The original `/etc/bluetooth/main.conf` was untouched. This disables the incoming
connection GATT client in
[BlueZ 5.82 `gatt_client_init`](https://github.com/bluez/bluez/blob/5.82/src/device.c).
The test runner included automatic restoration after 180 seconds and exit cleanup.

Advertising started at `19:41:09.271485Z`; the previously identified console
connected at `19:41:09.355599Z`. During approximately 50 seconds before stopping
advertising, **no ATT packets appeared in either direction**. The earlier BlueZ
MTU and service-discovery requests were absent. L2CAP connection-parameter traffic
remained: the initial 15 ms interval changed to 30 ms, then back to 15 ms. The user
confirmed that the console still displayed only the wired controller. Later the
interval briefly changed to 10 ms and returned to 15 ms, still without ATT traffic.

Disabling reverse discovery therefore did not suffice to trigger Nintendo pairing.
Missing LE 2M is a measured difference preceding ATT in the reference capture,
but is not yet proven to be a console requirement. Host HCI captures cannot reveal
every over-the-air Link Layer exchange. A comparison using an adapter with verified
LE 2M support, or an over-the-air capture, is needed to test this hypothesis.
Changing GATT handles or implementing pairing replies would not explain the current
absence of any console ATT request.

The application advertisement/GATT registration was stopped, the identified test
peer explicitly disconnected, and the daemon terminated. The temporary profile
and service override were removed and normal BlueZ restarted. Final application
status was idle with no peers. Evidence is stored locally under
`artifacts/2026-09-22-passive-gatt/` (ignored, restricted permissions).

The four existing regression suites passed in the normal and ASan/UBSan builds.
The diagnostic was exercised on the real adapter; invalid arguments and an
unprivileged capability read were also checked. These tests do not establish
working Joy-Con pairing or input emulation.

## Second adapter: CSR comparison

> **Later Classic HID result (2026-09-24):** this section concerns the earlier
> Joy-Con 2 BLE/GATT experiment. The same CSR adapter later completed a persisted
> Switch Pro Controller Classic HID reconnect after a full backend restart, while
> the Realtek adapter failed before authentication. See the
> [Classic HID adapter comparison](classic-pro-poc.md#persistent-reconnect-validated-on-csr-2026-09-24).

On 2026-09-22 the user attached a USB `0a12:0001` dongle reporting product
`CSR8510 A10`, HCI/LMP 4.0, revision/subversion `0x22bb`. It appeared as `hci1`
at `00:1A:7D:DA:71:13`. Read-only diagnostics returned LE features
`0100000000000000` and supported states `ffffff1f00000000`: neither LE 2M nor
data-length extension is advertised. These are controller-reported identities
and capabilities, not verification of the silicon's authenticity.

The existing C daemon was run with `--adapter hci1`, normal system BlueZ settings,
and the same right-controller advertisement/GATT definitions. No driver, firmware,
system configuration or Realtek adapter settings were changed.

Advertising started at `19:49:09.913107Z`; the previously identified Switch address
connected at `19:49:09.958004Z`. HCI reported success, PC peripheral role, 15 ms
connection interval, zero latency and 2000 ms supervision timeout. Approximately
24 seconds elapsed before advertising was stopped. All observed ATT requests
originated at the PC: BlueZ's MTU request was rejected, then its discovery and GAP
reads received responses. No console-initiated ATT request, Nintendo command or
vendor GATT callback was observed. No connection-update-complete event was present
in the observation window. The user again saw only their wired controller.

This reproduces the initialization stall on a second controller implementation;
switching from Realtek to this CSR dongle alone does not resolve it. It does not
prove a mandatory LE 2M requirement, since neither adapter supports that mode.
The passive GATT experiment above was performed on Realtek only; this CSR test
used normal reverse discovery and should not be described as a passive-server test.

Advertisement/GATT registrations were removed, the selected console peer was
disconnected, and the daemon and monitor stopped. Local evidence is saved in
`artifacts/2026-09-22-csr-discovery/`, including capabilities, daemon log, HCI capture,
decoded traffic and final status. This was a hardware experiment with unchanged
application code; no new unit tests were needed.

### CSR passive server and 15 ms preference

Two further controlled tests on 2026-09-22 used temporary BlueZ runtime profiles,
with the original system configuration left untouched:

| Test | Reverse discovery | Preferred interval | Observation |
| --- | --- | --- | --- |
| CSR baseline, 19:49 UTC | Enabled | Default 30–50 ms | Console answers BlueZ; no console ATT requests |
| CSR passive, 19:51 UTC | Disabled | Default 30–50 ms | No ATT packets; PC sends L2CAP parameter request |
| CSR passive, 19:52 UTC | Disabled | 15 ms | No ATT packets and no L2CAP parameter request |

The passive test connected at `19:51:17.508225Z` and advertised until
`19:51:48.364155Z`. The 15 ms test connected at `19:52:10.604165Z`; the first link
was observed for about 47 seconds before an explicit test disconnect. Because
advertising was still active, the console immediately reconnected. Advertising
was subsequently stopped and that second link explicitly disconnected too.
Neither connection showed ATT traffic. No additional console-screen report was
received during these two tests; the result is based on captured host traffic.

The second profile added `[LE] MinConnectionInterval=12` and
`MaxConnectionInterval=12` (1.25 ms units). The capture confirms successful MGMT
Set Default System Configuration with both values `0x000c`, and a successful
incoming connection at 15 ms. Unlike the earlier passive test, Linux did not send
an L2CAP Connection Parameter Update Request. Thus this experiment actually
removed both observed host-initiated procedures; merely editing configuration
was not treated as evidence that the radio behavior changed.

Before removing the temporary override, the original interval preferences were
explicitly restored to 24/40 units. The capture confirms a successful management
command carrying `0x0018` and `0x0028`. The normal BlueZ service was then restored;
final application status was idle with no peers, and both test processes stopped.
Evidence is under `artifacts/2026-09-22-csr-passive/` and
`artifacts/2026-09-22-csr-15ms/` with restricted local permissions.

These changes do not suffice to trigger console ATT activity. Neither this result
nor the earlier comparisons proves that LE 2M is mandatory, or identifies a driver
defect. The remaining pre-ATT Link Layer exchanges are not fully observable in
these host captures. Exact GATT implementation and pairing responses remain future
work, but cannot yet be exercised by the console. Further experiments should target
an independently evidenced difference rather than repeat these settings or claim
that rewriting the Linux driver will supply missing radio capabilities.
