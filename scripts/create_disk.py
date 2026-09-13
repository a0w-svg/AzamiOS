#!/usr/bin/env python3
"""
AzamiOS — Dual-Partition Disk Image Creator (MBR + Limine)
File: scripts/create_disk.py

Constructs a bootable dual-partition hard disk image (hdd.img):
  - Sector 0: MBR with Partition 1 (Boot, 64MB) and Partition 2 (Rootfs, 512MB)
  - Sectors 1..2047: 1MB reserved embedding gap for Limine stage2 bootloader
  - Partition 1 (LBA 2048..133119, 64MB ext2):
      /boot/kernel.elf, /boot/limine/limine.conf, /limine.conf, limine-bios.sys
  - Partition 2 (LBA 133120..1181695, 512MB ext2):
      Full userspace rootfs (/bin, /sbin, /etc, /usr, /music, etc.)
  - Installs Limine BIOS bootloader via 'limine bios-install'
"""

import os
import sys
import struct
import subprocess
import shutil

SECTOR_SIZE = 512
PART1_START_LBA = 2048          # 1 MB offset
PART1_SIZE_MB = 64
PART1_SECTORS = (PART1_SIZE_MB * 1024 * 1024) // SECTOR_SIZE

PART2_START_LBA = PART1_START_LBA + PART1_SECTORS
PART2_SIZE_MB = 512
PART2_SECTORS = (PART2_SIZE_MB * 1024 * 1024) // SECTOR_SIZE

TOTAL_SECTORS = PART2_START_LBA + PART2_SECTORS

def create_mbr(part1_lba, part1_sec, part2_lba, part2_sec):
    mbr = bytearray(512)
    # 0x00..0x1BD: Bootstrap code (standard x86 MBR stub)
    mbr[0:3] = b'\xfa\x31\xc0'  # cli; xor ax, ax

    def write_entry(offset, status, part_type, start_lba, sector_count):
        # status (1B), start_chs (3B), type (1B), end_chs (3B), start_lba (4B), count (4B)
        entry = struct.pack('<BBBBBBBBII',
                            status,
                            0x00, 0x02, 0x00, # start CHS dummy
                            part_type,
                            0xFF, 0xFF, 0xFF, # end CHS dummy
                            start_lba,
                            sector_count)
        mbr[offset:offset+16] = entry

    # Partition 1: Bootable (0x80), Linux Native (0x83)
    write_entry(0x1BE, 0x80, 0x83, part1_lba, part1_sec)
    # Partition 2: Inactive (0x00), Linux Native (0x83)
    write_entry(0x1CE, 0x00, 0x83, part2_lba, part2_sec)

    # MBR Boot Signature
    mbr[510] = 0x55
    mbr[511] = 0xAA
    return mbr

