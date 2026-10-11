#!/bin/bash
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"
mkdir -p build/semantics
make -j"$(nproc)" build/kernel.elf
make -C tools/linux toolchain namespace-probe
exec python3 -B tests/semantics/namespaces.py "$@"
