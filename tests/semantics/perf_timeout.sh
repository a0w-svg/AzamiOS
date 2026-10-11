#!/bin/bash
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$REPO"
make -j"$(nproc)" build/kernel.elf
make -C tools/linux toolchain perf-probe
exec python3 -B tests/semantics/perf_timeout.py "$@"
