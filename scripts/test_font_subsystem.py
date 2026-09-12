#!/usr/bin/env python3
"""
Test script for AzamiOS Font Subsystem:
- Boots QEMU with AzamiOS.iso and hdd.img
- Captures initial desktop (Settings showing installed fonts on drive)
- Sends CLI commands 'setfont' and 'setfont -p' to terminal
- Captures screenshots verifying on-disk font subsystem functionality
"""

import subprocess
import time
import socket
import struct
import os
import zlib

def ppm_to_png(ppm_path, png_path):
    with open(ppm_path, 'rb') as f:
        header = f.readline().strip()
        if header != b'P6':
            raise ValueError('Not P6 PPM')
        line = f.readline().strip()
        while line.startswith(b'#'):
            line = f.readline().strip()
        w, h = map(int, line.split())
        maxval = int(f.readline().strip())
        raw_rgb = f.read()

    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(raw_rgb[y * w * 3 : (y + 1) * w * 3])

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
    idat = zlib.compress(raw, 6)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', idat) + chunk(b'IEND', b'')
    with open(png_path, 'wb') as f:
        f.write(png)

def capture_screen(s, png_path):
    ppm_file = '/tmp/font_test.ppm'
    if os.path.exists(ppm_file):
        os.remove(ppm_file)
    s.sendall(f'screendump {ppm_file}\n'.encode('utf-8'))
    for _ in range(20):
        if os.path.exists(ppm_file) and os.path.getsize(ppm_file) > 1000:
            break
        time.sleep(0.1)
    if os.path.exists(ppm_file):
        ppm_to_png(ppm_file, png_path)
        print(f"Captured {png_path} ({os.path.getsize(png_path)} bytes)", flush=True)

def send_string(s, text):
    key_map = {
        ' ': 'spc',
        '-': 'minus',
        '_': 'shift-minus',
        '/': 'slash',
        '.': 'dot',
        '\n': 'ret',
        '\r': 'ret'
    }
    for ch in text:
        if ch in key_map:
            k = key_map[ch]
        elif ch.isupper():
            k = f"shift-{ch.lower()}"
        else:
            k = ch
        s.sendall(f"sendkey {k}\n".encode('utf-8'))
        time.sleep(0.04)

def main():
    sock_path = '/tmp/qemu-font-test.sock'
    if os.path.exists(sock_path):
        os.remove(sock_path)

    qemu_cmd = [
        'qemu-system-x86_64',
        '-M', 'q35',
        '-m', '1536M',
        '-smp', '4',
        '-enable-kvm', '-cpu', 'host',
        '-vga', 'std',
        '-display', 'none',
        '-monitor', f'unix:{sock_path},server,nowait',
        '-cdrom', 'build/AzamiOS.iso',
        '-drive', 'file=hdd.img,format=raw,if=none,id=drv0,cache=writeback',
        '-device', 'ide-hd,drive=drv0,bus=ide.0',
        '-netdev', 'user,id=net0,net=10.0.2.0/24,dhcpstart=10.0.2.15',
        '-device', 'e1000,netdev=net0',
        '-audiodev', 'pa,id=snd0',
        '-device', 'AC97,audiodev=snd0',
        '-device', 'intel-hda', '-device', 'hda-duplex,audiodev=snd0',
        '-device', 'ES1370,audiodev=snd0',
        '-device', 'virtio-rng-pci',
        '-device', 'pvpanic-pci',
        '-device', 'pci-serial',
        '-device', 'ich9-usb-uhci1',
        '-device', 'ich9-usb-ehci1',
        '-device', 'vmxnet3',
        '-serial', 'file:/tmp/qemu-serial.log'
    ]

    print("Launching QEMU for font subsystem validation...", flush=True)
    proc = subprocess.Popen(qemu_cmd)
    
    for _ in range(50):
        if os.path.exists(sock_path):
            break
        time.sleep(0.1)

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    time.sleep(0.5)
    s.recv(1024)

    print("Waiting 13 seconds for desktop environment...", flush=True)
    time.sleep(13)

    os.makedirs('build/captures', exist_ok=True)

    # 1. Capture base desktop showing Settings with font cards
    capture_screen(s, 'build/captures/font_desktop_initial.png')

    # 2. Press Alt+Tab to bring Terminal to the foreground
    print("Switching focus to Terminal with Alt+Tab...", flush=True)
    s.sendall(b'sendkey alt_l-tab\n')
    time.sleep(0.5)
    s.sendall(b'sendkey spc\n')
    time.sleep(0.5)

    # 3. Type 'setfont -l' into terminal
    print("Testing 'setfont -l' in terminal...", flush=True)
    send_string(s, 'setfont -l\n')
    time.sleep(1.2)
    capture_screen(s, 'build/captures/font_cli_list.png')

    # 4. Type 'setfont -i /usr/share/fonts/terminus_regular.azf' into terminal
    print("Testing 'setfont -i' in terminal...", flush=True)
    send_string(s, 'setfont -i /usr/share/fonts/terminus_regular.azf\n')
    time.sleep(1.2)
    capture_screen(s, 'build/captures/font_cli_info.png')

    # 5. Type 'setfont /usr/share/fonts/terminus_regular.azf' and cat /etc/font.conf
    print("Testing 'setfont <path>' in terminal...", flush=True)
    send_string(s, 'setfont /usr/share/fonts/terminus_regular.azf\n')
    time.sleep(1.0)
    send_string(s, 'cat /etc/font.conf\n')
    time.sleep(1.0)
    capture_screen(s, 'build/captures/font_applied.png')

    s.sendall(b'quit\n')
    s.close()
    proc.wait(timeout=5)
    print("Font subsystem validation complete!", flush=True)

if __name__ == '__main__':
    main()
