# AzamiOS

> **A Dual-Architecture (32-Bit x86 & 64-Bit x86_64 UEFI) Operating System Featuring a Unix-Like FHS Directory Hierarchy, VirtIO Paravirtualization, and a Modular Driver Framework.**

---

## 🌟 Overview

**AzamiOS** is a custom operating system designed from the ground up with modularity, clean architectural separation, and portability in mind. It bridges the gap between classic PC BIOS booting and modern 64-bit UEFI firmware, offering dual-target compilation from a single unified codebase.

Key technical highlights include:
- **Dual-Architecture Support**: Seamlessly builds for both 32-bit legacy Multiboot (`i686`) and 64-bit native UEFI Long Mode (`x86_64`).
- **Filesystem Hierarchy Standard (FHS)**: Full Unix-like directory tree populated dynamically at boot (`/bin`, `/sbin`, `/etc`, `/dev`, `/proc`, `/var`, `/home`).
- **Rich Hardware & Paravirtualization Drivers**: Native support for ATA IDE, SATA AHCI, Floppy DMA, and QEMU **VirtIO** block and networking devices.
- **Portable Core C Library (`lib/`)**: Strictly decoupled library subsystem validated against host compilers to guarantee zero ring-0 kernel leakage into userspace.

---

## 🏗️ Architecture Matrix

AzamiOS uses polymorphic memory types (`uintptr_t`, `size_t`) and hardware gates (`g_is_uefi`) to allow single-source kernel compilation across distinct hardware initialization flows:

| Feature / Target | 32-Bit Legacy Target (`ARCH=i686`) | 64-Bit UEFI Target (`ARCH=x86_64`) |
| :--- | :--- | :--- |
| **Firmware / Bootloader** | Legacy BIOS / GRUB Multiboot 1 | Native UEFI Firmware (PE32+ Executable) |
| **Entry Point** | `kernel/boot/boot.asm` | `kernel/boot/uefi_boot.c` (`efi_main`) |
| **CPU Operating Mode** | 32-Bit Protected Mode | 64-Bit Long Mode (PML4 Paging active) |
| **Linker Script** | `Link.ld` (Load at `0x100000`) | `Link64.ld` (Load at `0x400000`) |
| **Display Subsystem** | VGA Text Mode / Multiboot LFB | UEFI Graphics Output Protocol (GOP) Framebuffer |
| **Memory Map Discovery** | Multiboot Memory Map Table | UEFI `GetMemoryMap` & `ExitBootServices` |
| **Compiler Flags** | `-m32 -fno-pie` | `-m64 -mno-red-zone -mcmodel=large` |

---

## 📁 Unix-Like Filesystem Hierarchy (FHS)

AzamiOS enforces a standard Unix directory structure generated automatically during the build process and mounted via the Virtual Filesystem (VFS) and TarFS initrd image:

```text
/
├── bin/          # Essential user binaries (shell, ls, cat, write, clear)
├── sbin/         # System administration binaries (ifconfig, ping, arp, reboot, shutdown)
├── usr/
│   ├── bin/      # Secondary applications (wm window manager, cc compiler, glcube 3D demo)
│   ├── lib/      # Shared libraries and runtime archives
│   └── include/  # Standard system headers
├── etc/          # System configuration files (passwd, group, fstab, profile, motd, os-release)
├── dev/          # Hardware device nodes (/dev/hda, /dev/sda, /dev/vda, /dev/fd0)
├── proc/         # Process and system information virtual files
├── sys/          # Kernel hardware and driver attributes
├── var/
│   ├── log/      # System log files (messages, dmesg)
│   └── run/      # Runtime system state IDs
├── home/root/    # Default root user home directory (contains sample fib.c source)
├── tmp/          # Temporary scratchpad storage
└── mnt/ & media/ # Mount points for external storage volumes
```

### Automated Device Registration (`/dev`)
The VFS storage subsystem automatically probes hardware controllers at boot and populates device nodes inside `/dev`:
- `/dev/hda` — Primary ATA IDE hard drive
- `/dev/sda` — SATA AHCI storage volume
- `/dev/fd0` — Legacy DMA Floppy disk controller
- `/dev/vda` — QEMU **VirtIO Paravirtualized Block Device**

---

## 🚀 Building and Running

### 1. Toolchain Setup
AzamiOS provides an automated script to fetch, compile, and install a self-contained GNU cross-compiler toolchain (`binutils` + `gcc`) for 64-bit targeting:
```bash
# Builds and installs x86_64-elf cross toolchain to ~/opt/cross-x86_64
chmod +x scripts/build_toolchain.sh
./scripts/build_toolchain.sh
```

### 2. Compiling the OS

#### Build for 32-Bit Multiboot (`ARCH=i686`)
```bash
make clean all
```
*Outputs: `build/kernel.bin` and `build/initrd.tar`*

#### Build for 64-Bit UEFI (`ARCH=x86_64`)
```bash
make clean
make ARCH=x86_64 kernel.elf
```
*Outputs: `build/kernel.elf` (Verify format using `readelf -h build/kernel.elf`)*

### 3. Validating Library Decoupling
To ensure that portable C libraries (`lib/string`, `lib/stdlib`, `lib/fs`, `lib/net`) contain zero kernel ring-0 dependencies, execute the host compilation unit test:
```bash
make test-lib
```

### 4. Running in QEMU Emulator
```bash
# Run 32-bit kernel directly with attached VirtIO drive & initrd
make run

# Run bootable CD-ROM ISO image
make run-iso

# Run 64-bit UEFI target using OVMF firmware (requires OVMF.fd)
make run-uefi
```

---

## 🛠️ Subsystem Overview

### Virtual Filesystem (VFS) & Storage
- **VFS Layer**: Abstract node routing allowing seamless mounting of memory archives (`tarfs`) alongside physical disk file systems (`fat32`).
- **Directory Iterator**: POSIX-compliant `opendir`, `readdir`, and `closedir` implementation enabling dynamic folder traversal in userspace tools like `ls`.

### Networking Stack
- Implements a complete Ethernet, ARP, IPv4, ICMP, and UDP network protocol stack.
- Supports network interface cards including Intel e1000, Realtek 8139, Novell NE2000, AMD PCNet, and paravirtualized **VirtIO-Net**.

### Userspace Applications
- **`wm`**: Compositing window manager featuring a desktop environment, taskbar, terminal window, text notepad, and system monitor.
- **`cc`**: Self-hosted C compiler subset capable of compiling source files (e.g., `/home/root/fib.c`) directly inside the running OS.
- **`glcube`**: Software rendering demo spinning a 3D wireframe cube.
