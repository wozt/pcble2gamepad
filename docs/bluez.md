# BlueZ feasibility and boundaries

Start with the normal userspace D-Bus APIs. No raw-HCI emulator, address spoofing,
Bluetooth service replacement, firmware writes or NVM commands are included.

Reference API/source snapshot:
[BlueZ f26da43](https://github.com/bluez/bluez/tree/f26da4389e2dc9dac340182b5f618a63c0c96c08),
particularly `doc/org.bluez.LEAdvertisement.rst`, `org.bluez.GattService.rst`,
`org.bluez.GattCharacteristic.rst`, `org.bluez.GattDescriptor.rst`,
`src/advertising.c`, `src/gatt-database.c`, and the management API documentation.
This source snapshot is newer than installed BlueZ 5.82; actual adapter behavior
was separately checked in [local validation](validation.md).

## What D-Bus can reproduce

| Requirement | D-Bus support / prototype decision |
| --- | --- |
| Connectable legacy advertising | `LEAdvertisement1.Type=peripheral`; locally produces `ADV_IND` |
| Nintendo manufacturer bytes | `ManufacturerData[0x0553]`, 24 bytes following company ID |
| General discovery flags | Per-advertisement `Discoverable=true`, timeout zero; global adapter discoverability off |
| No extra AD fields | Omit LocalName, ServiceUUIDs, Appearance and TX power; Includes is empty |
| Advertising interval | Request MinInterval=MaxInterval=20 ms, verified in HCI commands |
| Vendor GATT services/characteristics | GattManager1 application with ObjectManager, exact vendor UUIDs |
| Read/write visibility | GattCharacteristic1/GattDescriptor1 callbacks, including BlueZ device/offset/MTU options when supplied |
| CCCD notifications | BlueZ owns CCCDs; StartNotify/StopNotify expose subscription transitions, not raw per-peer CCCD writes |
| Peer links | Device1 Connected and InterfacesAdded/Removed; may include unrelated adapter peers |
| Stop/cleanup | Unregister advertisement and GATT application; private bus owner exit releases registrations |

Flags generation is a BlueZ policy choice: `set_flags()` in `advertising.c` adds
BR/EDR Not Supported when the adapter is not globally discoverable while this
advertisement is. The backend refuses startup on a globally discoverable adapter
rather than changing shared settings behind the user's back. It does not disable
BR/EDR or disturb other Bluetooth peripherals.

## Known mismatches and limitations

### Advertising byte order and own address

BlueZ 5.82 emitted manufacturer data **before** Flags. Both AD structures, values,
length and PDU type match the reference, but byte-for-byte ordering does not.
Standard AD parsing is order-independent; whether Nintendo's filter accepts this
ordering is **unverified**. D-Bus offers structured advertisement data rather than
full PDU ownership. Do not claim an exact replay.

The backend keeps the real adapter identity. D-Bus does not offer a per-advertisement
public identity override; privacy and address selection are controlled by BlueZ and
the kernel. Local HCI parameters showed Public, which is separate evidence from
Adapter1.AddressType. If privacy is enabled elsewhere, inspect the actual HCI
parameters again. Advertising instances are not independent controller identities.

### Attribute handles and standard services

BlueZ manages a shared ATT database, including GAP/GATT and other applications.
Its `Handle` properties request allocation and may fail on conflicts; they do not
replace occupied handles. The real Joy-Con begins its vendor service at `0001`,
while BlueZ already uses low handles for its own services. Registering duplicate
GAP/GATT services would not remove these built-ins or make the real controller's
empty `1801` service appear at `0030`.

The probe intentionally uses automatic allocation and labels logged handles
`reference_handle`, never pretending they are actual BlueZ handles. Registration
succeeded locally. The exact assigned on-air database needs a remote ATT discovery
capture. The public Switch capture writes to `0005`, `001b`, `001f`, `0023`, `0016`
without a complete ordinary rediscovery in the inspected sequence. This makes
hardcoded/cached handle use a serious compatibility risk. It is a supported
hypothesis, not proof about every firmware version or a fresh unpaired console.

If our Switch writes to the wrong attributes, stop adding protocol workarounds:
record the actual ATT requests and errors, then evaluate a backend that owns the
ATT database. Management APIs alone do not replace BlueZ's GATT database.

### Pairing and encryption

BlueZ's normal pairing/agent APIs implement SMP. Nintendo's proprietary command
exchange produces an LTK outside SMP; neither marking a Device1 trusted nor calling
Pair implements this flow. The daemon registers no pairing agent, performs no
Device1.Pair, requests no encrypted GATT flags and writes no bond files.

Management `Load Long Term Keys` can load host-side keys, but peer address types,
role, EDIV/Rand, key byte order, lifetime and interactions with bluetoothd require
careful verification. It is a possible next step, **not an implemented solution**.
D-Bus GATT alone has no API for supplying the Nintendo-derived LTK to the link-layer
encryption procedure. Firmware/NVM rewriting is unnecessary and prohibited here.

### Timing and observability

The community documents a 5 ms connection interval used by the console, below the
usual 7.5 ms minimum for this Bluetooth generation. The examined initial pairing
CONNECT_IND instead uses 15 ms. Which later parameters are required and accepted
by this Realtek adapter remains open. The 20 ms **advertising** interval is unrelated
to the later **connection** interval.

D-Bus does not provide all failed incoming link attempts, exact LL parameters,
all ATT discovery packets, encryption events or HCI disconnect reasons. The daemon
reports unavailable values as null and logs no fabricated success. A passive
`btmon` capture is part of the hardware-test procedure, not a substitute Bluetooth
backend. No failed attempt before Device1 creation can be guaranteed visible in
GTK's D-Bus event log.

## Escalation order and evidence needed

1. **D-Bus:** current discovery probe; capture accepted/rejected advertising,
   incoming link attempts and actual ATT handle use on the console.
2. **Management APIs:** only for a demonstrated advertising, key-loading or adapter
   configuration requirement that D-Bus cannot expose. Keep changes reversible.
3. **Raw ATT/L2CAP:** investigate if exact attribute ownership/handles are the blocker;
   determine BlueZ coexistence and encryption ownership before implementation.
4. **Raw HCI:** only if lower-layer timing/security behavior cannot be expressed by
   the above. Document precisely which packet/procedure requires it.

Do not spend this milestone on two independent identities. Start with R on hci0;
a future L on hci1 is an acceptable solution.
