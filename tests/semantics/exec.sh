#!/bin/bash
# Compare real ELF/interpreter exec observations with Linux.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
make -j"$(nproc)" build/kernel.elf
make -C tools/linux toolchain exec-probe
exec python3 -B "$ROOT/tests/semantics/namespaces.py" --suite exec "$@"
