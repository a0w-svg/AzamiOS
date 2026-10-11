#!/bin/bash
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"
make -C tools/linux toolchain
make -C tests/bench -j"$(nproc)" all
exec python3 tests/bench/run.py "$@"
