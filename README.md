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
| `tr`, `strings`, `od` | Character translation, string extraction, and octal/hex dump |
| `pr` | Text pagination and header columnation for printing |
| `tsort` | Topological sort for directed dependency graphs |
| `find`, `xargs` | File search and parallel execution |
| `tar` | Archive creation and extraction |
| `truncate` | Shrink or extend the size of files |
| `mktemp` | Safe temporary file and directory creation |

### System & Environment
| Utility | Description |
| :--- | :--- |
| `sh` | POSIX shell with pipes, redirection, and scripting |
| `expr`, `test` | Shell arithmetic and logical expressions |
| `env`, `printenv`, `export` | Environment variable management |
| `date`, `time`, `cal`, `timeout` | Date, time, calendar, and timed command execution |
| `id`, `whoami`, `who`, `logname`, `groups`, `users` | User identity, active session tracking, and group memberships |
| `hostname` | View or configure system host and domain name |
| `sync` | Synchronize cached writes to persistent storage |
| `nice`, `renice` | Process scheduling priority inspection and modification |
| `kill`, `killall`, `pgrep`, `pkill` | Signal dispatching, process termination, and PID lookup |
| `tty` | Terminal device check |
| `basename`, `dirname`, `pathchk`, `readlink`, `realpath` | Path resolution, validity checking, and symlink canonicalization |
| `seq`, `yes`, `true`, `false` | Utility generators |
| `du`, `stat`, `cksum` | Disk usage, file metadata, and CRC32 checksums |
| `nohup`, `nl` | Process execution immune to hangups and line numbering |
| `chroot` | Run command or interactive shell with new root directory |

### Networking
| Utility | Description |
| :--- | :--- |
| `ifconfig`, `ping` | Network interface configuration and diagnostics |
| `nc` | Netcat — TCP/UDP connections |
| `curl` | HTTP/HTTPS file transfer |
| `httpd` | Lightweight HTTP server |
| `dhcpcd` | DHCP client daemon |
| `nslookup` | DNS name resolution lookup |

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
| `stdio.h` | `printf`, `scanf`, `fopen`, `fdopen`, `popen`, `dprintf`, `asprintf`, `getline`, `tmpfile`, … |
| `stdlib.h` | `malloc`, `free`, `atoi`, `qsort`, `posix_memalign`, `mkstemp`, `mkdtemp`, `drand48` suite, … |
| `string.h`, `strings.h`, `memory.h` | `memcpy`, `strcpy`, `strlcpy`, `strtok`, `memmove`, `strsignal`, `ffsl`, … |
| `ctype.h` | Character classification & conversion (`isalnum`, `isblank`, `isascii`, `toascii`, …) |
| `utmpx.h`, `utmp.h` | POSIX user accounting database (`getutxent`, `pututxline`, `logwtmp`, `login_tty`, …) |
| `getopt.h` | POSIX short options & GNU long options parsing (`getopt_long`, `getopt_long_only`) |
| `sys/resource.h` | Process resource limits and scheduling priority (`getrlimit`, `getpriority`, `setpriority`) |
| `sys/wait.h` | Process wait status macros (`WIFEXITED`, `WIFSTOPPED`, `WIFCONTINUED`, `waitid`) |
| `sys/time.h` | Time structures and interval timers (`gettimeofday`, `setitimer`, `utimes`) |
| `signal.h` | Signal manipulation (`sigaction`, `sigprocmask`, `sigpending`, `sigsuspend`, `sigwait`) |
| `limits.h` | System & POSIX limits (`PATH_MAX`, `NAME_MAX`, `OPEN_MAX`, `PIPE_BUF`, …) |
| `endian.h`, `byteswap.h` | Endian conversion and byte swapping intrinsics (`htons`, `bswap_32`, …) |
| `sysexits.h`, `paths.h` | Standard BSD exit codes (`EX_OK`, …) and FHS path constants |
| `alloca.h` | Stack memory allocation (`alloca`) |
| `math.h` | Full trig, inverse trig, hyperbolic, log, float variants (`sqrtf`, `sinf`, …) |
| `pthread.h` | Threads, mutexes, condvars, rwlocks, spinlocks, barriers, TLS keys |
| `setjmp.h` | `setjmp`, `longjmp`, `sigsetjmp`, `siglongjmp` (x86_64 asm) |
| `unistd.h` | `read`, `write`, `fork`, `exec`, `pipe`, `dup2`, `sync`, `syncfs`, `nice`, `chroot`, `gethostname`, … |
| `fcntl.h` | `open`, `openat`, `fcntl`, `flock`, `posix_fadvise`, `posix_fallocate`, `AT_*` constants |
| `sys/random.h` | Kernel CSPRNG interface (`getrandom`, `getentropy`) |
| `sys/sendfile.h` | Zero-copy kernel file stream transfer (`sendfile`) |
| `sys/statx.h` | Linux extended file status attribute query (`statx`, `struct statx`) |
| `sys/klog.h` | Linux kernel log buffer control interface (`klogctl`) |
| `sys/epoll.h` | I/O event notification interface (`epoll_create`, `epoll_ctl`, `epoll_wait`) |
| `sys/prctl.h`, `sys/auxv.h` | Process control operations (`prctl`) and ELF auxiliary vectors (`getauxval`) |
| `sys/timex.h` | Clock and timer synchronization tuning (`adjtimex`, `ntp_adjtime`) |
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
- **Scheduler**: Preemptive multi-tasking with per-CPU run queues, ELF64 binary loader, POSIX threads with System V AMD64 stack alignment, and signal delivery.
- **System Calls**: Linux-compatible system call ABI (`syscall` instruction, x86_64 System V AMD64 calling convention) with over 100+ registered handlers including `flock`, `fsync`, `fdatasync`, `sync`, `syncfs`, `getpgid`, `getsid`, `setreuid`, `setregid`, `setresuid`, `getresuid`, `setresgid`, `getresgid`, `getgroups`, `setgroups`, `clock_getres`, `clock_settime`, `clock_nanosleep`, and more.
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
- Drivers: **ext2**, **FAT32**, **ProcFS**, **SysFS**, **DevFS**, **Devpts**, **PipeFS**, **TarFS initrd**.
- POSIX-compliant `opendir`, `readdir`, `closedir`, `fstatat`, `getdents64`.

### GUI Subsystem (Azami Window Manager)

- **Compositor**: Software compositing with double-buffering and alpha blending.
- **Desktop Protocol**: Custom IPC protocol between `azwm` compositor and client applications.
- **Widgets**: Button, text input, list view, canvas, scrollable containers.
- **X11 Compatibility**: Basic X11 client protocol for portable GUI applications.

---

## 📄 License

AzamiOS is an independent open-source project. See [LICENSE](LICENSE) for details.
