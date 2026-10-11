#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
make -j"$(nproc)" build/kernel.elf
make -C tools/linux perf-static perf-stat-probe
exec python3 -B tests/semantics/perf_stock.py "$@"
