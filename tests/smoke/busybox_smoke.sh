#!/bin/bash
# W3.7: real Linux applets and both ELF interpreters in an ephemeral initrd.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

make -j"$(nproc)" build/kernel.elf
make -C tools/linux busybox shared-musl
make -C userland/libc
make -C tests/smoke -j"$(nproc)" all check

mkdir -p build/smoke
TASK_ROOT=$(mktemp -d "$REPO/build/smoke/root.XXXXXX")
trap 'rm -rf "$TASK_ROOT"' EXIT
mkdir -p "$TASK_ROOT"/{bin,lib,etc}
cp tests/smoke/busybox-smoke.sh "$TASK_ROOT/etc/"
ln -s busybox "$TASK_ROOT/bin/sh"
cp build/smoke/{native-dynamic-probe,musl-dynamic-probe} "$TASK_ROOT/bin/"
cp build/smoke/ld-azami.so userland/libc/libc.so "$TASK_ROOT/lib/"
cp tools/linux/musl/lib/libc.so "$TASK_ROOT/lib/ld-musl-x86_64.so.1"
cp build/smoke/native/libsmoke_{start,plugin}.so "$TASK_ROOT/lib/"
cp build/smoke/musl/libsmoke_start.so "$TASK_ROOT/lib/libsmoke_start-musl.so"
cp build/smoke/musl/libsmoke_plugin.so "$TASK_ROOT/lib/libsmoke_plugin-musl.so"

LINUX_TEST_ROOT="$TASK_ROOT" LINUX_TEST_WORK=build/smoke/vm \
    LINUX_TEST_TIMEOUT="${SMOKE_TIMEOUT:-90}" \
    ./scripts/linux-test.sh build/smoke/smoke-init
