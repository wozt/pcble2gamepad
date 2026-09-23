# Data-length negotiation experiment

The public Joy-Con pairing capture has a peripheral-originated LL_LENGTH_REQ
before the console's PHY request and first ATT exchange. It requests 251 octets
and 2120 microseconds in each direction. The local Realtek controller reports
support for these limits, and the same suggested defaults, but previous host
captures did not show a data-length-change event.

BlueZ's GATT D-Bus API does not expose connection-specific LE data-length control.
For this bounded experiment, the standard HCI LE Set Data Length command (0x2022)
will be issued only on the verified console connection, with 251 TX octets and
2120 microseconds. This is a temporary per-link suggestion, not a firmware/NVM
write or a replacement backend. HCI command success alone does not prove that an
over-the-air negotiation occurred; the capture must be checked separately.

Reference: Bluetooth Core, Volume 4, Part E, LE Set Data Length command;
public capture `btle_joycon2_pairing_decrypted.pcapng`, frames 504–505.

On 2026-09-23 the Realtek (now hci1) accepted the per-link request with status 0.
No data-length-change event or new console ATT request was observed before the
user changed the immediate objective to the Classic Pro Controller POC. The BLE
advertisement and test link were stopped. This experiment is inconclusive and
paused; it does not establish a PHY requirement or a firmware defect.
