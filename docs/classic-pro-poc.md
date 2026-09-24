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
They can reconnect by initiating both L2CAP channels to a known console address.
The POC currently accepts incoming reconnects; outbound reconnect is not implemented.

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

Fresh pairing from Change Grip/Order succeeds, but its SSP IO Capability exchange
has the Switch request No Bonding (`0x00`). Linux follows that request in responder
role, reports `store_hint=0`, and the Switch later rejects the locally restored key.

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
`btstack-preserve-local-bonding.patch`. The patch changes only the responder AuthReq:
it sends local Dedicated Bonding (`0x02`) while leaving BTstack's internal dedicated
procedure flag clear, because that internal flag would disconnect after pairing.
The first hardware test must verify the actual HCI IO Capability Reply before any
claim about durable Switch pairing is made.

## Change Grip/Order transition investigation

The initial real-console validation kept the Switch 2 on Change Grip/Order, so the
observed multi-minute connection did not validate the transition back to the HOME
menu. Historical NXBT implementations explicitly use a reduced report cadence
during this phase: 1 Hz before the first Switch response, then 15 Hz during pairing,
remaining slow until A, B or HOME is used to leave Change Grip/Order.

The current backend mirrors that transition for the Pro Controller experiment and
no longer injects an automatic L+R press. This is an experimental compatibility
change and requires real-console validation.
