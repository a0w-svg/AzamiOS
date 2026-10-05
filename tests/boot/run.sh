#!/bin/bash
# ==============================================================================
# AzamiOS — QEMU Boot Test Runner
# File: tests/boot/run.sh
# ==============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

# Configuration defaults
ACCEL="${QEMU_ACCEL:-auto}"
CPU="${QEMU_CPU:-auto}"
SMP="${QEMU_SMP:-4}"
MEM="${QEMU_MEM:-1536M}"
BOOT_MODE="${BOOT_MODE:-bios}"
IMAGE_TYPE="${IMAGE_TYPE:-test_iso}"
TIMEOUT="${BOOT_TIMEOUT:-20}"
LOG_DIR="${LOG_DIR:-build/boot-tests}"

mkdir -p "$LOG_DIR"
TEST_ID="$(date +%s)_${BOOT_MODE}_${CPU}_smp${SMP}_${MEM}"
LOG_FILE="$LOG_DIR/${TEST_ID}.log"

# Select accelerator and CPU
if [ "$ACCEL" = "auto" ]; then
    if [ -r /dev/kvm ] && [ -w /dev/kvm ] && [ "$CPU" != "qemu64" ] && [ "$CPU" != "max" ]; then
        ACCEL_FLAG="-enable-kvm"
        [ "$CPU" = "auto" ] && CPU="host"
    else
        ACCEL_FLAG="-accel tcg,thread=multi,tb-size=512"
        [ "$CPU" = "auto" ] && CPU="max"
    fi
elif [ "$ACCEL" = "kvm" ]; then
    ACCEL_FLAG="-enable-kvm"
    [ "$CPU" = "auto" ] && CPU="host"
else
    ACCEL_FLAG="-accel tcg,thread=multi,tb-size=512"
    [ "$CPU" = "auto" ] && CPU="qemu64"
fi

CPU_FLAG="-cpu $CPU"
if [ "$CPU" = "max" ] && [ "$ACCEL" = "tcg" ]; then
    CPU_FLAG="-cpu max,+rdrand,+rdseed"
fi

# Select image
QEMU_MEDIA_ARGS=()
if [ "$IMAGE_TYPE" = "test_iso" ]; then
    IMAGE="build/linux-test/test.iso"
    if [ ! -f "$IMAGE" ] || [ build/kernel.elf -nt "$IMAGE" ] ||
       [ tools/linux/out/bin/azami-abi-probe -nt "$IMAGE" ]; then
        bash scripts/linux-test.sh >"$LOG_DIR/prepare.log" 2>&1 || {
            cat "$LOG_DIR/prepare.log"
            exit 1
        }
    fi
    QEMU_MEDIA_ARGS+=("-cdrom" "$IMAGE")
elif [ "$IMAGE_TYPE" = "iso" ]; then
    IMAGE="build/AzamiOS.iso"
    if [ ! -f "$IMAGE" ]; then
        make iso >/dev/null 2>&1
    fi
    QEMU_MEDIA_ARGS+=("-cdrom" "$IMAGE")
elif [ "$IMAGE_TYPE" = "hdd" ]; then
    IMAGE="hdd.img"
    if [ ! -f "$IMAGE" ]; then
        make hdd.img >/dev/null 2>&1
    fi
    QEMU_MEDIA_ARGS+=(
        "-drive" "file=$IMAGE,format=raw,if=none,id=drv0,cache=writeback"
        "-device" "virtio-blk-pci,drive=drv0,bootindex=1"
    )
fi

# Firmware selection for UEFI
FIRMWARE_ARGS=()
if [ "$BOOT_MODE" = "uefi" ]; then
    OVMF_CODE="/usr/share/OVMF/OVMF_CODE.fd"
    [ -f "$OVMF_CODE" ] || OVMF_CODE="/usr/share/edk2/ovmf/OVMF_CODE.fd"
    OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS.fd"
    [ -f "$OVMF_VARS_SRC" ] || OVMF_VARS_SRC="/usr/share/edk2/ovmf/OVMF_VARS.fd"
    
    if [ ! -f "$OVMF_CODE" ]; then
        echo "FAIL: OVMF firmware not found for UEFI boot test" >&2
        exit 1
    fi
    VARS_FILE="$LOG_DIR/${TEST_ID}_vars.fd"
    cp "$OVMF_VARS_SRC" "$VARS_FILE"
    FIRMWARE_ARGS+=(
        "-drive" "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
        "-drive" "if=pflash,format=raw,file=$VARS_FILE"
    )
fi

echo -n "  • Booting [$BOOT_MODE | accel=$ACCEL_FLAG cpu=$CPU | smp=$SMP mem=$MEM | $IMAGE_TYPE]... "

QEMU_CMD=(
    qemu-system-x86_64
    -M q35
    -m "$MEM"
    -smp "$SMP"
    $ACCEL_FLAG
    $CPU_FLAG
    "${FIRMWARE_ARGS[@]}"
    "${QEMU_MEDIA_ARGS[@]}"
    -serial "file:$LOG_FILE"
    -vga std
    -display none
    -no-reboot
    -no-shutdown
)

"${QEMU_CMD[@]}" >"$LOG_FILE.qemu" 2>&1 &
QEMU_PID=$!

START_TIME=$(date +%s)
ELAPSED=0
SUCCESS=0
while kill -0 "$QEMU_PID" 2>/dev/null; do
    CURRENT_TIME=$(date +%s)
    ELAPSED=$((CURRENT_TIME - START_TIME))
    if [ -f "$LOG_FILE" ]; then
        if grep -qE "(PANIC|Triple fault|Kernel panic|orders_mask/free_list divergence|free list/bitmap divergence)" "$LOG_FILE" 2>/dev/null; then
            SUCCESS=0
            break
        elif grep -q "probe complete: [0-9]* passed, 0 failed" "$LOG_FILE" 2>/dev/null; then
            SUCCESS=1
            break
        elif grep -q "All core microkernel subsystems initialized successfully" "$LOG_FILE" 2>/dev/null; then
            if [ "$IMAGE_TYPE" != "test_iso" ]; then
                SUCCESS=1
                break
            fi
        fi
    fi
    if [ "$ELAPSED" -ge "$TIMEOUT" ]; then
        break
    fi
    sleep 0.5
done

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

if [ "$SUCCESS" -eq 1 ]; then
    echo "PASS (${ELAPSED}s)"
    exit 0
else
    echo "FAIL (${ELAPSED}s)"
    echo "  ── Tail of $LOG_FILE ──"
    tail -n 20 "$LOG_FILE" 2>/dev/null || echo "(no log)"
    tail -n 5 "$LOG_FILE.qemu" 2>/dev/null || true
    exit 1
fi
