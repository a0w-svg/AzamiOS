# AzamiOS

> **A 64-Bit x86_64 Operating System Featuring a Unix-Like FHS Directory Hierarchy, POSIX-Compliant Userspace, VirtIO Paravirtualization, and a Modular Driver Framework.**

---

## 🌟 Overview

**AzamiOS** is a custom operating system built from the ground up with modularity, clean architectural separation, and POSIX compliance in mind. It is designed for 64-bit `x86_64` Long Mode with a complete kernel, hardware abstraction layer, network stack, custom libc, and a rich set of userland utilities.

Key technical highlights include:
- **64-bit Native Architecture**: Designed purely for 64-bit `x86_64` Long Mode with PML4 paging.
- **Filesystem Hierarchy Standard (FHS)**: Full Unix-like directory tree populated dynamically at boot (`/bin`, `/sbin`, `/etc`, `/dev`, `/proc`, `/var`, `/home`).
- **Rich Hardware & Paravirtualization Drivers**: ATA IDE, Floppy DMA/FDC, VirtIO block/net/GPU/RNG, Intel e1000, Realtek RTL8139, AMD PCNet, AC'97/Intel HDA audio, BGA display, and DRM.
- **POSIX-Compliant Custom libc**: A full freestanding C runtime — `stdio`, `stdlib`, `string`, `math`, `pthread`, `socket`, `setjmp`, `wchar`, and more — with zero kernel leakage into userspace.
- **Compositing GUI**: Azami Window Manager (`azwm`) with a desktop environment, taskbar, terminal emulator, text editor, file manager, system monitor, and X11 client protocol support.

---

## 🏗️ Architecture Overview

| Feature | 64-Bit Target (`x86_64`) |
| :--- | :--- |
| **Bootloader** | Limine (UEFI/BIOS compatible) |
| **CPU Mode** | 64-Bit Long Mode (PML4 Paging) |
| **Linker Script** | `scripts/kernel.ld` |
| **Display Subsystem** | Framebuffer / VirtIO GPU / DRM |
| **Compiler** | `x86_64-elf-gcc` cross-compiler |
| **Compiler Flags** | `-m64 -mno-red-zone -mcmodel=kernel -ffreestanding` |

---

## 📁 Unix-Like Filesystem Hierarchy (FHS)

AzamiOS enforces a standard Unix directory structure generated automatically during the build process:

```text
/
├── bin/          # Core user utilities (sh, ls, cat, cp, mv, rm, grep, awk, sort, …)
├── sbin/         # System binaries (init, dhcpcd, ifconfig, ping, reboot, shutdown)
├── usr/
│   ├── bin/      # Additional applications (window manager, compiler, X11 apps)
│   ├── lib/      # Shared libraries and runtime archives
│   └── include/  # Standard system headers (POSIX-compliant)
├── etc/          # System configuration (passwd, group, fstab, profile, os-release)
├── dev/          # Hardware device nodes (/dev/hda, /dev/sda, /dev/vda, /dev/fd0)
├── proc/         # Process and system information virtual files
├── sys/          # Kernel hardware and driver attributes
├── var/
│   ├── log/      # System log files (messages, dmesg)
│   └── run/      # Runtime state
├── home/azami/   # Default user home directory
├── tmp/          # Temporary scratchpad storage
└── mnt/ & media/ # Mount points for external storage volumes
```

---

## 📦 Userland Utilities

AzamiOS ships an extensive set of POSIX shell utilities:

