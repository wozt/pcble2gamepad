# Dual Joy-Con 2 BLE Emulation on a Single Dongle

## Objective

Emulate two independent Switch 2 Joy-Con 2 controllers (LEFT `057e:2067`,
RIGHT `057e:2066`) over the native Joy-Con 2 BLE protocol, simultaneously,
through one TP-Link Bluetooth 6.0 dongle. This is not Joy-Con 1 Classic HID.

## Hardware

| Item | Value |
|---|---|
| Adapter | TP-Link UB600, USB `37ad:0600`, Realtek RTL8761BUV |
| Address | `60:15:6F:CD:49:E5` (select by address; the hciN index changes) |
| Host | Debian 13, kernel `6.12.113+deb13-amd64`, BlueZ 5.82, `rtl8761bu_fw.bin` |
| BlueZ report | `SupportedInstances: 4`, secondary channels 1M/2M/Coded, `MaxAdvLen: 251` |

The same adapter runs the Classic Pro Controller emulation against a real
Switch 2. That path is the regression baseline and must keep working.

## Evidence levels

Each result below is tagged with one level. They are never merged.

1. **Compiled** - code builds with `-Werror`.
2. **Automated test** - `meson test` passes.
3. **HCI command success** - the local controller accepted the commands.
4. **Externally observed** - an independent scanner or central confirmed the radio behaviour.
5. **Switch 2 validated** - a real console accepted the behaviour.

## Progress matrix (2026-11-01)

| Feature | Implementation | Evidence | Blocker |
|---|---|---|---|
| Hardware capability diagnostics | `pcble2gamepaddiag`, and capability report in `pcble2gamepad-dual-adv-diag` | Compiled | Not yet run on the TP-Link; LE features/states not recorded |
| LE 2M verification | `--le-2m` requests LE Set PHY per link and logs PHY Update Complete | Compiled | Hardware run pending |
| Two advertising sets | Ext. adv sets 0/1, legacy `ADV_IND`, exclusive HCI user channel | Compiled | Hardware run pending |
| Two distinct BLE identities | Per-set static random address (`LE Set Advertising Set Random Address`) | Compiled | Needs external scanner confirmation |
| Two simultaneous connections | Probe maps each link to its set via Advertising Set Terminated | Compiled | Needs two centrals connected at once |
| LEFT advertisement | Manufacturer data with PID `0x2067` in the diagnostic only | Compiled | Not in daemon |
| RIGHT advertisement | Daemon (BlueZ, single identity) and diagnostic | Switch 2 connected (2026-10-31), no sync | Pairing |
| GATT layout and routing | Single RIGHT reference layout over BlueZ, observation only | Switch 2 read observed | Per-identity routing not possible with one BlueZ GATT DB (see below) |
| Nintendo proprietary pairing | `pairing.c`: LTK = A1 XOR B1, B2 = AES-128 with byte reversal, strict parsers | Automated test (capture vector) | No state machine, no responses wired |
| Encryption/key installation | None | - | Needs a backend that owns LTK replies |
| Input reports | None for Joy-Con 2 | - | Pairing |
| GTK integration | None for Joy-Con 2 pair | - | Backend |
| Real Switch 2 validation | Only the single-identity discovery milestone | Switch 2 connected, no controller shown | Everything above |

## History of this effort

- `27cc994`, `026414d`, `7d2648f` (Devin session): added the dual advertising
  diagnostic, an MGMT Set PHY call, a placeholder Nintendo ACK and the pairing
  crypto module. The diagnostic used wrong HCI opcodes (0x43-0x47 are not the
  extended advertising commands) and raced bluetoothd on the raw channel. The
  ACK was never sent and the PHY call did not change Switch behaviour.
- A later uncommitted session fixed the opcodes, moved the diagnostic to the
  HCI user channel, made the pairing parsers strict and added capture test
  vectors. It also started a "multi-instance" layer in `bluez.c`/`core.c`
  that did not compile and had memory-safety bugs. That layer was set aside
  (patch kept outside the repository at
  `~/pcble2gamepad-wip-backup-2026-11-01/multi-instance-scaffolding.patch`)
  because a BlueZ-based design cannot provide the required topology (below).
