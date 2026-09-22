# Local control API v1

Transport: Linux `AF_UNIX`, `SOCK_STREAM`. UTF-8 JSON, one request terminated by
LF, one JSON response terminated by LF, then connection close. No GUI involvement.
Default: `$XDG_RUNTIME_DIR/pcble2joycon2/control.sock` (GLib user runtime directory
fallback if unset). `PCBLE2JOYCON2_SOCKET` overrides it.

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
- `logs`: events with sequence greater than `after`, plus `cursor` and `oldest`.

## Responses

```json
{"version":1,"ok":true,"result":{"state":"transitioning"}}
{"version":1,"ok":false,"error":"Operation in progress; query status before retrying"}
```

The first example is abbreviated. Successful mutation responses include the same
complete status schema as `status`. `ok:true` means the request was accepted:
**poll status until `state` is no longer `transitioning`**. Registration can fail
asynchronously, leaving `state:error` with a diagnostic. A failed advertisement
registration can leave `gatt_registered:true`; `stop` removes it.

States: `idle`, `transitioning`, `advertising`, `error`. Connection state is separate:
`peers` lists connected devices on this adapter, not authenticated Switch sessions.
Peers observed during daemon startup are logged with `initial=true`. The daemon
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
For a shell probe (optional `socat` and `jq` packages):

```sh
printf '%s\n' '{"version":1,"method":"status"}' |
  socat - UNIX-CONNECT:"$XDG_RUNTIME_DIR/pcble2joycon2/control.sock" | jq
```

The GUI uses this same API from worker tasks so slow IPC cannot block GTK. A custom
socket can be selected for the GUI with `PCBLE2JOYCON2_SOCKET`.

## Future control API

Buttons, sticks, mouse deltas, reconnect, disconnect and forget-pairing are not
implemented and are rejected as unknown methods. A later version will separate
persistent controller state from accumulated mouse deltas and allow atomic updates
of both in one report. Sustained low-latency input transport requires measurements,
persistent connections/batches and explicit ownership rules; v1 is administration
and diagnostics only. capture2cloud must use the API, never simulated GTK clicks.
