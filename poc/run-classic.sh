#!/bin/bash
# Run a bounded, reversible Classic HID experiment. Build as your normal user first.
set -euo pipefail
[[ $EUID == 0 ]] || { echo 'Run with sudo after building build-pro.' >&2; exit 1; }
[[ $# == 1 && $1 =~ ^hci[0-9]+$ ]] || { echo "Usage: $0 hciN" >&2; exit 2; }
project_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)
adapter=$1
app="$project_dir/build-pro/pcble2gamepad-pro-poc"
[[ -x "$app" ]] || { echo 'Build first: meson setup build-pro poc && meson compile -C build-pro'; exit 1; }
[[ -d /sys/class/bluetooth/$adapter ]] || { echo 'Adapter not present'; exit 1; }
for controller in /sys/class/bluetooth/hci*; do
    [[ -z "$(hcitool -i "${controller##*/}" con | tail -n +2)" ]] || {
        echo 'Connected Bluetooth devices; stop them before this shared-service test.'; exit 1;
    }
done
dropin=/run/systemd/system/bluetooth.service.d/90-pcble2gamepad-pro-poc.conf
[[ ! -e "$dropin" ]] || { echo 'A POC service override already exists'; exit 1; }
systemctl is-active --quiet bluetooth || { echo 'Bluetooth service must already be active'; exit 1; }
mkdir -p "$project_dir/artifacts"
capture=$(mktemp -d "$project_dir/artifacts/pro-classic-XXXXXXXX")
chmod 700 "$capture"
monitor_pid=
cleanup() {
    trap - EXIT INT TERM
    if [[ -n "$monitor_pid" ]]; then kill -INT "$monitor_pid" 2>/dev/null || true; wait "$monitor_pid" || true; fi
    rm -f "$dropin"
    rmdir /run/systemd/system/bluetooth.service.d 2>/dev/null || true
    systemctl daemon-reload
    systemctl restart bluetooth
    chmod 600 "$capture"/*
    if [[ -n ${SUDO_UID:-} && -n ${SUDO_GID:-} ]]; then chown -R "$SUDO_UID:$SUDO_GID" "$capture"; fi
    echo "Normal BlueZ restored. Private capture: $capture"
}
trap cleanup EXIT
# The foreground timeout forwards terminal signals to the POC, which restores adapter properties.
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p /run/systemd/system/bluetooth.service.d
cat > "$dropin" <<'SERVICE'
[Service]
ExecStart=
ExecStart=/usr/libexec/bluetooth/bluetoothd --compat --noplugin=*
SERVICE
systemctl daemon-reload
systemctl restart bluetooth
btmon -i "$adapter" -w "$capture/hci.btsnoop" > "$capture/btmon.log" 2>&1 &
monitor_pid=$!
echo 'Open Change Grip/Order. Commands: buttons HEX HEX HEX; sticks LX LY RX RY; release; status; quit.'
echo 'The test ends after 10 minutes; no buttons are pressed automatically.'
timeout --foreground --signal=TERM --kill-after=10s 600 "$app" "$adapter" "$project_dir/poc/pro-controller.xml" | tee "$capture/daemon.log"
