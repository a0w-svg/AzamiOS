#!/usr/bin/env python3
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
        raw.append(0)  # filter type 0
        raw.extend(raw_rgb[y * w * 3 : (y + 1) * w * 3])

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
    idat = zlib.compress(raw, 6)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', idat) + chunk(b'IEND', b'')
    with open(png_path, 'wb') as f:
        f.write(png)

def main():
    sock_path = '/tmp/qemu-monitor.sock'
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

    print("Launching QEMU...", flush=True)
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

    for i in range(5):
        ppm_file = f'/tmp/desktop_{i}.ppm'
        png_file = f'build/captures/desktop_{i}.png'
        if os.path.exists(ppm_file):
            os.remove(ppm_file)
        cmd = f'screendump {ppm_file}\n'
        s.sendall(cmd.encode('utf-8'))
        time.sleep(0.3)
        if os.path.exists(ppm_file):
            ppm_to_png(ppm_file, png_file)
            print(f"Captured {png_file} ({os.path.getsize(png_file)} bytes)", flush=True)
        time.sleep(0.3)

    s.sendall(b'quit\n')
    s.close()
    proc.wait(timeout=5)
    print("Capture complete!", flush=True)

if __name__ == '__main__':
    main()
