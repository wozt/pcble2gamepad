# Switch 1 Pro Controller over Classic Bluetooth

## Immediate objective

Validate software-only emulation on the existing Debian PC and a real Switch 2,
starting with the Switch 1 Pro Controller. Joy-Con 2 BLE work is paused; the earlier
LE 2M hypothesis was not proven. General controller-profile abstractions and
capture2cloud integration are deferred until the POC is validated.

## Reference study

Inspected source snapshots:

- [NXBT](https://github.com/Brikwerk/nxbt/tree/ec4b800ad6c55de96bb6c7f9f84b5bdc59a4c975)
- [NUXBT](https://github.com/hannahbee91/nuxbt/tree/d35693a341dce76c36a31925f8fc73c7b0d49666)
- Local JoyControl checkout `18a09da1a04306534ff9e1df8a1a69c0192a3244`.

NXBT/NUXBT advertise Classic HID through a BlueZ ProfileManager SDP record, while
owning AF_BLUETOOTH/SOCK_SEQPACKET L2CAP listeners for control PSM 17 and interrupt
PSM 19. Their controller loop emits input reports and replies to Nintendo
subcommands (device info, SPI calibration reads, mode, IMU, vibration and LEDs).
They reconnect by initiating both L2CAP channels to a known console address. The C
backend now implements that outbound path, including persisted Link Key reload through
the Bluetooth Management API. It has been validated on the CSR adapter described below.

NUXBT starts a separate default Agent1 with DisplayYesNo capability, accepting
confirmation/authorization and providing legacy PIN/passkey fallbacks. It also
configures BlueZ compatibility mode and disables plugins to avoid competing HID
listeners. Its code has no separate Switch 2 branch in the Pro Controller protocol;
this agent is general association handling, not proof of a Switch-2-only fix.

NXBT/NUXBT explicitly set the gamepad class **after** discoverability, documenting
that BlueZ can reset it otherwise. The inspected JoyControl server calls set_class
before discoverable. Its tested JOYCON_L profile also differs from this Pro profile.
Therefore the earlier lack of connection has several plausible causes; we cannot
attribute it exclusively to an agent or class-order bug without isolated tests.

## New C POC

The standalone `poc/` build reproduces the relevant transport/identity behavior:
Pro Controller alias, class `0x002508`, HID SDP, default pairing agent, incoming
control/interrupt channels, Nintendo report initialization and bounded manual input.
The protocol module has no BlueZ dependency. This adds no Python application code.
An isolated runtime service override is used rather than changing `/etc` or firmware.

## Real-console evidence, 2026-09-23

Adapter identity was rechecked after reboot: Realtek `E0:AD:47:40:70:D9` was `hci1`,
CSR `00:1A:7D:DA:71:13` was `hci0`. Existing rename changes were preserved and the
Git remote was updated to `git@github.com:wozt/pcble2gamepad.git`.

At 13:14:46 UTC the first C POC registered SDP and became discoverable as
Pro Controller with class `0x002508`. At 13:14:50.983 the default agent received
RequestAuthorization for the known console. At 13:14:51.473 both PSM 17 and 19
accepted that peer. HCI evidence includes pairing/confirmation, authentication,
encryption and Nintendo HID exchanges. **The user confirmed a Pro Controller was
visible on the real Switch 2.** No real Joy-Con/Pro Controller participated.

The initial implementation lacked the console's repeated NFC/IR configuration
subcommand `0x21`. Adding the documented NUXBT compatibility reply and restarting
the POC allowed initialization to progress. The console reconnected using the
retained bond. At 13:16:50.929 it set player LEDs to `0x03`, after enabling vibration
and selecting report mode `0x30`. These are protocol milestones, not independent
proof of working physical inputs or long-session stability.

The first launch used a terminal relay whose input did not reach the POC. Manual
input testing was moved to a private FIFO; an L+R impulse and neutral release were
logged at 13:16:55.305 and 13:16:55.815. Subsequent on-console input validation is
recorded below when available. A restart attempted before prior cleanup completed
returned BlueZ Busy; retry after cleanup succeeded. No bond was deleted.

Initial validation: four existing headless suites pass; the new wire-protocol test
passes with AddressSanitizer/UBSan. Captures remain local under
`artifacts/2026-09-23-pro-classic/`, not committed.

At 13:20:04, 13:20:06 and 13:20:08 UTC the POC sent separate 500 ms presses of
A, X and Plus with neutral releases. **The user confirmed all three on the
console's button-test screen.** This validates actual input handling, not merely
HID packet transmission. Stick validation is a separate test.

At 13:21:54–13:22:01 UTC the POC clicked the left stick, then deflected it right,
left and up with neutral releases. **The user confirmed all three directions and
correct centering on the console's stick-calibration screen.** The right stick has
wire-level coverage but was not separately confirmed on the console before the
user requested GTK integration. The connection stayed active for more than six
minutes with no spontaneous disconnect observed; this is not a long gaming-session
stress test. The user then explicitly authorized UI, keyboard mapping and direct
PC gamepad input work.

The user chose to proceed without a separate right-stick console test. Right-stick
support is implemented; its visual validation remains unperformed. After stopping,
a BlueZ Busy error occurred during one property restoration; the service was
restored normally, discovery was off, and the adapter alias read back as
`BlueZ 5.82`. The final POC now reports incomplete restoration as an error instead
of unconditionally claiming success. The runner still restores normal BlueZ.

## Dedicated bonding experiment, 2026-09-24

Fresh pairing from Change Grip/Order succeeds on Realtek, but its SSP IO Capability
exchange has the Switch request No Bonding (`0x00`). Linux follows that request in
responder role, reports `store_hint=0`, and the Switch later rejects the locally
restored key on that adapter. The later CSR comparison shows that these SSP values
alone do not determine whether reconnect succeeds.

A bounded `MGMT_OP_PAIR_DEVICE` experiment tested whether making Linux the pairing
initiator could preserve `HCI_AT_DEDICATED_BONDING`. The management command issued a
successful HCI Create Connection to `38:C6:CE:1F:B5:31`; the ACL completed with
handle 1. The Switch then terminated the ACL with `Remote User Terminated Connection
(0x13)` about 265 ms later, while Linux was reading remote features. No
Authentication Requested, IO Capability event or SSP packet occurred. The management
command completed with status `0x0e` (Disconnected).

This rules out Management Pair Device for this console flow: the Switch accepts the
page but refuses controller-initiated fresh pairing before Linux can send the
Dedicated Bonding authentication requirement. The automatic GTK path was removed;
the working Switch-initiated Pair / Sync behavior remains unchanged.

## Userspace HCI fallback assessment

A viable next experiment must retain the working direction: Switch-initiated ACL,
then answer the local HCI IO Capability Request with bonding (`0x02` or `0x04`) even
though the remote response is No Bonding. Linux cannot express that policy through
BlueZ or MGMT because `hci_get_auth_req()` deliberately follows the remote
No-Bonding value.

BTstack can run on Linux through `HCI_CHANNEL_USER`: BlueZ is stopped, the selected
HCI device is brought down, and BTstack takes exclusive protocol ownership while
the existing `btusb` transport remains loaded. This is preferable on the current
Realtek `0bda:c820` Wi-Fi/Bluetooth combination device to the libusb port, which
resets the whole USB device before claiming its Bluetooth interface and could also
disrupt the Wi-Fi interface on the same device.

Unmodified BTstack is not sufficient. In responder role its current SSP code drops
the local bonding flag after receiving a remote No-Bonding response, matching the
Linux behavior under investigation. The Switch experiment therefore needs a narrow
BTstack policy change that preserves a local Dedicated Bonding reply for this
controller profile, plus persistent TLV link-key storage. The existing Nintendo HID
report code and GTK IPC can remain above that transport. BTstack also carries a
non-commercial license, so it cannot simply be vendored into this MIT repository
without making that licensing boundary explicit or obtaining compatible terms.

The repository now contains an isolated implementation of that experiment under
`poc/btstack-pro-controller.c`. Its build script fetches BTstack commit
`e38553977a25fb0b55b383c72c289be0975f422c` outside the tracked source and applies
`btstack-preserve-local-bonding.patch`. The first hardware test proved that a local
Dedicated Bonding reply (`0x02`) is transmitted on air and creates a persistent TLV
key, but the Switch starts fresh SSP and generates a different key on the next
incoming connection. After clearing every controller on the console, leaving Change
Grip/Order and restarting the backend, controller-initiated reconnect still ended
before authentication with remote reason `0x13`.

Dedicated Bonding is the GAP pairing-only procedure and requires the paging device
to initiate authentication. This flow instead pairs while the Switch establishes
HID, so the bounded follow-up changed only the responder AuthReq to General Bonding
(`0x04`), as specified for bonding during channel establishment.

The General Bonding value was then verified on air. SSP completed and BTstack stored a new local Link Key. When Change Grip/Order
closed, the Switch disconnected with reason `0x13` and rejected four
controller-initiated pages before authentication. While the backend remained
discoverable and pairable, the Switch initiated a new SSP exchange about 23 seconds
later and generated a different Link Key. The complete HID initialization then
succeeded again. Raw keys remain only in the private capture.

This disproves the proposed userspace workaround: forcing local General Bonding
does not make the Switch retain its half of the key when its own AuthReq remains
No Bonding. The reference BTstack firmware documents the same protocol outcome.
The useful measured behavior is narrower: after a failed key-based handoff, keeping
the controller visible lets the console establish another temporary session.

## Change Grip/Order transition investigation

The initial real-console validation kept the Switch 2 on Change Grip/Order, so the
observed multi-minute connection did not validate the transition back to the HOME
menu. Historical NXBT implementations explicitly use a reduced report cadence
during this phase: 1 Hz before the first Switch response, then 15 Hz during pairing,
remaining slow until A, B or HOME is used to leave Change Grip/Order.

The current backend mirrors that transition for the Pro Controller experiment and
no longer injects an automatic L+R press. This behavior was validated on the real
Switch 2 on 2026-09-24: an A press left Change Grip/Order, the existing HID link
remained initialized, and the user confirmed that a later right, right, left, down
sequence moved the HOME cursor in that exact order. No reconnect or fresh SSP
occurred during this transition.

The inbound-pairing fallback added after rejected key-based handoff attempts was not
reached in this successful run. It remains a separate recovery path awaiting a
console-side trigger that actually closes the initial HID link.

## Controller Device ID experiment

The BlueZ capture exposed one remaining identity mismatch relevant to discovery:
the emulated controller's EIR still contained BlueZ's Linux host Device ID
(`1d6b:0246`). The reference BTstack implementation publishes the Switch Pro
Controller USB identity (`057e:2009`) in its Device ID service. The console did not
query the emulated controller's SDP during the measured pairing, so an SDP-only
record cannot correct the EIR value observed during inquiry.

The BlueZ backend now issues the standard Management `Set Device ID` command
(`0x0028`) with USB source, vendor `0x057e`, product `0x2009` and version
`0x0001` before enabling inquiry visibility. This changes volatile kernel EIR state
only; the runner's normal BlueZ restart restores the host identity. The corrected
USB identity was verified in the HCI inquiry response on air.

The durable test still failed. A fresh host-led pairing produced remote and local
No-Bonding AuthReq `0x00` and a type-4 Link Key. The initialized controller then
left Change Grip/Order through A and stayed connected in normal report mode for
about 90 seconds, sending more than 5,000 reports. After a complete backend and
BlueZ stop, the saved key was loaded successfully, but the Switch accepted the ACL
and terminated it with reason `0x13` before Authentication Requested or Link Key
Request. A separate successful Change Connection Link Key operation produced and
persisted a type-6 replacement key; the Switch rejected that key at the same
pre-authentication point. Device ID fidelity, session duration and post-pairing key
rotation therefore do not make this Switch retain the BR/EDR bond.

## Passive-pairing sequence comparison

The `cajunpanda/bluetooth-nes-advantage` BTstack implementation keeps incoming HID
PSMs at `LEVEL_0` during fresh pairing. It raises outgoing security to `LEVEL_2`
only on a later boot that already loaded the peer's Link Key. An intermediate local
experiment had instead called `gap_request_security_level(..., LEVEL_2)` as soon as
the Switch opened the initial ACL. That call has been removed. The userspace-HCI
POC now also distinguishes a key loaded at process start from one generated during
the current host-led pairing.

The aligned hardware test paired and initialized successfully, was stopped while
HID remained active, then restarted with one stored key. All four controller-led
pages were terminated by the Switch with reason `0x13` before authentication. The
Switch subsequently initiated fresh SSP and generated a new key. The reference
project documents automatic reconnect for hosts that retain a bond, but it also
states that a host advertising No Bonding keeps no key. Our capture shows that
No-Bonding case consistently on this Switch 2.

A second alignment covered the Nintendo wire profile itself: initial `0x3F`
reports at 10 Hz, transition to `0x30` only after subcommand `0x03`, firmware
`03 48` in Device Info, reference battery/vibrator fields, elapsed-time data, and
reference ACKs for NFC/IR and vibration. The Switch completed initialization, the
real A bit left Change Grip/Order, and the link remained healthy in normal cadence.
After 30 seconds outside the menu and a clean backend stop, the next page was still
terminated with `0x13` before authentication. Nintendo HID response fidelity is
therefore not what prevents this console from retaining the BR/EDR Link Key
on the Realtek adapter.

## Persistent reconnect validated on CSR, 2026-09-24

A same-machine adapter comparison changed the reconnect result without changing the
backend, Switch 2, controller profile or persisted-key procedure.

| Adapter | Identity | Fresh pairing | Outbound reconnect after full backend stop |
| --- | --- | --- | --- |
| Realtek combo | USB `0bda:c820`, `E0:AD:47:40:70:D9`, HCI manufacturer `0x005d` | Pro Controller initialization and input succeed | Switch accepts the ACL, then terminates it with `0x13` before `Authentication Requested` or `Link Key Request` |
| CSR dongle | USB `0a12:0001`, `00:1A:7D:DA:71:13`, HCI manufacturer `0x000a` | Pro Controller initialization and input succeed | Authentication, encryption, both HID channels and Nintendo initialization succeed |

The CSR fresh-pairing trace still reports remote and local
`No Bonding / no MITM (0x00)`. It produces a type-4 Link Key with management
`store_hint=0`, exactly the values that had appeared to explain the Realtek failure.
The backend nevertheless saved the key, stopped completely, restored the key on a
new process launch and initiated the connection successfully.

The CSR reconnect trace contains the complete security sequence missing on Realtek:

1. Outbound ACL connection completes and the PC becomes the peripheral role.
2. Linux sends `Authentication Requested`.
3. The controller raises `Link Key Request`; Linux replies with the restored key.
4. `Authentication Complete` returns success.
5. Encryption is enabled with E0 and a 16-byte key.
6. L2CAP control PSM 17 and interrupt PSM 19 connect.
7. The Switch repeats the Nintendo initialization exchange, enables vibration and
   sets player light 1.

The sequence succeeded on three separate backend restarts. On the final run, a B
input was sent after reconnect and the session remained initialized for more than
four minutes; its final status reported `tx=16834`, `rx=1506`,
`initialized=true`. An earlier A input selected
Change Grip/Order from the console's Controllers menu. The Switch then disconnected
the controller as that screen normally does; the user confirmed the menu transition,
so this event is not a reconnect failure.

This validates the persisted reconnect implementation and shows that `No Bonding`
and `store_hint=0` do not by themselves prevent this Switch 2 from accepting the
restored key. It also narrows the Realtek failure to behavior before authentication:
the console terminates the Realtek ACL before Linux can request or present the key.
Nintendo HID report contents, Device ID, Link Key reload and the GTK reconnect flow
are downstream of that divergence.

### What remains unresolved

The experiment correlates the failure with adapter identity, but does not yet prove
which adapter property causes it. The console sees different public Bluetooth
addresses, so a pre-existing console-side controller record for the CSR address is
a remaining confounder alongside controller firmware and timing differences. The
CSR adapter also differs in HCI/LMP implementation, USB topology and firmware from
the Realtek Wi-Fi/Bluetooth combination device.

The next investigation should compare the two HCI traces from `Create Connection`
through the first 300 ms of the ACL, especially role change, remote-feature reads,
link policy, packet type and the timing of `Authentication Requested`. A clean
console-side removal and fresh pairing of the CSR address can separate address
history from chipset behavior. If the result remains adapter-specific, capture
`btusb`/USB timing and test whether issuing authentication immediately after
Connection Complete avoids the Realtek pre-authentication `0x13`. No persistent
adapter firmware or NVM change is needed for these tests.
