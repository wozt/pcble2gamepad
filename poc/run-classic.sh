#!/bin/bash
# Run the reversible Classic HID backend.
set -euo pipefail
[[ $EUID == 0 ]] || { echo 'Run through pkexec or sudo.' >&2; exit 1; }
[[ $# -ge 1 && $1 =~ ^hci[0-9]+$ ]] || { echo "Usage: $0 hciN [--desktop] [--profile pro|joycon-pair] [--secondary hciN] [--verbose] [--body-color RRGGBB] [--button-color RRGGBB] [--left-grip-color RRGGBB] [--right-grip-color RRGGBB] [--reconnect MAC]" >&2; exit 2; }
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
adapter=$1
shift
desktop=false
profile=pro
secondary=
reconnect=
verbose=false
body_color=828282
button_color=0F0F0F
left_grip_color=828282
right_grip_color=828282
while [[ $# -gt 0 ]]; do
    case $1 in
        --desktop) desktop=true; shift ;;
        --profile) [[ $# -ge 2 ]] || { echo 'Missing profile value' >&2; exit 2; }; profile=$2; shift 2 ;;
        --secondary) [[ $# -ge 2 && $2 =~ ^hci[0-9]+$ ]] || { echo 'Invalid secondary adapter' >&2; exit 2; }; secondary=$2; shift 2 ;;
        --reconnect)
            [[ $# -ge 2 && $2 =~ ^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$ ]] || { echo 'Invalid reconnect MAC' >&2; exit 2; }
            reconnect=$2
            shift 2
            ;;
        --verbose) verbose=true; shift ;;
        --body-color)
            [[ $# -ge 2 && $2 =~ ^[0-9A-Fa-f]{6}$ ]] || { echo 'Invalid body color' >&2; exit 2; }
            body_color=$2
            shift 2
            ;;
        --button-color)
            [[ $# -ge 2 && $2 =~ ^[0-9A-Fa-f]{6}$ ]] || { echo 'Invalid button color' >&2; exit 2; }
            button_color=$2
            shift 2
            ;;
        --left-grip-color)
            [[ $# -ge 2 && $2 =~ ^[0-9A-Fa-f]{6}$ ]] || { echo 'Invalid left grip color' >&2; exit 2; }
            left_grip_color=$2
            shift 2
            ;;
        --right-grip-color)
            [[ $# -ge 2 && $2 =~ ^[0-9A-Fa-f]{6}$ ]] || { echo 'Invalid right grip color' >&2; exit 2; }
            right_grip_color=$2
            shift 2
            ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
[[ $profile == pro || $profile == joycon-pair ]] || { echo 'Profile must be pro or joycon-pair' >&2; exit 2; }
if [[ $profile == joycon-pair ]]; then
    $desktop || { echo 'Joy-Con pair currently requires desktop mode' >&2; exit 2; }
    [[ -n $secondary && $secondary != "$adapter" ]] || { echo 'Joy-Con pair requires two distinct adapters' >&2; exit 2; }
fi
[[ -z $reconnect || $profile == pro ]] || { echo 'Reconnect is currently implemented for the Pro Controller profile only.' >&2; exit 2; }
backend_options=()
$verbose && backend_options+=(--verbose)
[[ -n $reconnect ]] && backend_options+=(--reconnect "$reconnect")
if [[ $profile == pro ]]; then
    backend_options+=(
        --body-color "$body_color"
        --button-color "$button_color"
        --left-grip-color "$left_grip_color"
        --right-grip-color "$right_grip_color"
    )
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
bluetooth_overridden=false

[[ ! -e "$dropin" ]] || {
    echo 'A pcble2gamepad Bluetooth service override already exists'
    exit 1
}

systemctl is-active --quiet bluetooth || {
    echo 'Bluetooth service must already be active'
    exit 1
}
capture_owner=${PKEXEC_UID:-${SUDO_UID:-0}}

# Runtime diagnostics are temporary session artifacts, not persistent state.
# Keep every capture in a private random directory under /tmp.
capture=$(mktemp -d "/tmp/pcble2gamepad-${capture_owner}-XXXXXXXX")
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
    if $bluetooth_overridden; then
        rm -f "$dropin"
        rmdir /run/systemd/system/bluetooth.service.d 2>/dev/null || true
        systemctl daemon-reload
        systemctl restart bluetooth
    fi
    find "$capture" -type f -exec chmod 600 {} +
    if [[ $capture_owner != 0 ]]; then chown -R "$capture_owner" "$capture"; fi
    echo "Normal BlueZ restored. Private capture: $capture"
}
trap cleanup EXIT
# The foreground timeout forwards terminal signals to the POC, which restores adapter properties.
trap 'exit 130' INT
trap 'exit 143' TERM
if [[ -n $reconnect ]]; then
    echo 'Reconnect mode: starting isolated BlueZ and restoring persistent controller pairing.'
else
    echo 'Pairing mode: starting isolated BlueZ compatibility service.'
fi

mkdir -p /run/systemd/system/bluetooth.service.d
cat > "$dropin" <<'SERVICE'
[Service]
ExecStart=
ExecStart=/usr/libexec/bluetooth/bluetoothd --compat --noplugin=*
SERVICE

bluetooth_overridden=true
systemctl daemon-reload
systemctl restart bluetooth

if [[ -n $reconnect ]]; then
    echo 'Reconnect mode: BlueZ restarted; loading saved Link Key.'
else
    echo 'Pairing mode: BlueZ restarted; starting controller backend.'
fi

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
        "$app" "$adapter" "$sdp" --desktop "$PKEXEC_UID" --type pro "${backend_options[@]}" | tee "$capture/daemon.log"
    else
        "$app" "$adapter" "$sdp" --desktop "$PKEXEC_UID" --type joycon-l --allow-adapter "$secondary" "${backend_options[@]}" > "$capture/joycon-left.log" 2>&1 &
        left_pid=$!;backend_pids+=("$left_pid")
        left_socket="/run/pcble2gamepad/$PKEXEC_UID/joycon-left.sock"
        for _ in {1..100}; do [[ -S $left_socket ]] && break; kill -0 "$left_pid" 2>/dev/null || break; sleep .05; done
        [[ -S $left_socket ]] || { echo 'Left Joy-Con backend failed to start' >&2; wait "$left_pid" || true; exit 1; }
        "$app" "$secondary" "$sdp" --desktop "$PKEXEC_UID" --type joycon-r --allow-adapter "$adapter" --shared-profile "${backend_options[@]}" > "$capture/joycon-right.log" 2>&1 &
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
    timeout --foreground --signal=TERM --kill-after=10s 600 "$app" "$adapter" "$sdp" --type pro "${backend_options[@]}" | tee "$capture/daemon.log"
fi