### File & Text Processing
| Utility | Description |
| :--- | :--- |
| `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `rmdir` | Core file operations |
| `grep`, `awk`, `sed`, `sort`, `uniq` | Text search and transformation |
| `head`, `tail`, `wc`, `cut`, `paste` | Text extraction and column manipulation |
| `fold`, `expand`, `unexpand` | Line folding and tab/space conversion |
| `comm`, `cmp`, `diff` | File comparison |
| `tee`, `split` | Stream splitting |
| `tr`, `strings` | Character translation and string extraction |
| `find`, `xargs` | File search and parallel execution |
| `tar` | Archive creation and extraction |

### System & Environment
| Utility | Description |
| :--- | :--- |
| `sh` | POSIX shell with pipes, redirection, and scripting |
| `expr`, `test` | Shell arithmetic and logical expressions |
| `env`, `printenv`, `export` | Environment variable management |
| `date`, `time`, `cal` | Date, time, and calendar |
| `id`, `whoami` | User identity |
| `basename`, `dirname` | Path manipulation |
| `seq`, `yes`, `true`, `false` | Utility generators |
| `du`, `stat` | Disk usage and file metadata |

### Networking
| Utility | Description |
| :--- | :--- |
| `ifconfig`, `ping` | Network interface configuration and diagnostics |
| `nc` | Netcat — TCP/UDP connections |
| `curl` | HTTP/HTTPS file transfer |
| `httpd` | Lightweight HTTP server |
| `dhcpcd` | DHCP client daemon |

### Filesystem & Permissions
| Utility | Description |
| :--- | :--- |
| `chmod`, `chown`, `ln` | File permissions and linking |
| `getfacl`, `setfacl` | POSIX Access Control List management |

### GUI Applications
| Application | Description |
| :--- | :--- |
| `azwm` | Compositing Azami Window Manager + desktop environment |
| `terminal` | Terminal emulator |
| `texteditor` | Graphical text editor |
| `filemanager` | Graphical file browser |
| `sysmon` | System resource monitor |
| `calculator`, `clock`, `paint` | Productivity and creative apps |
| `xclock`, `xcalc`, `xeyes`, `xcursor` | X11 protocol client applications |
| `minesweeper`, `2048`, `snake` | Games |

---

## 📚 Userspace C Library (libc)

AzamiOS includes a fully custom freestanding C runtime (`userland/libc/`) targeting POSIX compliance:

### Headers & Subsystems

| Header(s) | Subsystem |
| :--- | :--- |
| `stdio.h` | `printf`, `scanf`, `fopen`, `fdopen`, `popen`, `dprintf`, `getline`, `tmpfile`, … |
| `stdlib.h` | `malloc`, `free`, `atoi`, `qsort`, `posix_memalign`, `mkstemp`, `drand48` suite, … |
| `string.h`, `strings.h` | `memcpy`, `strcpy`, `strlcpy`, `strtok`, `memmove`, `strsignal`, `ffsl`, … |
| `math.h` | Full trig, inverse trig, hyperbolic, log, float variants (`sqrtf`, `sinf`, …) |
| `pthread.h` | Threads, mutexes, condvars, rwlocks, spinlocks, barriers, TLS keys |
| `setjmp.h` | `setjmp`, `longjmp`, `sigsetjmp`, `siglongjmp` (x86_64 asm) |
| `unistd.h` | `read`, `write`, `fork`, `exec`, `pipe`, `dup2`, `*at` family, … |
| `fcntl.h` | `open`, `openat`, `posix_fadvise`, `posix_fallocate`, `AT_*` constants |
| `wchar.h`, `wctype.h` | Wide character manipulation and classification |
| `sys/stat.h` | `stat`, `fstat`, `fstatat`, `chmod`, `fchmodat`, `futimens`, `utimensat`, … |
| `sys/socket.h`, `netdb.h` | BSD socket API, DNS resolution |
| `sys/times.h` | `times()`, `struct tms` |
| `sys/statvfs.h` | `statvfs()`, `fstatvfs()` |
| `sys/sysmacros.h` | `major()`, `minor()`, `makedev()` |
| `shadow.h` | Shadow password database (`getspnam`, `getspent`) |
| `tar.h`, `cpio.h` | POSIX archive format constants |
| `regex.h`, `fnmatch.h`, `glob.h` | Regular expressions, filename patterns, globbing |
| `pwd.h`, `grp.h` | User and group database access |
| `poll.h`, `sys/select.h` | I/O multiplexing |
| `syslog.h` | System logging |
| `semaphore.h` | POSIX semaphores |
| `spawn.h` | `posix_spawn` and `posix_spawnp` |

---

## 🚀 Building and Running

### 1. Toolchain Setup

```bash
# Builds and installs x86_64-elf cross-compiler toolchain to ~/opt/cross-x86_64
chmod +x scripts/build_toolchain.sh
./scripts/build_toolchain.sh
```

### 2. Build the OS

```bash
make clean
make all
```

*Outputs: `build/kernel.elf` (kernel) and `build/bin/*.elf` (userland binaries)*

### 3. Build libc Only

```bash
make -C userland/libc
```

### 4. Build Userland Apps Only

```bash
make -C userland apps
```

### 5. Run in QEMU

```bash
# Run 64-bit kernel with VirtIO drive and initrd
make run

# Run bootable CD-ROM ISO
make run-iso

# Run with UEFI firmware (requires OVMF.fd)
make run-uefi
```

---

## 🛠️ Subsystem Overview

### Kernel

- **Memory Management**: Physical Memory Manager (PMM), slab-based `kmalloc`, x86_64 PML4 virtual memory management with `mmap`/`munmap` support.
- **Scheduler**: Preemptive multi-tasking with per-CPU run queues, ELF binary loader, and signal delivery.
- **System Calls**: Linux-compatible system call ABI (`syscall` instruction, x86_64 System V AMD64 calling convention) with over 80 registered handlers.
- **IPC**: Inter-process communication via pipes, shared memory, and the AzamiOS IPC message bus.
- **Security**: Capability-based security model with POSIX ACL enforcement.

### Hardware Abstraction Layer (HAL)

- **PCI Bus**: Enumeration, capability detection, MSI/INTx routing.
- **IRQ Subsystem**: I/O APIC + LAPIC interrupt routing and SMP support.
- **VirtIO**: PCI-based VirtIO transport with virtqueue management (block, network, GPU, RNG).

### Drivers

| Category | Drivers |
| :--- | :--- |
| **Block Storage** | ATA IDE, Floppy FDC (DMA), VirtIO Block |
| **Networking** | Intel e1000, Realtek RTL8139, AMD PCNet, VirtIO Net |
| **Video** | BGA Display, VirtIO GPU, Framebuffer DRM |
| **Audio** | AC'97, Intel HDA (High Definition Audio), PC Speaker |
| **Misc** | RTC, VirtIO RNG, ACPI/IOAPIC/power management |
| **Input** | PS/2 keyboard and mouse |

### Networking Stack

- Full Ethernet, ARP, IPv4, ICMPv4, UDP, and TCP implementation.
- DHCP client for automatic address assignment.
- BSD socket API (`socket`, `bind`, `connect`, `send`, `recv`, `select`).

### Virtual Filesystem (VFS)

- Abstract VFS node layer with mount point support.
- Drivers: **ext2**, **FAT32**, **ProcFS**, **DevFS**, **PipeFS**, **TarFS initrd**.
- POSIX-compliant `opendir`, `readdir`, `closedir`, `fstatat`, `getdents64`.

### GUI Subsystem (Azami Window Manager)

- **Compositor**: Software compositing with double-buffering and alpha blending.
- **Desktop Protocol**: Custom IPC protocol between `azwm` compositor and client applications.
- **Widgets**: Button, text input, list view, canvas, scrollable containers.
- **X11 Compatibility**: Basic X11 client protocol for portable GUI applications.

---

## 📄 License

AzamiOS is an independent open-source project. See [LICENSE](LICENSE) for details.
