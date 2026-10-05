#!/bin/bash
# ==============================================================================
# AzamiOS — Linux-ABI regression run
# File: scripts/linux-test.sh
#
# Boots a headless VM whose PID 1 is a binary built by a stock Linux toolchain,
# and prints whatever it wrote to the serial console. The default payload is
# tools/linux/out/bin/azami-abi-probe; pass a different binary as $1 to run
# that instead.
#
#   scripts/linux-test.sh                       # run the ABI probe
#   scripts/linux-test.sh path/to/static-binary # run any static Linux binary
#   LINUX_TEST_CMDLINE="clocksource=hpet" scripts/linux-test.sh   # extra kernel args
#   LINUX_TEST_ROOT=path/to/root scripts/linux-test.sh payload  # extra initrd files
#
# Payloads must report "== probe complete: N passed, M failed ==". A missing
# verdict, timeout or kernel panic is a test failure, even if QEMU exits zero.
#
# The initrd is built from scratch here rather than reusing build/initrd.ext2:
# the point is to isolate the ABI, so the image holds the binary under test,
# BusyBox if it has been built, and nothing else.
# ==============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO" || exit 1

PAYLOAD="${1:-tools/linux/out/bin/azami-abi-probe}"
TIMEOUT="${LINUX_TEST_TIMEOUT:-60}"
WORK="${LINUX_TEST_WORK:-build/linux-test}"
EXTRA_ROOT="${LINUX_TEST_ROOT:-}"

if [ ! -x "$PAYLOAD" ]; then
    echo "error: no such executable: $PAYLOAD" >&2
    echo "       run 'make linux' first, or pass a static Linux binary." >&2
    exit 1
fi
if [ ! -f build/kernel.elf ]; then
    echo "error: build/kernel.elf missing — run 'make' first." >&2
    exit 1
fi

# mkisofs and xorriso take the same options for what we need; prefer whichever
# is present, matching the top-level Makefile's own fallback.
if command -v xorriso >/dev/null 2>&1; then MKISOFS="xorriso -as mkisofs"
elif command -v mkisofs >/dev/null 2>&1; then MKISOFS="mkisofs"
elif command -v genisoimage >/dev/null 2>&1; then MKISOFS="genisoimage"
else echo "error: need xorriso, mkisofs or genisoimage." >&2; exit 1
fi

LIMINE=tools/limine
[ -f "$LIMINE/limine-bios-cd.bin" ] || LIMINE=/usr/share/limine
if [ ! -f "$LIMINE/limine-bios-cd.bin" ]; then
    echo "error: no Limine bootloader files — run 'make iso' once to fetch them." >&2
    exit 1
fi

if [ -n "$EXTRA_ROOT" ] && [ ! -d "$EXTRA_ROOT" ]; then
    echo "error: LINUX_TEST_ROOT is not a directory: $EXTRA_ROOT" >&2
    exit 1
