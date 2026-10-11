#!/bin/bash
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"
make -j"$(nproc)" build/kernel.elf
make -C tools/linux toolchain openat2-probe
exec python3 -B tests/semantics/namespaces.py --suite openat2 "$@"
