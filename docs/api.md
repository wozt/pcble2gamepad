# Local control API v1

Transport: Linux `AF_UNIX`, `SOCK_STREAM`. UTF-8 JSON, one request terminated by
LF, one JSON response terminated by LF, then connection close. No GUI involvement.
Default: `$XDG_RUNTIME_DIR/pcble2gamepad/control.sock` (GLib user runtime directory
fallback if unset). `PCBLE2GAMEPAD_SOCKET` overrides it.

The server uses asynchronous I/O and dispatches engine actions in one GLib main
context. Clients may connect concurrently. Only one lifecycle transition is
accepted at a time. A socket lock prevents two daemons from owning the same path;
a stale socket is recovered after an unclean exit. Only peers with the daemon's
UID are admitted. Bounds: 4096 request bytes excluding LF, 32 simultaneous clients,
10-second connection deadline. A client should bound response size (the shipped
client allows 2 MiB) and apply a timeout.

## Requests

```json
{"version":1,"method":"status"}
{"version":1,"method":"start"}
{"version":1,"method":"sync"}
{"version":1,"method":"stop"}
{"version":1,"method":"disconnect","address":"AA:BB:CC:DD:EE:FF"}
{"version":1,"method":"logs","after":0}
```

`version` and `after` are JSON integers. Unknown methods, malformed JSON, invalid
versions, negative/noninteger cursors and concurrent transitions return `ok:false`.
Additional object members are currently ignored; no future input command is silently
accepted. Missing `after` defaults to zero.

- `status`: return lifecycle state, adapter/address, active right identity,
  advertising and GATT registration state, known connected adapter peers and error.
- `start`: register the observation GATT application, then discovery advertisement.
- `sync`: same advertisement as `start`, plus a sync event. No SMP or proprietary
  pairing is initiated. Calling start/sync while advertising is idempotent.
- `stop`: unregister this application's advertisement and GATT application. Existing
  adapter links are not forcibly disconnected. Idempotent when already stopped.
- `disconnect`: explicitly disconnect one currently observed adapter peer, selected
  by its full Bluetooth address (case-insensitive). No wildcard or implicit peer
  selection is accepted. The peer need not be a Switch: check its identity first.
  This calls Device1.Disconnect, not RemoveDevice, and never forgets pairing.
  Other peer links and this application's advertising registration are unchanged.
- `logs`: events with sequence greater than `after`, plus `cursor` and `oldest`.

## Responses

```json
{"version":1,"ok":true,"result":{"state":"transitioning"}}
{"version":1,"ok":false,"error":"Operation in progress; query status before retrying"}
```

The first example is abbreviated. Successful mutation responses include the same
complete status schema as `status`. `ok:true` means the request was accepted:
**poll status until `state` is no longer `transitioning`**. Registration can fail
asynchronously. A failed advertisement registration automatically unregisters the
probe GATT application, then leaves `state:error` with a diagnostic. Cleanup does
not disconnect any peer. If cleanup itself fails, inspect `gatt_registered` and
use `stop` or exit the daemon to release its D-Bus registrations.

States: `idle`, `transitioning`, `advertising`, `error`. Connection state is separate:
`peers` lists connected devices on this adapter, not authenticated Switch sessions.
Peers first seen in an adapter snapshot are logged once as `peer_already_connected`
with `initial=true`, and have `first_seen_in_snapshot:true` in status. Repeated
snapshot reads do not invent connection events. The daemon
neither initiates connections nor knows which local GATT service caused a link.
`bluez_paired`, when present, is BlueZ's property, not Nintendo pairing completion.
`advertising_registered` tracks registration, not continuous on-air transmission
while a connection is active. Unknown encryption and connection intervals are JSON
`null`; report rate is zero because this prototype sends no input notifications.