- `baf431e`: committed the fixes and removed the MGMT PHY and ACK code.
- Current step: the dual-advertising diagnostic became a connection-concurrency
  probe.

## Why the daemon cannot simply run two instances through BlueZ

These are documented properties, not yet hardware-demonstrated failures:

- `LEAdvertisement1` and MGMT Add Extended Advertising have no per-instance
  address parameter. Distinct per-instance addresses only appear with LE
  privacy, as RPAs that rotate and share one IRK. A Switch that stores the
  controller address after proprietary pairing cannot reconnect to a rotating
  RPA.
- BlueZ exposes one GATT database per adapter. LEFT and RIGHT would share
  attributes and notification fan-out. Writes carry a `device` option, but
  notifications through `PropertiesChanged` reach every subscribed link.
- Nintendo pairing derives the LTK in-band. The controller then starts
  encryption with that LTK. BlueZ answers LTK requests only from its own SMP
  key store, so the derived key would have to be injected through MGMT Load
  Long Term Keys for an address BlueZ never paired with. This is unverified.

AGENTS.md requires a demonstrated limitation before raw HCI. The probe is
that demonstration for the controller side. If it shows two concurrent links
on distinct static identities, the next backend is a userspace
HCI-user-channel peripheral. It would own both identities, a minimal ATT
server per link, and LTK replies from the Nintendo pairing result. BlueZ
then does not use this adapter while the Joy-Con 2 pair runs, so the Pro
Controller cannot share the TP-Link at the same time.

## Connection-concurrency probe

`pcble2gamepad-dual-adv-diag [--le-2m] [--timeout SECONDS] <hciN|address>`

It runs on the HCI user channel with no kernel or BlueZ involvement. It:

- resets the controller, then enables LE events;
- prints LE features (DLE, 2M, Coded, extended advertising) and the state
  bits from `docs/validation.md`;
- advertises LEFT and RIGHT with fresh static random addresses;
- logs each LE link, the advertising set that accepted it, and connection,
  data-length and PHY updates;
- answers ATT with MTU 23 and "not found" errors, rejects SMP pairing and
  answers LTK requests negatively, so a generic central does not time out;
- prints the peak link count and whether LEFT and RIGHT links were up at the
  same time;
- disables and clears the sets and resets the controller on exit.

No firmware, NVM or persistent identity is touched.

### Run procedure

Stop any Pro Controller emulation that uses the TP-Link first.

```bash
cd /home/wozt/dev/pcble2gamepad
printf 'select 60:15:6F:CD:49:E5\npower off\n' | bluetoothctl
sudo ./build/pcble2gamepad-dual-adv-diag --le-2m 60:15:6F:CD:49:E5 \
  | tee ~/dual-adv-probe-$(date +%F).log
# On a phone (nRF Connect): scan, connect to the LEFT address, then from a
# second phone or computer connect to the RIGHT address. Keep both open for
# at least 60 s. Then press Enter in the probe terminal.
printf 'select 60:15:6F:CD:49:E5\npower on\n' | bluetoothctl
```

One phone can usually hold several connections in nRF Connect. That is
enough to test the peripheral side. Record what the central displays: both
addresses, both connected, and the PHY if shown. Do not commit the log if it
contains console or phone addresses.

### Results

None yet. Fill in: LE features/states lines, both addresses seen by the
scanner (yes/no), peak links, "observed/not observed" for concurrent
identities, PHY per link and disconnect reasons.

## Next implementation step

1. The user runs the probe and records the results above.
2. If concurrency is observed: start `src/hci-user.c`, a transport
   independent of the Joy-Con protocol code. It would own the user channel,
   the identities, ACL and the per-link ATT server, and serve the reference
   GATT layout from `protocol.c` per identity. Then wire the `0x15`
   pairing state machine on top of `pairing.c` and answer LTK requests with
   the derived key.
3. If it is not observed: document the controller limitation with the probe
   log. Two dongles remain a fallback only, never the silent default.

## Recovery

If the probe is killed hard, the adapter may stay in user-channel state until
the process exits. Then power it back on through `bluetoothctl` as above. If
needed, use `sudo systemctl restart bluetooth`, or unplug and replug the dongle.
