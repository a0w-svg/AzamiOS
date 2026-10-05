#!/bin/bash
# ==============================================================================
# AzamiOS — QEMU Boot Test Matrix
# File: tests/boot/matrix.sh
# ==============================================================================
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

echo "======================================================================"
echo "  AzamiOS Boot Regression Matrix"
echo "======================================================================"

RUN="tests/boot/run.sh"
TOTAL=0
PASSED=0
FAILED=0

run_case() {
    local accel="$1"
    local cpu="$2"
    local smp="$3"
    local mem="$4"
    local mode="$5"
    local img="$6"
    local timeout="${7:-20}"

    TOTAL=$((TOTAL + 1))
    if QEMU_ACCEL="$accel" QEMU_CPU="$cpu" QEMU_SMP="$smp" QEMU_MEM="$mem" \
       BOOT_MODE="$mode" IMAGE_TYPE="$img" BOOT_TIMEOUT="$timeout" "$RUN"; then
        PASSED=$((PASSED + 1))
    else
        FAILED=$((FAILED + 1))
    fi
}

# 1. KVM matrix (fast, full host features)
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    echo "── 1. KVM Boot Tests (Host CPU) ──────────────────────────────────"
    run_case "kvm" "host" "1" "512M"  "bios" "test_iso" 15
    run_case "kvm" "host" "4" "512M"  "bios" "test_iso" 15
    run_case "kvm" "host" "4" "1536M" "bios" "test_iso" 15
    run_case "kvm" "host" "4" "1536M" "uefi" "test_iso" 25
else
    echo "SKIP: four KVM configurations (/dev/kvm unavailable)"
fi

# 2. TCG matrix with default QEMU CPU (qemu64)
echo "── 2. TCG Boot Tests: qemu64 (Emulated baseline CPU) ─────────────"
run_case "tcg" "qemu64" "1" "512M"  "bios" "test_iso" 20
run_case "tcg" "qemu64" "4" "512M"  "bios" "test_iso" 20
run_case "tcg" "qemu64" "4" "1536M" "bios" "test_iso" 20

# 3. TCG matrix with max CPU (all emulated CPU features, XSAVE/AVX512/PKU)
echo "── 3. TCG Boot Tests: max (Emulated feature-rich CPU) ────────────"
run_case "tcg" "max" "1" "512M"  "bios" "test_iso" 20
run_case "tcg" "max" "4" "1536M" "bios" "test_iso" 20
run_case "tcg" "max" "4" "1536M" "uefi" "test_iso" 30

echo "======================================================================"
echo "  Matrix Results: $PASSED / $TOTAL passed ($FAILED failed)"
echo "======================================================================"

[ "$FAILED" -eq 0 ]
