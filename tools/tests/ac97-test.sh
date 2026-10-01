#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
test_dir=$(mktemp -d /tmp/azami-ac97-test.XXXXXX)
trap 'rm -f "$test_dir/test"; rmdir "$test_dir"' EXIT HUP INT TERM
"${CC:-cc}" -O2 -ffreestanding -fno-builtin -ffunction-sections \
    -fdata-sections -Wall -Wextra -Wno-unused-parameter -I. -Iinclude \
    tools/tests/ac97_test.c -Wl,--gc-sections -o "$test_dir/test"
"$test_dir/test"
