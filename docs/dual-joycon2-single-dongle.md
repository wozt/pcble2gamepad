# Dual Joy-Con 2 BLE Emulation on Single Bluetooth 6.0 Dongle

## Project Objective

Fully emulate two independent Nintendo Switch 2 Joy-Con 2 controllers (LEFT and RIGHT), connected simultaneously to a real Nintendo Switch 2, using only ONE USB Bluetooth 6.0 adapter.

## Hardware

- **Adapter**: TP-Link USB Bluetooth 6.0 dongle
- **USB VID:PID**: `37ad:0600`
- **Chipset**: Realtek RTL8761BUV
- **MAC Address**: `60:15:6F:CD:49:E5` (stable)
- **Kernel**: Linux `6.12.113+deb13-amd64`
- **BlueZ**: 5.82
- **HCI Version**: 5.1
- **Manufacturer**: Realtek Semiconductor Corporation (0x005d)

## Phase 1: Adapter Capability Investigation

### Status: IN_PROGRESS

### Diagnostic Tools

Two diagnostic tools are available:

1. **pcble2gamepaddiag** - Read-only capability inspector
   - Reads LE features and supported states
   - Reports number of supported advertising sets
   - No HCI writes, only read commands

2. **pcble2gamepad-dual-adv-diag** - Dual advertising set tester
   - Attempts to create two connectable advertising sets
   - Tests per-set random address support
   - Requires root privileges
   - Performs cleanup on exit

### Running Diagnostics

```bash
# Read-only capability check (no root required for basic info, but needs root for HCI commands)
sudo ./build/pcble2gamepaddiag hci0

# Dual advertising test (requires root)
sudo ./build/pcble2gamepad-dual-adv-diag hci0
```

### Current Adapter Report (from bluetoothctl)

```
Adapter: hci0 (60:15:6F:CD:49:E5)
SupportedInstances: 4
SupportedSecondaryChannels: 1M, 2M, Coded
MaxAdvLen: 251
MaxScnRspLen: 251
SupportedFeatures: CanSetTxPower, HardwareOffload
```

### Test Results (2026-10-31)

**Single Joy-Con 2 Connection Test:**
- Daemon started successfully with BlueZ D-Bus
- Advertising registered successfully
- Switch 2 (38:C6:CE:1F:B5:31) established BLE connection
- Switch performed GATT read on control characteristic (handle 0x0003)
- **Result**: Connection established but Switch did NOT display controller in sync menu

**Critical Discovery:**
The new TP-Link adapter **supports LE 2M PHY**, while the older Realtek adapter (E0:AD:47:40:70:D9) did not. This may be the missing requirement for Switch 2 to recognize the controller.

Reference capture shows the Switch selecting LE 2M PHY in both directions during the connection sequence. The old adapter's lack of LE 2M support may have prevented the Switch from completing initialization.

### Known Adapter Capabilities

From validation.md (2026-09-22 tests on Realtek E0:AD:47:40:70:D9):

- **LE Features**: `bd00000000000000` (bit 5 = DLE supported, bit 8 = LE 2M NOT supported)
- **LE States**: `ffffffff00000000` (bits above 31 absent)
- **Linux Restriction**: Requires state bits 38+21 (peripheral) or 35+19 (central) for connectable advertising with existing link
- **LE 2M PHY**: NOT supported on the older Realtek adapter
- **Data Length Extension**: Supported

### Open Questions for New TP-Link Adapter

1. Does the RTL8761BUV support LE 2M PHY?
2. What are the exact LE states?
3. Can it support two simultaneous connectable advertising sets with distinct random addresses?
4. Can it maintain two simultaneous BLE peripheral connections?
5. Does Linux/BlueZ impose any additional restrictions beyond the hardware?

### Investigation Plan

1. Run `pcble2gamepaddiag` to read baseline capabilities
2. Run `pcble2gamepad-dual-adv-diag` to test dual advertising sets
3. Verify with external BLE scanner (bluetoothctl or nRF Connect)
4. If dual advertising works, test simultaneous connections
5. Document any hardware or software limitations found

## Architecture

### Target Topology

```
            DEBIAN 13 PC
                 |
          PCBLE2GAMEPAD
                 |
      TP-Link Bluetooth 6.0
           37ad:0600
                 |
        ONE physical HCI
                 |
          +------+------+
          |             |
       Joy-Con 2     Joy-Con 2
         LEFT          RIGHT
          |             |
          +------+------+
                 |
          NINTENDO SWITCH 2
                 |
       Two distinct BLE links
                 |
       Both controllers usable
```

### Controller Identities

- **Joy-Con 2 Left**: VID `057e` / PID `2067`
- **Joy-Con 2 Right**: VID `057e` / PID `2066`