def main():
    build_dir = "build"
    hdd_img = "hdd.img"
    boot_root = os.path.join(build_dir, "boot_root")
    kernel_elf = os.path.join(build_dir, "kernel.elf")
    userland_build = "userland/build"

    if not os.path.isfile(kernel_elf):
        print(f"Error: {kernel_elf} not found. Run 'make' first.")
        sys.exit(1)

    print("  ↓  Preparing boot partition root (build/boot_root)...")
    os.makedirs(os.path.join(boot_root, "boot", "limine"), exist_ok=True)
    os.makedirs(os.path.join(boot_root, "EFI", "BOOT"), exist_ok=True)

    # Copy kernel
    shutil.copy2(kernel_elf, os.path.join(boot_root, "boot", "kernel.elf"))
    shutil.copy2(kernel_elf, os.path.join(boot_root, "kernel.elf"))

    # Write partition 1 limine.conf
    limine_conf = """# AzamiOS Partition 1 Limine Configuration
timeout: 0
serial: yes
terminal_background: 0x000000

/:AzamiOS (Partitioned Disk Boot)
    protocol: limine
    kernel_path: boot():/boot/kernel.elf
    kernel_cmdline: root=/dev/sata0p2 debug
    framebuffer_width:  1280
    framebuffer_height: 800
    framebuffer_bpp:    32

/:AzamiOS (Single-Core Debug)
    protocol: limine
    kernel_path: boot():/boot/kernel.elf
    kernel_cmdline: root=/dev/sata0p2 debug nosmp
    framebuffer_width:  1280
    framebuffer_height: 800
    framebuffer_bpp:    32
"""
    with open(os.path.join(boot_root, "limine.conf"), "w") as f:
        f.write(limine_conf)
    with open(os.path.join(boot_root, "boot", "limine", "limine.conf"), "w") as f:
        f.write(limine_conf)

    # Copy Limine stage binaries
    limine_dirs = ["tools/limine", "/usr/share/limine"]
    for ldir in limine_dirs:
        bios_sys = os.path.join(ldir, "limine-bios.sys")
        if os.path.isfile(bios_sys):
            shutil.copy2(bios_sys, os.path.join(boot_root, "boot", "limine", "limine-bios.sys"))
            shutil.copy2(bios_sys, os.path.join(boot_root, "limine-bios.sys"))
            break

    # Build Partition 1 ext2 image
    boot_ext2 = os.path.join(build_dir, "boot.ext2")
    if os.path.exists(boot_ext2):
        os.remove(boot_ext2)
    print(f"  ↓  Building boot partition ({PART1_SIZE_MB}M ext2)...")
    cmd_p1 = [
        "mke2fs", "-q", "-F", "-t", "ext2", "-b", "1024",
        "-d", boot_root, boot_ext2, f"{PART1_SIZE_MB}M"
    ]
    subprocess.run(cmd_p1, check=True)

    # Build Partition 2 ext2 image
    root_ext2 = os.path.join(build_dir, "rootfs.ext2")
    if os.path.exists(root_ext2):
        os.remove(root_ext2)
    print(f"  ↓  Building root partition ({PART2_SIZE_MB}M ext2 from {userland_build})...")
    cmd_p2 = [
        "mke2fs", "-q", "-F", "-t", "ext2", "-b", "4096",
        "-d", userland_build, root_ext2, f"{PART2_SIZE_MB}M"
    ]
    subprocess.run(cmd_p2, check=True)

    # Assemble raw partitioned disk image
    print(f"  ↓  Assembling partitioned disk image {hdd_img} ({TOTAL_SECTORS * SECTOR_SIZE // (1024*1024)}M)...")
    mbr = create_mbr(PART1_START_LBA, PART1_SECTORS, PART2_START_LBA, PART2_SECTORS)

    with open(hdd_img, "wb") as out_f:
        # Write MBR at sector 0
        out_f.write(mbr)

        # Pad reserved gap until sector 2048
        gap_bytes = (PART1_START_LBA - 1) * SECTOR_SIZE
        out_f.write(b'\x00' * gap_bytes)

        # Append Partition 1
        with open(boot_ext2, "rb") as p1_f:
            shutil.copyfileobj(p1_f, out_f)

        # Pad to Partition 2 start if needed
        cur_pos = out_f.tell()
        target_pos = PART2_START_LBA * SECTOR_SIZE
        if cur_pos < target_pos:
            out_f.write(b'\x00' * (target_pos - cur_pos))

        # Append Partition 2
        with open(root_ext2, "rb") as p2_f:
            shutil.copyfileobj(p2_f, out_f)

    # Install Limine BIOS bootloader
    print(f"  ↓  Installing Limine BIOS bootloader on {hdd_img}...")
    limine_bin = "tools/limine/limine"
    if not os.path.isfile(limine_bin):
        limine_bin = shutil.which("limine")

    if limine_bin and os.path.isfile(limine_bin):
        res = subprocess.run([limine_bin, "bios-install", hdd_img], capture_output=True, text=True)
        if res.returncode == 0:
            print("  ✓  Limine BIOS bootloader installed to MBR successfully.")
        else:
            print(f"  ⚠  Limine bios-install notice: {res.stderr.strip()}")
    else:
        print("  ⚠  Warning: Limine binary not found; skipping bios-install.")

    print(f"  ✓  Partitioned disk ready: {hdd_img}")
    print(f"     • Partition 1 (Boot): LBA {PART1_START_LBA}..{PART2_START_LBA-1} ({PART1_SIZE_MB} MB)")
    print(f"     • Partition 2 (Root): LBA {PART2_START_LBA}..{TOTAL_SECTORS-1} ({PART2_SIZE_MB} MB)")

if __name__ == "__main__":
    main()
