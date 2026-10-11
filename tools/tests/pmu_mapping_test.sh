#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
work=$(mktemp -d /tmp/azami-pmu-map.XXXXXX)
trap 'rm -rf "$work"' EXIT HUP INT TERM
cc -O2 -ffreestanding -ffunction-sections -fdata-sections -Iinclude -I. \
   -c arch/x86_64/cpu/pmu.c -o "$work/pmu.o"
cc -O2 -Wall -Wextra -Werror -Wl,--gc-sections tools/tests/pmu_mapping_test.c \
   "$work/pmu.o" -o "$work/test"
"$work/test"
