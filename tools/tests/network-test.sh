#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
work=$(mktemp -d /tmp/azami-network-test.XXXXXX)
trap 'rm -f "$work/test"; rmdir "$work"' EXIT HUP INT TERM
"${CC:-cc}" -O2 -ffreestanding -fno-builtin -ffunction-sections \
    -fdata-sections -Wall -Wextra -Wno-unused-parameter -I. -Iinclude \
    tools/tests/udp_test.c tools/tests/net_buf_ref_test.c kernel/net/net.c \
    -pthread -Wl,--gc-sections -o "$work/test"
"$work/test"
