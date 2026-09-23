#!/bin/bash
# Run the reversible Classic HID backend.
set -euo pipefail
[[ $EUID == 0 ]] || { echo 'Run through pkexec or sudo.' >&2; exit 1; }
[[ ($# == 1 || ($# == 2 && $2 == --desktop)) && $1 =~ ^hci[0-9]+$ ]] || {
    echo "Usage: $0 hciN [--desktop]" >&2; exit 2;
}
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
adapter=$1
installed=false
if [[ -x "$script_dir/pcble2gamepad-pro-backend" && -f "$script_dir/pro-controller.xml" ]]; then
    app="$script_dir/pcble2gamepad-pro-backend"
    sdp="$script_dir/pro-controller.xml"
    installed=true
else
    project_dir=$(cd -- "$script_dir/.." && pwd)
    sdp="$project_dir/poc/pro-controller.xml"
    app=
    for candidate in \
        "$project_dir/build/pcble2gamepad-pro-backend" \
        "$project_dir/build-studio/pcble2gamepad-pro-backend" \
        "$project_dir/build-check/pcble2gamepad-pro-backend" \
        "$project_dir/build-pro/pcble2gamepad-pro-poc"; do
        if [[ -x "$candidate" ]]; then app=$candidate; break; fi
    done
fi
[[ -n "$app" && -x "$app" ]] || { echo 'Build the Pro Controller backend first.' >&2; exit 1; }
[[ -d /sys/class/bluetooth/$adapter ]] || { echo 'Adapter not present'; exit 1; }
for controller in /sys/class/bluetooth/hci*; do
    [[ -z "$(hcitool -i "${controller##*/}" con | tail -n +2)" ]] || {
        echo 'Connected Bluetooth devices; stop them before this shared-service test.'; exit 1;
    }
done
dropin=/run/systemd/system/bluetooth.service.d/90-pcble2gamepad-pro-poc.conf
[[ ! -e "$dropin" ]] || { echo 'A POC service override already exists'; exit 1; }
systemctl is-active --quiet bluetooth || { echo 'Bluetooth service must already be active'; exit 1; }
capture_owner=${PKEXEC_UID:-${SUDO_UID:-0}}
if $installed; then
    capture_base="/var/lib/pcble2gamepad/$capture_owner"
else
    capture_base="$project_dir/artifacts"
fi
mkdir -p "$capture_base"
if [[ $capture_owner != 0 ]]; then chown "$capture_owner" "$capture_base"; fi
chmod 700 "$capture_base"
capture=$(mktemp -d "$capture_base/pro-classic-XXXXXXXX")
chmod 700 "$capture"
monitor_pid=
cleanup() {
    trap - EXIT INT TERM
    if [[ -n "$monitor_pid" ]]; then kill -INT "$monitor_pid" 2>/dev/null || true; wait "$monitor_pid" || true; fi
    rm -f "$dropin"
    rmdir /run/systemd/system/bluetooth.service.d 2>/dev/null || true
    systemctl daemon-reload
    systemctl restart bluetooth
    find "$capture" -type f -exec chmod 600 {} +
    if [[ $capture_owner != 0 ]]; then chown -R "$capture_owner" "$capture"; fi
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
if [[ ${2:-} == --desktop ]]; then
    [[ -n ${PKEXEC_UID:-} ]] || { echo 'Desktop mode requires pkexec.' >&2; exit 1; }
    mkdir -p "/run/pcble2gamepad/$PKEXEC_UID"
    chown "$PKEXEC_UID" "/run/pcble2gamepad/$PKEXEC_UID"
    chmod 700 "/run/pcble2gamepad/$PKEXEC_UID"
    "$app" "$adapter" "$sdp" --desktop "$PKEXEC_UID" | tee "$capture/daemon.log"
else
    echo 'Open Change Grip/Order. Commands: buttons HEX HEX HEX; sticks LX LY RX RY; release; status; quit.'
    echo 'The test ends after 10 minutes; no buttons are pressed automatically.'
    timeout --foreground --signal=TERM --kill-after=10s 600 "$app" "$adapter" "$sdp" | tee "$capture/daemon.log"
fi