Each log entry has `seq`, UTC `time`, `level`, `event` and `message`. The newest 1000
events are retained. Events are also written as JSON lines to stderr; `--verbose`
includes DEBUG there. The in-memory API ring always includes DEBUG. If `after` is
older than `oldest - 1`, events were lost; reread from zero. Cursors reset on daemon
restart: if `cursor < after`, reset to zero. Logs are not a durable audit store.

## C client example

`src/client.h` provides `jc_client_request(path, method, after, error)` returning an
owned `JsonObject`. Link the client library with GIO and JSON-GLib; GTK is unnecessary.
Use `jc_client_request_full(path, "disconnect", 0, peer_address, error)` for a
targeted disconnect; the simpler entry point remains available for existing clients.
For a shell probe (optional `socat` and `jq` packages):

```sh
printf '%s\n' '{"version":1,"method":"status"}' |
  socat - UNIX-CONNECT:"$XDG_RUNTIME_DIR/pcble2gamepad/control.sock" | jq
```

The GUI uses this same API from worker tasks so slow IPC cannot block GTK. A custom
socket can be selected for the GUI with `PCBLE2GAMEPAD_SOCKET`.
Its Disconnect peer button targets the displayed address and is enabled only
when exactly one connected peer is available; multiple peers require an explicit
CLI address. No program automatically disconnects a peer on advertisement failure.

## Future control API

Buttons, sticks, mouse deltas, reconnect and forget-pairing are not
implemented and are rejected as unknown methods. A later version will separate
persistent controller state from accumulated mouse deltas and allow atomic updates
of both in one report. Sustained low-latency input transport requires measurements,
persistent connections/batches and explicit ownership rules; v1 is administration
and diagnostics only. capture2cloud must use the API, never simulated GTK clicks.

## Pro Controller session API

Controller Studio uses a separate local API owned by the Classic HID backend. The
Pro Controller path is `/run/pcble2gamepad/UID/pro.sock`. Joy-Con pair sessions use
`joycon-left.sock` and `joycon-right.sock` in the same directory. Each socket and
parent directory are mode `0600` and `0700`, and peer credentials must match that desktop UID. The
root backend creates the socket for the authenticated `PKEXEC_UID`. Mock mode uses
`$XDG_RUNTIME_DIR/pcble2gamepad/pro.sock`. `PCBLE2GAMEPAD_PRO_SOCKET` can override
the mock/client path for tests.

The same one-line JSON framing and version `1` apply:

```json
{"version":1,"method":"status"}
{"version":1,"method":"input","buttons":[8,0,0],"sticks":[2159,1916,2070,2013]}
{"version":1,"method":"logging","enabled":true}
{"version":1,"method":"release"}
{"version":1,"method":"stop"}
```

`buttons` contains the three Switch report bytes. Each `sticks` member is a 12-bit
value from 0 through 4095. The arrays must have exactly three and four integer
members. A valid input frame replaces the complete controller state atomically.
If another valid frame does not arrive within 500 ms, the backend returns every
button and stick to neutral. `release` does so immediately. `stop` exits the
backend, after which the launcher restores normal BlueZ. In desktop mode, every
request also renews a five-second presence lease. If the UI crashes or disappears,
the backend exits when that lease expires and the launcher performs the same
restoration.

`logging` enables or disables repetitive `hid_rx` packet lines and periodic
`status` lines immediately. Detailed traffic is disabled when a backend starts.

Successful responses contain `simulated`, `initialized`, `state`, `peer`, report
counters, player lights, current buttons/sticks and a bounded diagnostics list.
`state:connected` means the HID peer is present; `initialized:true` means the
console configured vibration and player lights. The UI labels mock results as
simulation. High-volume raw HID receive events remain in the private HCI capture
when detailed application logging is disabled.

For a Joy-Con pair, Controller Studio sends the same atomic input frame to both
sockets. The left backend masks right-side buttons and the right stick; the right
backend masks left-side buttons and the left stick. Status is merged only when both
requests succeed. Closing the UI sends `stop` to both processes.
