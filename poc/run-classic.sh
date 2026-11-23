#!/bin/bash
# Run the reversible Classic HID backend.
set -euo pipefail
[[ $EUID == 0 ]] || { echo 'Run through pkexec or sudo.' >&2; exit 1; }
[[ $# -ge 1 && $1 =~ ^hci[0-9]+$ ]] || { echo "Usage: $0 hciN [--wake-probe|--wake-capture|--wake-send MAC HEX] [--desktop] [--profile pro|joycon-pair] [--secondary hciN] [--verbose] [--body-color RRGGBB] [--button-color RRGGBB] [--left-grip-color RRGGBB] [--right-grip-color RRGGBB] [--reconnect MAC]" >&2; exit 2; }
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
adapter=$1
shift
desktop=false
profile=pro
secondary=
expected_primary=
expected_secondary=
reconnect=
verbose=false
wake_probe=false
wake_capture=false
wake_send=false
wake_send_controller=
wake_send_data=
body_color=828282
button_color=0F0F0F
left_grip_color=828282
right_grip_color=828282
while [[ $# -gt 0 ]]; do
    case $1 in
        --wake-probe) wake_probe=true; shift ;;
        --wake-capture) wake_capture=true; shift ;;
        --wake-send)
            [[ $# -ge 3 ]] || { echo 'Missing wake-send MAC/data' >&2; exit 2; }

            wake_send_controller=$2
            wake_send_data=$3

            [[ $wake_send_controller =~ ^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$ ]] || {
                echo 'Invalid wake controller MAC' >&2
                exit 2
            }

            [[ $wake_send_data =~ ^[0-9A-Fa-f]+$ ]] || {
                echo 'Invalid wake advertisement hex' >&2
                exit 2
            }

            (( ${#wake_send_data} >= 2 &&
               ${#wake_send_data} <= 62 &&
               ${#wake_send_data} % 2 == 0 )) || {
                echo 'Wake advertisement must contain 1..31 bytes' >&2
                exit 2
            }

            wake_send=true
            shift 3
            ;;
        --desktop) desktop=true; shift ;;
        --profile) [[ $# -ge 2 ]] || { echo 'Missing profile value' >&2; exit 2; }; profile=$2; shift 2 ;;
        --secondary) [[ $# -ge 2 && $2 =~ ^hci[0-9]+$ ]] || { echo 'Invalid secondary adapter' >&2; exit 2; }; secondary=$2; shift 2 ;;
        --expect-primary-address) [[ $# -ge 2 && $2 =~ ^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$ ]] || { echo 'Invalid expected primary address' >&2; exit 2; }; expected_primary=${2^^}; shift 2 ;;
        --expect-secondary-address) [[ $# -ge 2 && $2 =~ ^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$ ]] || { echo 'Invalid expected secondary address' >&2; exit 2; }; expected_secondary=${2^^}; shift 2 ;;
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
# hciN can change on hot-plug: reject a stale GUI selection before opening
# either protocol backend. The stable address is read from the live sysfs device.
verify_adapter() {
    local device=$1 expected=$2 property actual
    [[ -z $expected ]] && return 0

    # The address sysfs attribute is not universally provided. The BlueZ
    # Adapter1 D-Bus object instead ties the current hciN to a MAC address.
    # This check happens before the launcher suspends the BlueZ daemon.
    if ! property=$(busctl --system get-property org.bluez \
        "/org/bluez/$device" org.bluez.Adapter1 Address 2>/dev/null); then
        echo "Cannot verify adapter identity via BlueZ: $device" >&2
        return 1
    fi
    if [[ $property =~ ^s[[:space:]]\"(([[:xdigit:]]{2}:){5}[[:xdigit:]]{2})\"$ ]]; then
        actual=${BASH_REMATCH[1]^^}
    else
        echo "Cannot parse BlueZ adapter address: $device" >&2
        return 1
    fi
    [[ $actual == "$expected" ]] || {
        echo "Adapter identity changed: $device is $actual (expected $expected)" >&2
        return 1
    }
}
verify_adapter "$adapter" "$expected_primary"
if [[ -n $secondary ]]; then verify_adapter "$secondary" "$expected_secondary"; fi
printf '[ROUTE] profile=%s primary=%s mac=%s secondary=%s mac=%s mode=%s\n' \
    "$profile" "$adapter" "${expected_primary:-unknown}" \
    "${secondary:-none}" "${expected_secondary:-none}" \
    "$(if [[ -n $reconnect ]]; then printf reconnect; else printf pairing; fi)"
if [[ $profile == joycon2-left || $profile == joycon2-right ]]; then
    # Joy-Con 2 runs its own BLE stack on an exclusive HCI user channel. It
    # does not touch the bluetoothd configuration; the kernel hands the
    # adapter back to BlueZ when the backend exits.
    jc2=
    for candidate in \
        "$script_dir/pcble2gamepad-joycon2-backend" \
        "$script_dir/../build/pcble2gamepad-joycon2-backend"; do
        if [[ -x "$candidate" ]]; then jc2=$candidate; break; fi
    done
    [[ -n "$jc2" ]] || { echo 'Build the Joy-Con 2 backend first.' >&2; exit 1; }
    [[ -d /sys/class/bluetooth/$adapter ]] || { echo 'Adapter not present' >&2; exit 1; }
    jc2_options=(--side "${profile#joycon2-}")
    $verbose && jc2_options+=(--verbose)
    [[ -n $reconnect ]] && jc2_options+=(--reconnect)
    if $desktop; then
        [[ -n ${PKEXEC_UID:-} ]] || { echo 'Desktop mode requires pkexec.' >&2; exit 1; }
        jc2_options+=(--desktop "$PKEXEC_UID")
    fi
    capture_owner=${PKEXEC_UID:-${SUDO_UID:-0}}
    capture=$(mktemp -d "/tmp/pcble2gamepad-${capture_owner}-XXXXXXXX")
    chmod 700 "$capture"
    status=0
    "$jc2" "$adapter" "${jc2_options[@]}" 2>&1 | tee "$capture/joycon2.log" || status=$?
    chmod 600 "$capture/joycon2.log"
    [[ $capture_owner == 0 ]] || chown -R "$capture_owner" "$capture"
    echo "Adapter returned to BlueZ. Private log: $capture"
    exit $status
fi
# Two BLE identities, each with its own HCI user channel, GATT state, bond
# record and IPC socket. No Bluetooth Classic daemon override is needed.
if [[ $profile == joycon2-pair ]]; then
    $desktop || { echo 'A Joy-Con 2 pair requires desktop mode' >&2; exit 2; }
    [[ -n ${PKEXEC_UID:-} ]] || { echo 'Desktop mode requires pkexec' >&2; exit 2; }
    [[ -n $secondary && $adapter != "$secondary" ]] || { echo 'Select two distinct Joy-Con 2 adapters' >&2; exit 2; }
    [[ -d /sys/class/bluetooth/$adapter && -d /sys/class/bluetooth/$secondary ]] || {
        echo 'One of the Joy-Con 2 adapters is missing' >&2; exit 2;
    }
    jc2=
    for candidate in "$script_dir/pcble2gamepad-joycon2-backend" \
                     "$script_dir/../build/pcble2gamepad-joycon2-backend"; do
        if [[ -x $candidate ]]; then jc2=$candidate; break; fi
    done
    [[ -n $jc2 ]] || { echo 'Build the Joy-Con 2 backend first' >&2; exit 1; }
    capture=$(mktemp -d "/tmp/pcble2gamepad-${PKEXEC_UID}-XXXXXXXX")
    chmod 700 "$capture"
    left_pid=; right_pid=
    dual_cleanup() {
        trap - EXIT INT TERM
        [[ -z $left_pid ]] || kill -TERM "$left_pid" 2>/dev/null || true
        [[ -z $right_pid ]] || kill -TERM "$right_pid" 2>/dev/null || true
        [[ -z $left_pid ]] || wait "$left_pid" 2>/dev/null || true
        [[ -z $right_pid ]] || wait "$right_pid" 2>/dev/null || true
        find "$capture" -type f -exec chmod 600 {} +
        chown -R "$PKEXEC_UID" "$capture"
        echo "Both BLE adapters released. Private logs: $capture"
    }
    trap dual_cleanup EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    options=(--vertical --desktop "$PKEXEC_UID")
    [[ -z $reconnect ]] || options+=(--reconnect)
    $verbose && options+=(--verbose)
    echo "[ROUTE] JC2-L BLE adapter=$adapter mac=${expected_primary:-unknown} socket=joycon-left.sock"
    "$jc2" "$adapter" --side left --socket joycon-left.sock "${options[@]}" \
        > >(sed -u "s/^/[JC2-L BLE $adapter] /" | tee "$capture/joycon2-left.log") 2>&1 &
    left_pid=$!
    echo "[ROUTE] JC2-R BLE adapter=$secondary mac=${expected_secondary:-unknown} socket=joycon-right.sock"
    "$jc2" "$secondary" --side right --socket joycon-right.sock "${options[@]}" \
        > >(sed -u "s/^/[JC2-R BLE $secondary] /" | tee "$capture/joycon2-right.log") 2>&1 &
    right_pid=$!
    left_socket="/run/pcble2gamepad/$PKEXEC_UID/joycon-left.sock"
    right_socket="/run/pcble2gamepad/$PKEXEC_UID/joycon-right.sock"
    for _ in {1..100}; do
        [[ -S $left_socket && -S $right_socket ]] && break
        kill -0 "$left_pid" 2>/dev/null && kill -0 "$right_pid" 2>/dev/null || break
        sleep .05
    done
    [[ -S $left_socket && -S $right_socket ]] || { echo 'One of the Joy-Con 2 backends failed to start' >&2; exit 1; }
    # A pair is a single logical controller: when one backend exits,
    # terminate its partner and let the trap restore both radios.
    set +e
    first_pid=
    if wait -n -p first_pid "$left_pid" "$right_pid"; then
        status=0
    else
        status=$?
    fi
    set -e
    first_label=unknown
    [[ ${first_pid:-} != "$left_pid" ]] || first_label=JC2-L
    [[ ${first_pid:-} != "$right_pid" ]] || first_label=JC2-R
    echo "[EXIT] profile=joycon2-pair first=$first_label pid=${first_pid:-none} status=$status"
    exit "$status"
fi
[[ $profile == pro || $profile == joycon-pair || $profile == joycon-mixed || $profile == joycon-mixed-reverse ]] || { echo 'Profile must be pro, joycon-pair, joycon-mixed, joycon2-left or joycon2-right' >&2; exit 2; }
jc2=
if [[ $profile == joycon-mixed || $profile == joycon-mixed-reverse ]]; then
    # One Classic and one BLE backend, with the radio assigned by side.
    for candidate in \
        "$script_dir/pcble2gamepad-joycon2-backend" \
        "$script_dir/../build/pcble2gamepad-joycon2-backend"; do
        if [[ -x "$candidate" ]]; then jc2=$candidate; break; fi
    done
    [[ -n "$jc2" ]] || { echo 'Build the Joy-Con 2 backend first.' >&2; exit 1; }
fi
if [[ $profile == joycon-pair || $profile == joycon-mixed || $profile == joycon-mixed-reverse ]]; then
    $desktop || { echo 'Joy-Con pair currently requires desktop mode' >&2; exit 2; }
    [[ -n $secondary && $secondary != "$adapter" ]] || { echo 'Joy-Con pair requires two distinct adapters' >&2; exit 2; }
fi
# Internally the Classic HID backend always uses $adapter. In the reverse
# hybrid that is the right-hand (GUI secondary) radio, not the BLE left radio.
if [[ $profile == joycon-mixed-reverse ]]; then
    ble_adapter=$adapter
    adapter=$secondary
    secondary=$ble_adapter
fi
[[ -z $reconnect || $profile == pro || $profile == joycon-mixed || $profile == joycon-mixed-reverse ]] || { echo 'Reconnect is implemented for the Pro Controller and the Joy-Con 1 (L) + Joy-Con 2 (R) profiles.' >&2; exit 2; }
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
        "$project_dir/pcble2gamepad-controller-backend" \
        "$project_dir/build/pcble2gamepad-controller-backend" \
        "$project_dir/build-studio/pcble2gamepad-controller-backend" \
        "$project_dir/build-check/pcble2gamepad-controller-backend" \
        "$project_dir/build-pro/pcble2gamepad-pro-poc"; do
        if [[ -x "$candidate" ]]; then app=$candidate; break; fi
    done
fi
[[ -n "$app" && -x "$app" ]] || { echo 'Build the controller backend first.' >&2; exit 1; }

# Wake-beacon HCI compatibility probe.
#
# This path deliberately does not install the pcble bluetoothd override
# or start a controller session. It briefly stops normal BlueZ, exercises
# the exact raw LE commands needed by beacon capture/transmit, then
# restores the service before returning.
if $wake_probe || $wake_capture || $wake_send; then
    bluetooth_was_active=false

    if systemctl is-active --quiet bluetooth; then
        bluetooth_was_active=true
    fi

    restore_probe_bluez() {
        trap - EXIT

        if $bluetooth_was_active; then
            systemctl start bluetooth || true
        fi
    }

    trap restore_probe_bluez EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM

    if $bluetooth_was_active; then
        systemctl stop bluetooth
    fi

    sleep .2

    if $wake_probe; then
        "$app" --wake-probe "$adapter"
    elif $wake_capture; then
        "$app" --wake-capture "$adapter"
    else
        "$app" --wake-send "$adapter" "$wake_send_controller" "$wake_send_data"
    fi

    exit $?
fi
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
# Keep the launcher output with the session logs; the desktop app does not
# show it.
exec > >(tee -a "$capture/runner.log") 2>&1
trap 'echo "runner: line $LINENO failed: $BASH_COMMAND"' ERR
monitor_pids=()
backend_pids=()
cleanup() {
    trap - EXIT INT TERM
    app_target=$(readlink -f "$app")
    jc2_target=
    [[ -n $jc2 ]] && jc2_target=$(readlink -f "$jc2")
    for pid in "${backend_pids[@]}"; do
        exe=$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)
        [[ -n $exe && ( $exe == "$app_target" || $exe == "$jc2_target" ) ]] && kill -TERM "$pid" 2>/dev/null || true
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
    elif [[ $profile == joycon-mixed || $profile == joycon-mixed-reverse ]]; then
        jc2_side=right; classic_side=left
        if [[ $profile == joycon-mixed-reverse ]]; then jc2_side=left; classic_side=right; fi
        jc2_options=(--side "$jc2_side" --vertical --socket "joycon-$jc2_side.sock" --desktop "$PKEXEC_UID")
        [[ -n $reconnect ]] && jc2_options+=(--reconnect)
        $verbose && jc2_options+=(--verbose)
        echo "[ROUTE] JC2-${jc2_side^^} BLE adapter=$secondary socket=joycon-$jc2_side.sock"
        "$jc2" "$secondary" "${jc2_options[@]}" \
            > >(sed -u "s/^/[JC2-${jc2_side^^} BLE $secondary] /" | tee "$capture/joycon2-$jc2_side.log") 2>&1 &
        jc2_pid=$!;backend_pids+=("$jc2_pid")
        jc2_socket="/run/pcble2gamepad/$PKEXEC_UID/joycon-$jc2_side.sock"
        for _ in {1..100}; do [[ -S $jc2_socket ]] && break; kill -0 "$jc2_pid" 2>/dev/null || break; sleep .05; done
        [[ -S $jc2_socket ]] || { echo 'Joy-Con 2 backend failed to start' >&2; wait "$jc2_pid" || true; exit 1; }
        echo "[ROUTE] JC1-${classic_side^^} CLASSIC adapter=$adapter socket=joycon-$classic_side.sock"
        "$app" "$adapter" "$sdp" --desktop "$PKEXEC_UID" --type "joycon-${classic_side:0:1}" "${backend_options[@]}" \
            > >(sed -u "s/^/[JC1-${classic_side^^} CLASSIC $adapter] /" | tee "$capture/joycon-$classic_side.log") 2>&1 &
        classic_pid=$!;backend_pids+=("$classic_pid")
        set +e
        first_pid=
        if wait -n -p first_pid "$classic_pid" "$jc2_pid"; then
            first_status=0
        else
            first_status=$?
        fi
        first_label=unknown
        [[ ${first_pid:-} != "$classic_pid" ]] || first_label="JC1-${classic_side^^} CLASSIC"
        [[ ${first_pid:-} != "$jc2_pid" ]] || first_label="JC2-${jc2_side^^} BLE"
        echo "[EXIT] profile=$profile first=$first_label pid=${first_pid:-none} status=$first_status"
        kill -TERM "$classic_pid" "$jc2_pid" 2>/dev/null
        wait "$classic_pid" 2>/dev/null; classic_status=$?
        wait "$jc2_pid" 2>/dev/null; ble_status=$?
        backend_pids=()
        set -e
        echo "[EXIT] final JC1-${classic_side^^}=$classic_status JC2-${jc2_side^^}=$ble_status"
        if (( first_status != 0 )); then
            echo "[DIAG] First backend failed. See its tagged output above and private session logs: $capture"
        fi
        exit "$first_status"
    else
        # Both Classic radios initialize concurrently, like the two
        # independent BLE radios in the working Joy-Con 2 profile. JC1-L
        # owns the global BlueZ default agent and the shared SDP record.
        echo "[ROUTE] JC1-L CLASSIC adapter=$adapter socket=joycon-left.sock role=default-agent+SDP"
        "$app" "$adapter" "$sdp" --desktop "$PKEXEC_UID" --type joycon-l --allow-adapter "$secondary" "${backend_options[@]}" \
            > >(sed -u "s/^/[JC1-L CLASSIC $adapter] /" | tee "$capture/joycon-left.log") 2>&1 &
        left_pid=$!;backend_pids+=("$left_pid")
        echo "[ROUTE] JC1-R CLASSIC adapter=$secondary socket=joycon-right.sock role=shared-agent+SDP"
        "$app" "$secondary" "$sdp" --desktop "$PKEXEC_UID" --type joycon-r --allow-adapter "$adapter" --shared-profile --shared-agent "${backend_options[@]}" \
            > >(sed -u "s/^/[JC1-R CLASSIC $secondary] /" | tee "$capture/joycon-right.log") 2>&1 &
        right_pid=$!;backend_pids+=("$right_pid")
        left_socket="/run/pcble2gamepad/$PKEXEC_UID/joycon-left.sock"
        right_socket="/run/pcble2gamepad/$PKEXEC_UID/joycon-right.sock"
        for _ in {1..100}; do
            [[ -S $left_socket && -S $right_socket ]] && break
            if ! kill -0 "$left_pid" 2>/dev/null || ! kill -0 "$right_pid" 2>/dev/null; then break; fi
            sleep .05
        done
        [[ -S $left_socket && -S $right_socket ]] || {
            echo 'Joy-Con 1 pair startup failed: one of the two IPC sockets is missing' >&2
            exit 1
        }
        echo '[ROUTE] JC1-L and JC1-R IPC ready (parallel startup)'
        set +e
        first_pid=
        if wait -n -p first_pid "$left_pid" "$right_pid"; then
            first_status=0
        else
            first_status=$?
        fi
        first_label=unknown
        [[ ${first_pid:-} != "$left_pid" ]] || first_label=JC1-L
        [[ ${first_pid:-} != "$right_pid" ]] || first_label=JC1-R
        echo "[EXIT] profile=joycon-pair first=$first_label pid=${first_pid:-none} status=$first_status"
        kill -TERM "$left_pid" "$right_pid" 2>/dev/null
        wait "$left_pid" 2>/dev/null; left_status=$?
        wait "$right_pid" 2>/dev/null; right_status=$?
        backend_pids=()
        set -e
        echo "[EXIT] final JC1-L=$left_status JC1-R=$right_status"
        exit "$first_status"
    fi
else
    echo 'Open Change Grip/Order. Commands: buttons HEX HEX HEX; sticks LX LY RX RY; release; status; quit.'
    echo 'The test ends after 10 minutes; no buttons are pressed automatically.'
    timeout --foreground --signal=TERM --kill-after=10s 600 "$app" "$adapter" "$sdp" --type pro "${backend_options[@]}" | tee "$capture/daemon.log"
fi
