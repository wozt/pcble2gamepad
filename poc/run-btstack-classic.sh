#!/bin/bash
# Run the isolated BTstack/HCI_USER_CHANNEL bonding experiment.
set -euo pipefail

[[ $EUID == 0 ]] || { echo 'Run through pkexec or sudo.' >&2; exit 1; }
[[ $# -ge 1 && $1 =~ ^hci[0-9]+$ ]] || {
    echo "Usage: $0 hciN [--reset-bond] [--passive]" >&2
    exit 2
}

script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
adapter=$1
shift
reset_bond=false
passive=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --reset-bond) reset_bond=true; shift ;;
        --passive) passive=true; shift ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

app="$project_dir/build/pcble2gamepad-btstack-poc"
[[ -x $app ]] || { echo 'Run ./poc/build-btstack.sh first.' >&2; exit 1; }
[[ -d /sys/class/bluetooth/$adapter ]] || { echo 'Adapter not present.' >&2; exit 1; }
systemctl is-active --quiet bluetooth || { echo 'Bluetooth service must already be active.' >&2; exit 1; }

for controller in /sys/class/bluetooth/hci*; do
    [[ -z "$(hcitool -i "${controller##*/}" con | tail -n +2)" ]] || {
        echo 'Connected Bluetooth devices; disconnect them before the userspace-HCI test.' >&2
        exit 1
    }
done

owner=${PKEXEC_UID:-${SUDO_UID:-0}}
capture=$(mktemp -d "/tmp/pcble2gamepad-btstack-${owner}-XXXXXXXX")
chmod 700 "$capture"
state_dir=/var/lib/pcble2gamepad/btstack
mkdir -p "$state_dir"
chmod 700 /var/lib/pcble2gamepad "$state_dir"
tlv="$state_dir/${adapter}.tlv"
arguments=(--device-id "${adapter#hci}" --tlv "$tlv" --logfile "$capture/hci.pklg")
$reset_bond && arguments+=(--reset-bond)
$passive && arguments+=(--passive)
[[ ! -e $tlv ]] || chmod 600 "$tlv"
umask 077

restored=false
cleanup() {
    trap - EXIT INT TERM
    if ! $restored; then
        systemctl restart bluetooth
        restored=true
    fi
    [[ ! -e $tlv ]] || chmod 600 "$tlv"
    find "$capture" -type f -exec chmod 600 {} +
    if [[ $owner != 0 ]]; then chown -R "$owner" "$capture"; fi
    echo "Normal BlueZ restored. Private capture: $capture"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

echo "Stopping BlueZ and opening $adapter through HCI_CHANNEL_USER."
systemctl stop bluetooth
hciconfig "$adapter" down

echo 'Open Controllers -> Change Grip/Order for a fresh pair.'
echo 'On a later run, a stored bond triggers a controller-initiated reconnect.'
"$app" "${arguments[@]}" | tee "$capture/backend.log"