fi
mkdir -p "$WORK"
STAGING=$(mktemp -d "$WORK/staging.XXXXXX")
QEMU_PID=""
cleanup() {
    if [ -n "$QEMU_PID" ]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -rf "$STAGING"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$STAGING"/root/{sbin,bin,usr/bin,usr/sbin,tmp,etc,root} \
         "$STAGING"/iso/boot/limine "$STAGING"/iso/EFI/BOOT

# The payload runs as PID 1: the kernel spawns /sbin/init.elf at boot.
cp "$PAYLOAD" "$STAGING/root/sbin/init.elf"
printf '127.0.0.1 localhost\nazamios\n' > "$STAGING/root/etc/hosts"

# Bring BusyBox along when it exists, so a shell-script payload has a userland.
if [ -x tools/linux/out/bin/busybox ]; then
    cp tools/linux/out/bin/busybox "$STAGING/root/bin/busybox"
    tools/linux/out/bin/busybox --list-full | while read -r applet; do
        name=${applet##*/}
        case "$applet" in
            sbin/*|usr/sbin/*) dir=usr/sbin ;;
            *)                 dir=usr/bin  ;;
        esac
        ln -sf ../../bin/busybox "$STAGING/root/$dir/$name"
    done
fi
if [ -n "$EXTRA_ROOT" ]; then
    cp -a "$EXTRA_ROOT/." "$STAGING/root/"
fi

mke2fs -q -F -t ext2 -b 4096 -d "$STAGING/root" "$WORK/initrd.ext2" 24M || exit 1

cp build/kernel.elf         "$STAGING/iso/boot/kernel.elf"
cp "$WORK/initrd.ext2"      "$STAGING/iso/boot/initrd.ext2"
# LINUX_TEST_CMDLINE appends kernel parameters to every boot entry.
sed -e "s|^\(\s*kernel_cmdline:.*\)$|\1 ${LINUX_TEST_CMDLINE:-}|" \
    limine.conf > "$STAGING/iso/boot/limine/limine.conf"
cp "$LIMINE/limine-bios-cd.bin" "$LIMINE/limine-bios.sys" "$STAGING/iso/boot/limine/"
EFI_ARGS=()
if [ -f "$LIMINE/BOOTX64.EFI" ] && [ -f "$LIMINE/limine-uefi-cd.bin" ]; then
    cp "$LIMINE/BOOTX64.EFI" "$STAGING/iso/EFI/BOOT/"
    cp "$LIMINE/limine-uefi-cd.bin" "$STAGING/iso/boot/limine/"
    EFI_ARGS+=(-eltorito-alt-boot -e boot/limine/limine-uefi-cd.bin -no-emul-boot)
fi

$MKISOFS -b boot/limine/limine-bios-cd.bin -no-emul-boot \
    -boot-load-size 4 -boot-info-table -R -J \
    "${EFI_ARGS[@]}" -o "$WORK/test.iso" "$STAGING/iso" >/dev/null 2>&1 || {
        echo "error: ISO build failed." >&2; exit 1; }
if [ -x "$LIMINE/limine" ]; then
    "$LIMINE/limine" bios-install "$WORK/test.iso" >/dev/null 2>&1
fi

# KVM when this user can open it (an order of magnitude faster, and the CPU
# model is the real one), multi-threaded TCG otherwise — the same choice the
# top-level Makefile makes for `make run`.
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL="-enable-kvm -cpu host"
else
    ACCEL="-accel tcg,thread=multi,tb-size=512 -cpu max,+rdrand,+rdseed"
fi

echo "  ▸  Booting $(basename "$PAYLOAD") as PID 1 (timeout ${TIMEOUT}s)..."
rm -f "$WORK/serial.log"
# shellcheck disable=SC2086
timeout "$TIMEOUT" qemu-system-x86_64 \
    -M q35 -m 1536M -smp 4 \
    $ACCEL \
    -serial "file:$WORK/serial.log" -vga std -display none -no-reboot \
    -cdrom "$WORK/test.iso" >"$WORK/qemu.log" 2>&1 &
QEMU_PID=$!

# The payload is PID 1 and never exits, so the VM runs until the timeout
# unless stopped. Stop it as soon as the verdict line is out (plus a moment
# for trailing output), which turns a fixed 60 s wait into a few seconds.
while kill -0 "$QEMU_PID" 2>/dev/null; do
    if grep -aqE "== probe complete: [0-9]+ passed, [0-9]+ failed ==" "$WORK/serial.log" 2>/dev/null; then
        sleep 1
        kill "$QEMU_PID" 2>/dev/null || true
        break
    fi
    if grep -aqE 'PANIC|Triple fault|Kernel panic|orders_mask/free_list divergence|free list/bitmap divergence' "$WORK/serial.log" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        break
    fi
    sleep 0.5
done
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

echo "  ── guest output ────────────────────────────────────────────────────"
# Everything from the first line the payload printed; the kernel's own boot
# chatter ends at the scheduler hand-off.
sed -n '/Starting preemptive CFS scheduling/,$p' "$WORK/serial.log" 2>/dev/null | tail -n +2

# Report unimplemented syscalls separately: they are the usual reason a newly
# ported binary misbehaves, and the kernel logs each number once.
if grep -q "unimplemented syscall" "$WORK/serial.log" 2>/dev/null; then
    echo "  ── unimplemented syscalls ──────────────────────────────────────────"
    grep "unimplemented syscall" "$WORK/serial.log"
fi

if grep -aqE 'PANIC|Triple fault|Kernel panic|orders_mask/free_list divergence|free list/bitmap divergence|== probe complete: [0-9]+ passed, [1-9][0-9]* failed ==' "$WORK/serial.log"; then
    exit 1
fi
if grep -aqE '== probe complete: [1-9][0-9]* passed, 0 failed ==' "$WORK/serial.log"; then
    exit 0
fi
echo "error: guest did not report a successful probe verdict (timeout, crash or QEMU failure)." >&2
echo "       logs: $WORK/serial.log and $WORK/qemu.log" >&2
exit 1
