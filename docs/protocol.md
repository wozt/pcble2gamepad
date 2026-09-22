# Joy-Con 2 R protocol research

Evidence labels: **community** means documented by reverse engineering;
**capture** means independently inspected in the public PCAP named below;
**local** means observed on the development adapter, not on a Switch.
No console interoperability has been established for this emulator.

## Sources inspected

Research snapshot: [ndeadly a3306b4](https://github.com/ndeadly/switch2_controller_research/tree/a3306b473acff0d6844fb1e288883a3940df0baf).
Read `bluetooth_interface.md`, `descriptors.md`, `hid_reports.md`, command framing
and the pairing/feature-selection sections of `commands.md`. Inspected advertisement,
pairing, reconnect and mouse PCAPs in `captures/nrf52840/` with Wireshark/tshark.

Secondary implementations:

- [joycon2-connector 82d0495](https://github.com/Misaka10571/joycon2-connector/tree/82d04952999b64d7e1ed463ce20219e25ccc51f9):
  `DeviceManager.h` identifies Nintendo advertisements, while `JoyConDecoder.cpp`
  decodes mouse and controller state from real devices. Its optical coordinates
  use signed little-endian values at offsets `0x10..0x13` in its chosen buffer.
- [joycon2pc 27332b9](https://github.com/FingerlessCoder/joycon2pc/tree/27332b9ab5164e3c64408600649d6a98a7a2f315):
  `NS2InputReportDecoder.cs` has its own prefix handling, button and stick offsets
  and calibration assumptions. These are not interchangeable with raw report
  `0x08` below. Do not transplant buffer offsets without identifying the report
  type and transport framing first.
- The [Reddit nRF52 prototype](https://www.reddit.com/r/switch2/comments/1w69red/connecting_a_bluetooth_mouse_to_the_switch_2_w/)
  is an independent direction of investigation, not reproducible source evidence
  or proof that a standard Linux adapter can implement the same link behavior.

## Exact observed discovery advertisement

**Capture:** `btle_joycon2_advertise.pcapng`, frame 1, timestamp
2025-06-28 19:10:26.098162 UTC. Right-controller identity is corroborated by
`descriptors.md`'s JoyCon 2 (R) USB descriptor: VID `0x057e`, PID `0x2066`.
Left PID is `0x2067`; `0x2069` in the general advertisement example is Pro Controller.

| Field | Observed value |
| --- | --- |
| Primary PHY | LE 1M |
| Advertising access address | `0x8e89bed6` |
| PDU | `ADV_IND`, connectable undirected, scannable |
| Header bytes | `00 25`: PDU type 0, TxAdd public, length 37 |
| AdvA | `48:F1:EB:2D:36:8F` in this capture, public address |
| AdvA wire byte order | `8f 36 2d eb f1 48` |
| AdvData length | 31 bytes (the full legacy budget) |
| Flags AD | `02 01 06` = general discoverable + BR/EDR unsupported |
| Manufacturer AD | length `1b`, type `ff`, 26 value bytes |
| Name, advertised service UUIDs, appearance, TX power | Absent |
| First two captured packet times | 18.750 ms apart; this alone does not establish the configured interval |

Complete AdvData:

```text
02 01 06
1b ff 53 05 01 00 03 7e 05 66 20 00 01 00 00 00 00 00 00 00 0f 00 00 00 00 00 00 00
```

Manufacturer offsets below start at `53`, not the AD length/type:

| Offset | Size | Meaning / value |
| --- | --- | --- |
| `00` | 2 | Bluetooth company ID `0x0553`, little endian `53 05` |
| `02` | 3 | Unknown fixed bytes `01 00 03` |
| `05` | 2 | Nintendo vendor ID `0x057e`, `7e 05` |
| `07` | 2 | Right product ID `0x2066`, `66 20` |
| `09` | 2 | Unknown fixed bytes `00 01` |
| `0b` | 1 | `00` discovery/reconnect; community wake example uses `81` |
| `0c` | 6 | Host address reversed; all zero during discovery |
| `12` | 1 | Unknown fixed `0f` |
| `13` | 7 | Reserved zero |

BlueZ `ManufacturerData` maps company ID `0x0553` to the remaining **24 bytes**,
not to all 26 bytes. The adapter's own manufacturer ID `0x005d` (Realtek) is a
separate HCI hardware property; it does not need to become Nintendo. The emulator
keeps the PC adapter's real public address; no Nintendo OUI or identity is copied.
Address/OUI filtering by the console remains an open question for the hardware test.

## Expected GATT database

**Community:** `bluetooth_interface.md`, Joy-Con R rows. Handles below belong to a
real controller. Characteristic declarations occupy the preceding odd/even slots
as implied by each value handle; the table lists value handles explicitly.
CCCD UUID `2902` uses the standard Bluetooth base UUID.

| Handle/range | Kind / properties | UUID | Purpose |
| --- | --- | --- | --- |
| `0001–0007` | Primary service | `00c5af5d-1964-4e30-8f51-1956f96bd280` | Control |
| `0003` | Read | `00c5af5d-1964-4e30-8f51-1956f96bd281` | Unknown control information |
| `0005` | Write | `00c5af5d-1964-4e30-8f51-1956f96bd282` | Unknown control write |
| `0007` | Read | `00c5af5d-1964-4e30-8f51-1956f96bd283` | Unknown control value |
| `0008–002a` | Primary service | `ab7de9be-89fe-49ad-828f-118f09df7fd0` | Reports and commands |
| `000a` | Read, notify | `ab7de9be-89fe-49ad-828f-118f09df7fd2` | Input report 05 |
| `000b` | Descriptor | `2902` | CCCD |
| `000c` | Descriptor | `679d5510-5a24-4dee-9557-95df80486ecb` | Report rate? |
| `000e` | Read, notify | `d5a9e01e-2ffc-4cca-b20c-8b67142bf442` | Right input report 08 |
| `000f` | Descriptor | `2902` | CCCD |
| `0010` | Descriptor | `679d5510-5a24-4dee-9557-95df80486ecb` | Report rate? |
| `0012` | Write without response | `fa19b0fb-cd1f-46a7-84a1-bbb09e00c149` | Rumble |
| `0014` | Write without response | `649d4ac9-8eb7-4e6c-af44-1ea54fe5f005` | Command |
| `0016` | Write without response | `65a724b3-f1e7-4a61-8078-a342376b27ff` | Rumble + command |
| `0018` | Write without response | `4147423d-fdae-4df7-a4f7-d23e5df59f8d` | Large/firmware command |
| `001a` | Notify | `c765a961-d9d8-4d36-a20a-5315b111836a` | Basic command response |
| `001b` | Descriptor | `2902` | CCCD |
| `001c` | Descriptor | `b746df8c-f358-495b-9cd2-e3bbeda4f979` | Unknown |
| `001e` | Notify | `640ca58e-0e88-410c-a7f3-426faf2b690b` | Extended right response |
| `001f` | Descriptor | `2902` | CCCD |
| `0020` | Descriptor | `b746df8c-f358-495b-9cd2-e3bbeda4f979` | Unknown |
| `0022` | Notify | `d3bd69d2-841c-4241-ab15-f86f406d2a80` | Unknown input |
| `0023` | Descriptor | `2902` | CCCD |
| `0024` | Descriptor | `b746df8c-f358-495b-9cd2-e3bbeda4f979` | Unknown |
| `0026` | Read, notify | `ab7de9be-89fe-49ad-828f-118f09df7fde` | Unknown input |
| `0027` | Descriptor | `2902` | CCCD |
| `0028` | Descriptor | `679d5510-5a24-4dee-9557-95df80486ecb` | Report rate? |
| `002a` | Write without response | `ab7de9be-89fe-49ad-828f-118f09df7fdf` | Unknown output |
| `002b–002f` | Primary service | `1800` | Generic Access |
| `002d` | Read | `2a00` | Device Name |
| `002f` | Read | `2a01` | Appearance |
| `0030` | Empty primary service | `1801` | Generic Attribute |

The Markdown does not specify all descriptor permissions/values. The probe exposes
vendor descriptors as read/write for observation, rejects unimplemented reads and
acknowledges logged writes without applying protocol state. This is an explicit
approximation, not a recovered exact descriptor implementation. BlueZ provides its
own standard GAP/GATT services, name and appearance; the probe does not pretend to
control their layout. There are no Pro Controller audio attributes in the R schema.

## What the pairing capture adds

**Capture:** `btle_joycon2_pairing_decrypted.pcapng`:

- Frame 502: `CONNECT_IND`, public initiator/responder addresses, initial interval
  **15 ms**, latency 0, supervision timeout 2000 ms. The community observation of
  a later 5 ms interval must not be confused with this initial connection interval.
- Frame 536: control read-by-type response includes `04 00 05 00 01 01 00`.
  The prototype's first control read returns this sample. Its meaning is unknown.
- Frame 537 writes `01 00` to `0005`. Frame 544 returns an eight-byte value for
  the second control read; it is not blindly replayed because its semantics are unknown.
- Frames 545/549/553 write `01 00` to `001b/001f/0023` CCCDs. Frames 557 onward
  issue commands directly to `0016`. This is evidence that handle fidelity matters;
  UUID equivalence alone must not be assumed sufficient.
- Frame 573: command `15 91 01 01 00 0e 00 00`, then address-exchange data.
  Frames 577/583/587 perform subcommands `04`, `02`, `03` respectively.
- Frame 757 writes `85 00` to vendor descriptor `0010`; frame 761 enables `000f`.
  This adds concrete descriptor-write evidence beyond the UUID table.

The rumble+command characteristic carries a 17-byte prefix, then an 8-byte command
header. Header fields are command ID, direction (`91` request / `01` response),
transport (`01` BLE), subcommand, unknown byte, data length/request or ACK/response,
and two reserved bytes. Command-only `0014` starts directly with that header.
The prototype checks truncation and logs pairing stage names, but never fabricates
an ACK notification or a successful pairing result.

## Proprietary pairing and reconnect

**Community**, corroborated by the sequence above:

1. Command `15`, subcommand `01`: exchange the console's two host addresses and
   the controller address, using the exact command payload framing.
2. Subcommand `04`: exchange A1/B1. Wire payloads include a leading selector byte;
   the 16-byte values must not be confused with the entire 17-byte payload.
3. Compute `LTK = A1 XOR B1`. The documented controller B1 is
   `5cf6ee792cdf05e1ba2b6325c41a5f10` in the example's representation.
4. Subcommand `02`: for the documented wire arrays, calculate
   `AES-128-ECB(key=reverse(LTK), plaintext=reverse(A2))` to obtain B2.
   Reversals are essential. Build capture-derived test vectors before implementing.
5. Subcommand `03`: finalize host/key storage. No SMP pairing is substituted.

Reconnect/wake advertisements put a host address into manufacturer offsets `0c..11`;
wake also uses byte `0b=81`. The inspected reconnect capture begins regular
initialization commands at frame 86 and does not repeat the same plaintext key
exchange sequence. Link encryption and key installation are a separate backend
problem; implementing AES alone cannot solve it. Do not persist host keys until
these flows and byte orders have dedicated tests. See [BlueZ limits](bluez.md).

## Mouse remains a required capability

**Community:** report `08` on right characteristic `000e` includes controller and
mouse fields together, not separate mutually exclusive operating modes:

| Offset | Size | Field |
| --- | --- | --- |
| `00` | 1 | Sequence counter |
| `01` | 1 | Power state |
| `02` | 2 | Buttons |
| `04` | 1 | Unknown, often `07` |
| `05` | 3 | Packed 12-bit stick coordinates |
| `08` | 1 | Unknown |
| `09` | 2 | Mouse delta X |
| `0b` | 2 | Mouse delta Y |
| `0d` | 1 | Mouse extra field, possibly lift-off distance |
| `0e` | 1 | NFC state |
| `0f` | 1 | Motion data length |
| `10` | up to 40 | Packed motion data |
| `38` | 7 | Reserved in the documented 63-byte layout |

Button byte 0 from low to high bit: B, A, Y, X, R, ZR, Plus, Stick.
Byte 1: Home bit 0, C bit 4, SR bit 6, SL bit 7. Capture belongs to the left
controller. Feature bit 4 enables mouse data; bit 2 enables motion data.

**Capture:** `btle_joycon2_mouse_mode_decrypted.pcapng`, frame 2368 begins a 63-byte
notification on `000e`; later samples include variable motion length (30/40) while
preserving the shared controller/mouse layout. This confirms that a future encoder
must update buttons/stick and mouse deltas in the same state/report pipeline.
Mouse sign, scaling, accumulation, overflow and the extra field need capture-based
validation. Connector-side absolute touch mapping must not be assumed to be the
console's native relative mouse semantics.

## Reproduce the capture inspection

With an external checkout of the research snapshot and `tshark` installed:

```sh
tshark -r captures/nrf52840/btle_joycon2_advertise.pcapng -c 1 -V
tshark -r captures/nrf52840/btle_joycon2_pairing_decrypted.pcapng \
  -Y 'btatt.opcode == 0x52 || btatt.opcode == 0x12' \
  -T fields -e frame.number -e btatt.handle -e btatt.value
tshark -r captures/nrf52840/btle_joycon2_mouse_mode_decrypted.pcapng \
  -Y 'btatt.opcode == 0x1b && btatt.handle == 0x000e' \
  -T fields -e frame.number -e btatt.value
```

Public captures are referenced, not redistributed in this repository.
