#!/bin/bash
# Run the reversible Classic HID backend.
set -euo pipefail
[[ $EUID == 0 ]] || { echo 'Run through pkexec or sudo.' >&2; exit 1; }
[[ $# -ge 1 && $1 =~ ^hci[0-9]+$ ]] || { echo "Usage: $0 hciN [--desktop] [--profile pro|joycon-pair] [--secondary hciN]" >&2; exit 2; }
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
adapter=$1
shift
desktop=false
profile=pro
secondary=
while [[ $# -gt 0 ]]; do
    case $1 in
        --desktop) desktop=true; shift ;;
        --profile) [[ $# -ge 2 ]] || { echo 'Missing profile value' >&2; exit 2; }; profile=$2; shift 2 ;;
        --secondary) [[ $# -ge 2 && $2 =~ ^hci[0-9]+$ ]] || { echo 'Invalid secondary adapter' >&2; exit 2; }; secondary=$2; shift 2 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
[[ $profile == pro || $profile == joycon-pair ]] || { echo 'Profile must be pro or joycon-pair' >&2; exit 2; }
if [[ $profile == joycon-pair ]]; then
    $desktop || { echo 'Joy-Con pair currently requires desktop mode' >&2; exit 2; }
    [[ -n $secondary && $secondary != "$adapter" ]] || { echo 'Joy-Con pair requires two distinct adapters' >&2; exit 2; }
fi
installed=false
if [[ -x "$script_dir/pcble2gamepad-controller-backend" && -f "$script_dir/pro-controller.xml" ]]; then
    app="$script_dir/pcble2gamepad-controller-backend"
    sdp="$script_dir/pro-controller.xml"
    installed=true
else
    project_dir=$(cd -- "$script_dir/.." && pwd)
    sdp="$project_dir/poc/pro-controller.xml"
    app=
    for candidate in \
        "$project_dir/build/pcble2gamepad-controller-backend" \
        "$project_dir/build-studio/pcble2gamepad-controller-backend" \
        "$project_dir/build-check/pcble2gamepad-controller-backend" \
        "$project_dir/build-pro/pcble2gamepad-pro-poc"; do
        if [[ -x "$candidate" ]]; then app=$candidate; break; fi
    done
fi
[[ -n "$app" && -x "$app" ]] || { echo 'Build the controller backend first.' >&2; exit 1; }
[[ -d /sys/class/bluetooth/$adapter ]] || { echo 'Adapter not present'; exit 1; }
[[ -z $secondary || -d /sys/class/bluetooth/$secondary ]] || { echo 'Secondary adapter not present'; exit 1; }
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
monitor_pids=()
backend_pids=()
cleanup() {
    trap - EXIT INT TERM
    app_target=$(readlink -f "$app")
    for pid in "${backend_pids[@]}"; do
        [[ -e /proc/$pid/exe && $(readlink -f "/proc/$pid/exe") == "$app_target" ]] && kill -TERM "$pid" 2>/dev/null || true
    done
    for pid in "${backend_pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    for pid in "${monitor_pids[@]}"; do kill -INT "$pid" 2>/dev/null || true; wait "$pid" || true; done
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
monitor_pids+=("$!")
if [[ -n $secondary ]]; then
    btmon -i "$secondary" -w "$capture/hci-secondary.btsnoop" > "$capture/btmon-secondary.log" 2>&1 &
    monitor_pids+=("$!")
fi
if $desktop; then
    [[ -n ${PKEXEC_UID:-} ]] || { echo 'Desktop mode requires pkexec.' >&2; exit 1; }
    mkdir -p "/run/pcble2gamepad/$PKEXEC_UID"
    chown "$PKEXEC_UID" "/run/pcble2gamepad/$PKEXEC_UID"
    chmod 700 "/run/pcble2gamepad/$PKEXEC_UID"
    if [[ $profile == pro ]]; then
        "$app" "$adapter" "$sdp" --desktop "$PKEXEC_UID" --type pro | tee "$capture/daemon.log"
    else
        "$app" "$adapter" "$sdp" --desktop "$PKEXEC_UID" --type joycon-l --allow-adapter "$secondary" > "$capture/joycon-left.log" 2>&1 &
        left_pid=$!;backend_pids+=("$left_pid")
        left_socket="/run/pcble2gamepad/$PKEXEC_UID/joycon-left.sock"
        for _ in {1..100}; do [[ -S $left_socket ]] && break; kill -0 "$left_pid" 2>/dev/null || break; sleep .05; done
        [[ -S $left_socket ]] || { echo 'Left Joy-Con backend failed to start' >&2; wait "$left_pid" || true; exit 1; }
        "$app" "$secondary" "$sdp" --desktop "$PKEXEC_UID" --type joycon-r --allow-adapter "$adapter" --shared-profile > "$capture/joycon-right.log" 2>&1 &
        right_pid=$!;backend_pids+=("$right_pid")
        set +e
        wait "$left_pid"; left_status=$?
        wait "$right_pid"; right_status=$?
        backend_pids=()
        set -e
        [[ $left_status == 0 && $right_status == 0 ]]
    fi
else
    echo 'Open Change Grip/Order. Commands: buttons HEX HEX HEX; sticks LX LY RX RY; release; status; quit.'
    echo 'The test ends after 10 minutes; no buttons are pressed automatically.'
    timeout --foreground --signal=TERM --kill-after=10s 600 "$app" "$adapter" "$sdp" --type pro | tee "$capture/daemon.log"
fi
