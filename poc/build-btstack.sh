#!/bin/bash
# Build the optional userspace-HCI Pro Controller experiment.
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)
btstack_commit=e38553977a25fb0b55b383c72c289be0975f422c
btstack_url=https://github.com/bluekitchen/btstack.git
source_dir=${BTSTACK_ROOT:-"$project_dir/build/btstack-src"}
build_dir=${BTSTACK_BUILD_ROOT:-"$project_dir/build/btstack-linux"}
patch_file="$project_dir/poc/btstack-preserve-local-bonding.patch"
output="$project_dir/build/pcble2gamepad-btstack-poc"

if [[ ! -d "$source_dir/.git" ]]; then
    [[ ! -e "$source_dir" ]] || {
        echo "BTSTACK_ROOT exists but is not a Git checkout: $source_dir" >&2
        exit 1
    }
    git clone --filter=blob:none --no-checkout "$btstack_url" "$source_dir"
    git -C "$source_dir" fetch --depth 1 origin "$btstack_commit"
    git -C "$source_dir" checkout --detach "$btstack_commit"
fi

actual_commit=$(git -C "$source_dir" rev-parse HEAD)
[[ $actual_commit == "$btstack_commit" ]] || {
    echo "Expected BTstack $btstack_commit, found $actual_commit" >&2
    exit 1
}

if git -C "$source_dir" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
    : # The isolated dependency checkout is already patched.
elif git -C "$source_dir" apply --check "$patch_file"; then
    git -C "$source_dir" apply "$patch_file"
else
    echo 'BTstack bonding patch does not apply cleanly.' >&2
    exit 1
fi

cmake -S "$source_dir/port/linux" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target btstack

cc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 \
    -I"$source_dir/port/linux" \
    -I"$source_dir/src" \
    -I"$source_dir/platform/linux" \
    -I"$source_dir/platform/posix" \
    -I"$project_dir/poc" \
    "$project_dir/poc/btstack-pro-controller.c" \
    "$project_dir/poc/protocol.c" \
    "$build_dir/libbtstack.a" \
    -lasound -lbluetooth -lm -pthread \
    -o "$output"

printf 'Built %s\n' "$output"
echo 'BTstack is licensed for personal, non-commercial use; see poc/BTSTACK-LICENSE.'