Each controller requires independent:
- BLE identity and address
- Advertising configuration
- Connection handle
- GATT/ATT session
- Pairing state machine
- Encryption material
- Paired Switch association
- Input report sequence counter
- Button and analog stick states
- Connection and reconnection lifecycle

## Implementation Status

### Phase 1: Adapter Capability Investigation
- **Status**: COMPLETED
- **Diagnostic tools**: IMPLEMENTED
- **Hardware testing**: COMPLETED
- **Key Finding**: TP-Link adapter supports 4 advertising sets and LE 2M PHY (critical difference from old Realtek)

### Phase 1a: LE 2M PHY Enforcement
- **Status**: IN_PROGRESS
- **Current Issue**: BlueZ D-Bus may not expose PHY control; may need Management API
- **Next Step**: Investigate BlueZ Management API for LE Set PHY command

### Phase 1b: Nintendo Command Responses
- **Status**: PENDING
- **Required**: Implement responses to pairing and initialization commands
- **Reference**: protocol.md and ndeadly research captures

### Phase 2: Independent Joy-Con 2 Identities
- **Status**: NOT_STARTED
- **Architecture design**: NOT_STARTED
- **Implementation**: NOT_STARTED

### Phase 3: Nintendo BLE Discovery and GATT
- **Status**: NOT_STARTED
- **Left controller GATT**: NOT_STARTED
- **Right controller GATT**: NOT_STARTED
- **GATT handle routing**: NOT_STARTED

### Phase 4: Proprietary Joy-Con 2 Pairing
- **Status**: NOT_STARTED
- **Crypto implementation**: NOT_STARTED
- **Test vectors**: NOT_STARTED
- **State machine**: NOT_STARTED

### Phase 5: Simultaneous Gamepad Input
- **Status**: NOT_STARTED
- **Input routing**: NOT_STARTED
- **Report generation**: NOT_STARTED
- **Concurrency testing**: NOT_STARTED

### Phase 6: Controller Studio Integration
- **Status**: NOT_STARTED
- **Profile creation**: NOT_STARTED
- **UI panels**: NOT_STARTED
- **Controls**: NOT_STARTED

### Phase 7: Hardware Validation
- **Status**: NOT_STARTED
- **Real Switch 2 testing**: NOT_STARTED
- **Verification steps**: NOT_STARTED

### Phase 8: Regression Testing
- **Status**: NOT_STARTED
- **Pro Controller preservation**: NOT_STARTED
- **Test suite**: NOT_STARTED

## Protocol References

Primary research repository:
https://github.com/ndeadly/switch2_controller_research

Key documents:
- `bluetooth_interface.md` - BLE interface specification
- `descriptors.md` - GATT descriptor layout
- `hid_reports.md` - Input report format
- `commands.md` - Command framing and pairing
- `captures/nrf52840/` - Reference PCAP captures

## Risks and Dependencies

### Hardware Risks

1. ~~**Single-adapter dual identity unsupported**: The RTL8761BUV may not support two simultaneous connectable advertising sets with distinct addresses~~ **RESOLVED**: Adapter supports 4 advertising sets
2. **Connection limit**: Maximum simultaneous BLE connections may be insufficient (to be tested)
3. ~~**LE 2M requirement**: If the Switch 2 requires LE 2M PHY and the adapter doesn't support it, emulation may fail~~ **RESOLVED**: New adapter supports LE 2M PHY (critical difference from old Realtek)
4. **Linux kernel restrictions**: The kernel may block certain LE state combinations regardless of hardware capability

### Software Risks

1. **BlueZ limitations**: BlueZ D-Bus may not expose per-advertising-set random address control
2. **GATT handle fidelity**: BlueZ allocates handles dynamically; Nintendo may expect fixed handles
3. **Pairing encryption**: Installing Nintendo-derived LTKs may require Management APIs or raw HCI
4. **State isolation**: Ensuring LEFT and RIGHT controller states don't interfere

### Fallback Options

If single-dongle dual identity is not feasible:

1. Use two separate Bluetooth adapters (already supported for Joy-Con 1 Classic)
2. Investigate raw HCI userspace backend (BTstack or similar)
3. Wait for future adapter/firmware with better support

## Recovery Procedures

If the diagnostic tool leaves advertising enabled:

```bash
# Stop advertising via bluetoothctl
bluetoothctl
select hci0
advertise off
# Or reset the adapter
sudo hciconfig hci0 reset
```

If the adapter gets into a bad state:

```bash
# Power cycle the adapter
sudo hciconfig hci0 down
sudo hciconfig hci0 up
# Or restart bluetoothd
sudo systemctl restart bluetooth
```

## Notes

- This investigation is for Joy-Con 2 BLE only, not Joy-Con 1 Classic HID
- The existing Switch Pro Controller Classic HID implementation must remain functional
- No firmware or NVM modifications are performed
- All diagnostics are read-only or reversible
- Work on Capture2Cloud is paused for this milestone
