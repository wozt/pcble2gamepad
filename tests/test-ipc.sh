#!/bin/sh
set -eu
build=$1
runtime=$(mktemp -d)
export PCBLE2GAMEPAD_SOCKET="$runtime/control.sock"
pid=
cleanup() {
    if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
    rm -rf "$runtime"
}
trap cleanup EXIT INT TERM
"$build/pcble2gamepadd" --mock >"$runtime/daemon.log" 2>&1 &
pid=$!
i=0
while [ ! -S "$PCBLE2GAMEPAD_SOCKET" ]; do
    i=$((i + 1))
    if [ "$i" -gt 100 ]; then cat "$runtime/daemon.log"; exit 1; fi
    sleep 0.02
done
"$build/pcble2gamepadctl" status | grep -q '"state" : "idle"'
"$build/pcble2gamepadctl" start | grep -q '"advertising_registered" : true'
"$build/pcble2gamepadctl" sync | grep -q '"simulated" : true'
"$build/pcble2gamepadctl" stop | grep -q '"advertising_registered" : false'
"$build/pcble2gamepadctl" logs | grep -q 'mock_advertising_started'
if "$build/pcble2gamepadctl" unsupported >"$runtime/error.json"; then exit 1; fi
grep -q '"ok" : false' "$runtime/error.json"
if "$build/pcble2gamepadd" --mock >"$runtime/second.log" 2>&1; then exit 1; fi
"$build/pcble2gamepadctl" status >/dev/null
[ "$(stat -c %a "$PCBLE2GAMEPAD_SOCKET")" = 600 ]
kill "$pid"
wait "$pid"
pid=
[ ! -e "$PCBLE2GAMEPAD_SOCKET" ]
# A stale socket is recovered after a forced termination.
"$build/pcble2gamepadd" --mock >"$runtime/restart.log" 2>&1 &
pid=$!
sleep 0.1
kill -9 "$pid"
wait "$pid" 2>/dev/null || true
pid=
"$build/pcble2gamepadd" --mock >"$runtime/recovered.log" 2>&1 &
pid=$!
sleep 0.1
"$build/pcble2gamepadctl" status >/dev/null
