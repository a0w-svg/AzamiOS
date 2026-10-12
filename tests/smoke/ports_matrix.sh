#!/bin/bash
# ==============================================================================
# AzamiOS — Port Smoke Test Matrix Runner
# File: tests/smoke/ports_matrix.sh
#
# Launches an ephemeral test VM running stock Linux ports:
# SQLite3, Lua, MicroPython, Dash, Toybox, and Zstandard.
# At the end of the run, captures telemetry from /proc/azami/enosys.
# ==============================================================================
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

make -j"$(nproc)" build/kernel.elf
make -C tools/linux busybox
make -C tests/smoke -j"$(nproc)" all

mkdir -p build/smoke
TASK_ROOT=$(mktemp -d "$REPO/build/smoke/ports.XXXXXX")
trap 'rm -rf "$TASK_ROOT"' EXIT
mkdir -p "$TASK_ROOT"/{bin,etc}

cp tests/smoke/ports-smoke.sh "$TASK_ROOT/etc/"
chmod +x "$TASK_ROOT/etc/ports-smoke.sh"
ln -sf busybox "$TASK_ROOT/bin/sh"

# Copy tested ports into ephemeral initrd
for bin in sqlite3 lua micropython dash toybox zstd; do
    if [ -f "tools/linux/out/bin/$bin" ]; then
        cp "tools/linux/out/bin/$bin" "$TASK_ROOT/bin/"
    else
        echo "error: missing tools/linux/out/bin/$bin" >&2
        exit 1
    fi
done

LINUX_TEST_ROOT="$TASK_ROOT" LINUX_TEST_WORK=build/smoke/vm-ports \
    LINUX_TEST_TIMEOUT="${SMOKE_TIMEOUT:-90}" \
    ./scripts/linux-test.sh build/smoke/ports-init

