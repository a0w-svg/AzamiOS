#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
test_dir=$(mktemp -d /tmp/azami-stdio-test.XXXXXX)
trap 'rm -f "$test_dir/test.o" "$test_dir/test"; rmdir "$test_dir"' EXIT HUP INT TERM
"${CC:-cc}" -O2 -fno-builtin -ffunction-sections -fdata-sections \
    -c tools/tests/stdio_buffer_test.c -o "$test_dir/test.o"
# Keep the tested libc private so it cannot interpose on the host runtime.
"${OBJCOPY:-objcopy}" --keep-global-symbol=main "$test_dir/test.o"
"${CC:-cc}" "$test_dir/test.o" -Wl,--gc-sections -o "$test_dir/test"
"$test_dir/test"
echo 'stdio buffered-read regressions passed'
