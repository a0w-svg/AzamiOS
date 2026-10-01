#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
test_dir=$(mktemp -d /tmp/azami-uart-test.XXXXXX)
trap 'rm -f "$test_dir/test"; rmdir "$test_dir"' EXIT HUP INT TERM
"${CC:-cc}" -O2 -ffreestanding -fno-builtin -ffunction-sections \
    -fdata-sections -I. -Iinclude tools/tests/uart_buffer_test.c \
    -Wl,--gc-sections -o "$test_dir/test"
"$test_dir/test"
echo 'UART ring-buffer regressions passed'
