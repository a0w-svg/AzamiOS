#!/bin/sh
# Builds an isolated boot image; never writes to the user's OS disk.
set -eu
cd "$(dirname "$0")/../.."
work=$(mktemp -d /tmp/azami-ac97-qemu.XXXXXX)
pid=
trap 'if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi' EXIT HUP INT TERM
echo "AC97 test artifacts: $work"
make CCACHE=0 -j4 build/kernel.elf > "$work/build.log" 2>&1
"${CC:-tools/linux/musl/bin/musl-gcc}" -O2 -static -idirafter /usr/include \
    -idirafter /usr/include/x86_64-linux-gnu \
    tools/tests/ac97_oss_probe.c -o "$work/probe"
mkdir -p "$work/root/sbin" "$work/root/tmp" "$work/iso/boot/limine"
cp "$work/probe" "$work/root/sbin/init.elf"
mke2fs -q -F -t ext2 -b 4096 -d "$work/root" "$work/iso/boot/initrd.ext2" 24M
cp build/kernel.elf "$work/iso/boot/kernel.elf"
cp limine.conf "$work/iso/boot/limine/limine.conf"
cp tools/limine/limine-bios-cd.bin tools/limine/limine-bios.sys "$work/iso/boot/limine/"
xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin -no-emul-boot \
    -boot-load-size 4 -boot-info-table -R -J -o "$work/test.iso" \
    "$work/iso" > "$work/iso.log" 2>&1
qemu-system-x86_64 -M q35 -m 1536M -smp 4 -accel tcg \
    -cpu max,+rdrand,+rdseed -nic none \
    -serial "file:$work/serial.log" -display none -no-reboot \
    -audiodev "wav,id=snd,path=$work/audio.wav,out.frequency=44100" \
    -device AC97,audiodev=snd -cdrom "$work/test.iso" \
    > "$work/qemu.log" 2>&1 &
pid=$!
for tick in $(seq 1 120); do
    if grep -q 'AC97 probe complete:' "$work/serial.log" 2>/dev/null; then break; fi
    if grep -q 'System halted' "$work/serial.log" 2>/dev/null; then break; fi
    if ! kill -0 "$pid" 2>/dev/null; then break; fi
    sleep 0.5
done
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
pid=
grep -E 'AC97|FAIL' "$work/serial.log" || true
if ! grep -q 'AC97 probe complete: 0 failed' "$work/serial.log"; then
    cat "$work/qemu.log"
    exit 1
fi
python3 - "$work/audio.wav" <<'PY'
import array
import sys
import wave
with wave.open(sys.argv[1], 'rb') as wav:
    assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (2, 2, 44100)
    pcm = array.array('h', wav.readframes(wav.getnframes()))
if sys.byteorder != 'little':
    pcm.byteswap()
# QEMU approximates the codec's dB gain with linear attenuation. Measure
# that gain, then verify burst amplitudes, stereo ordering, and duration.
peak_left = max(map(abs, pcm[0::2]), default=0)
peak_right = max(map(abs, pcm[1::2]), default=0)
assert 8000 <= peak_left <= 12000, peak_left
assert abs(peak_left - 4 * peak_right) <= 8, (peak_left, peak_right)
counts = [0, 0]
for left, right in zip(pcm[0::2], pcm[1::2]):
    for i, (lval, rval) in enumerate(((peak_left / 2, peak_right / 2), (peak_left, peak_right))):
        if abs(abs(left) - lval) <= 2 and abs(abs(right) - rval) <= 2:
            counts[i] += 1
assert all(abs(count - 8192) <= 16 for count in counts), counts
print('QEMU audio capture passed: both stereo bursts, 8192 frames each')
PY
