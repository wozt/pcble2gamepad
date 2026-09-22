#!/bin/sh
set -eu
build=$1
if [ "${2-}" != inside ]; then
    exec dbus-run-session -- "$0" "$build" inside
fi
# Never connect this test to the real system bus or adapter.
export DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"
runtime=$(mktemp -d)
export PCBLE2JOYCON2_SOCKET="$runtime/control.sock"
fake_pid=
daemon_pid=
cleanup() {
    for pid in "$daemon_pid" "$fake_pid"; do
        if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
    done
    rm -rf "$runtime"
}
trap cleanup EXIT INT TERM
"$build/fake-bluez" >"$runtime/fake.log" 2>&1 &
fake_pid=$!
i=0
while ! gdbus call --session --dest org.bluez --object-path / --method io.github.wozt.TestBluez.GetState >"$runtime/state" 2>/dev/null; do
    i=$((i + 1)); [ "$i" -lt 100 ] || exit 1
    sleep 0.02
done
"$build/pcble2joycon2d" --verbose >"$runtime/daemon.log" 2>&1 &
daemon_pid=$!
i=0
while ! "$build/pcble2joycon2ctl" status >"$runtime/status" 2>/dev/null; do
    i=$((i + 1)); [ "$i" -lt 100 ] || exit 1
    sleep 0.02
done
# Repeat the failing registration: snapshot enumeration must not invent new links.
for attempt in 1 2; do
    "$build/pcble2joycon2ctl" start >/dev/null
    i=0
    while :; do
        "$build/pcble2joycon2ctl" status >"$runtime/status"
        if grep -q '"state" : "error"' "$runtime/status"; then break; fi
        i=$((i + 1)); [ "$i" -lt 100 ] || exit 1
        sleep 0.02
    done
    grep -q '"gatt_registered" : false' "$runtime/status"
    grep -q 'Existing adapter peers' "$runtime/status"
done
gdbus call --session --dest org.bluez --object-path / --method io.github.wozt.TestBluez.GetState | grep -q '(true, false, false)'
[ "$(grep -c '"event":"peer_already_connected"' "$runtime/daemon.log")" -eq 1 ]
if grep -q '"event":"peer_connected"' "$runtime/daemon.log"; then exit 1; fi
# Unknown addresses cannot be sent to BlueZ.
if "$build/pcble2joycon2ctl" disconnect 22:34:56:78:9A:BC >/dev/null; then exit 1; fi
"$build/pcble2joycon2ctl" disconnect 12:34:56:78:9a:bc >/dev/null
i=0
while :; do
    gdbus call --session --dest org.bluez --object-path / --method io.github.wozt.TestBluez.GetState >"$runtime/state"
    if grep -q '(false, false, false)' "$runtime/state"; then break; fi
    i=$((i + 1)); [ "$i" -lt 100 ] || exit 1
    sleep 0.02
done
# A new discovery request now succeeds using the same daemon.
i=0
while ! "$build/pcble2joycon2ctl" sync >/dev/null; do
    i=$((i + 1)); [ "$i" -lt 100 ] || exit 1
    sleep 0.02
done
i=0
while :; do
    "$build/pcble2joycon2ctl" status >"$runtime/status"
    if grep -q '"state" : "advertising"' "$runtime/status"; then break; fi
    i=$((i + 1)); [ "$i" -lt 100 ] || exit 1
    sleep 0.02
done
gdbus call --session --dest org.bluez --object-path / --method io.github.wozt.TestBluez.GetState | grep -q '(false, true, true)'
"$build/pcble2joycon2ctl" stop >/dev/null
