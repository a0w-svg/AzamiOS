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
#
# The initrd is built from scratch here rather than reusing build/initrd.ext2:
# the point is to isolate the ABI, so the image holds the binary under test,
# BusyBox if it has been built, and nothing else.
# ==============================================================================
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO" || exit 1

PAYLOAD="${1:-tools/linux/out/bin/azami-abi-probe}"
TIMEOUT="${LINUX_TEST_TIMEOUT:-60}"
WORK="${LINUX_TEST_WORK:-build/linux-test}"

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

rm -rf "$WORK"
mkdir -p "$WORK"/root/{sbin,bin,usr/bin,usr/sbin,tmp,etc,root} \
         "$WORK"/iso/boot/limine "$WORK"/iso/EFI/BOOT

# The payload runs as PID 1: the kernel spawns /sbin/init.elf at boot.
cp "$PAYLOAD" "$WORK/root/sbin/init.elf"
printf '127.0.0.1 localhost\nazamios\n' > "$WORK/root/etc/hosts"

# Bring BusyBox along when it exists, so a shell-script payload has a userland.
if [ -x tools/linux/out/bin/busybox ]; then
    cp tools/linux/out/bin/busybox "$WORK/root/bin/busybox"
    tools/linux/out/bin/busybox --list-full | while read -r applet; do
        name=${applet##*/}
        case "$applet" in
            sbin/*|usr/sbin/*) dir=usr/sbin ;;
            *)                 dir=usr/bin  ;;
        esac
        ln -sf ../../bin/busybox "$WORK/root/$dir/$name"
    done
fi

mke2fs -q -F -t ext2 -b 4096 -d "$WORK/root" "$WORK/initrd.ext2" 24M || exit 1

cp build/kernel.elf         "$WORK/iso/boot/kernel.elf"
cp "$WORK/initrd.ext2"      "$WORK/iso/boot/initrd.ext2"
cp limine.conf              "$WORK/iso/boot/limine/limine.conf"
cp "$LIMINE/limine-bios-cd.bin" "$LIMINE/limine-bios.sys" "$WORK/iso/boot/limine/"
[ -f "$LIMINE/BOOTX64.EFI" ] && cp "$LIMINE/BOOTX64.EFI" "$WORK/iso/EFI/BOOT/"

$MKISOFS -b boot/limine/limine-bios-cd.bin -no-emul-boot \
    -boot-load-size 4 -boot-info-table -R -J \
    -o "$WORK/test.iso" "$WORK/iso" >/dev/null 2>&1 || {
        echo "error: ISO build failed." >&2; exit 1; }
[ -x "$LIMINE/limine" ] && "$LIMINE/limine" bios-install "$WORK/test.iso" >/dev/null 2>&1

echo "  ▸  Booting $(basename "$PAYLOAD") as PID 1 (timeout ${TIMEOUT}s)..."
rm -f "$WORK/serial.log"
timeout "$TIMEOUT" qemu-system-x86_64 \
    -M q35 -m 1536M -smp 4 \
    -accel tcg,thread=multi,tb-size=512 -cpu max,+rdrand,+rdseed \
    -serial "file:$WORK/serial.log" -vga std -display none -no-reboot \
    -cdrom "$WORK/test.iso" >/dev/null 2>&1

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

grep -q "0 failed" "$WORK/serial.log" 2>/dev/null && exit 0
grep -qE "[1-9][0-9]* failed" "$WORK/serial.log" 2>/dev/null && exit 1
exit 0
