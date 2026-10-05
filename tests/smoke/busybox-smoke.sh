#!/bin/sh
set -eu

check() {
    name=$1
    shift
    "$@"
    printf 'PASS: BusyBox %s\n' "$name"
}

check sh sh -c 'ls -la / && free && ps'
check ls ls -la /
check ps ps
check cat cat /etc/hosts
check mkdir mkdir /tmp/smoke-dir
printf 'busybox file I/O\n' > /tmp/smoke-dir/data
check cat-data sh -c 'test "$(cat /tmp/smoke-dir/data)" = "busybox file I/O"'
check rm rm -r /tmp/smoke-dir
test ! -e /tmp/smoke-dir
check free free
check top top -b -n 1
check uname uname -a
check env env
check env-inheritance sh -c 'test "$(env | grep "^AZAMI_SMOKE_TOKEN=")" = "AZAMI_SMOKE_TOKEN=loader-environment"'
printf 'BusyBox smoke complete: 0 failed\n'
