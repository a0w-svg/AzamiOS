# AzamiOS Internal Technical Documentation

This document provides detailed deep-dives into the kernel internal architecture, memory subsystems, dual 32-bit / 64-bit adaptation patterns, and hardware driver implementations.

---

## 1. 64-Bit Architecture & System Design

AzamiOS is designed specifically for 64-bit (`x86_64`) Long Mode, ensuring a clean and minimalistic architecture without legacy 32-bit code paths or dual-build complexity.

### A. Data Types & Memory Pointers
All pointers, page frame counts, and virtual memory offsets across the kernel use `uintptr_t` or `size_t` from `<stdint.h>`.
- In 64-bit compilation (`x86_64-elf-gcc -m64`), `uintptr_t` resolves to an 8-byte unsigned integer (`uint64_t`).

Example from process management (`process.h`):
```c
typedef struct process {
    uint32_t pid;
    char name[32];
    uint32_t state;
    uintptr_t cr3;          // Physical Page Directory / PML4 base address
    uintptr_t kernel_esp;   // Saved kernel stack pointer (points to registers_t)
    uintptr_t kernel_stack; // Allocated ring-0 stack base address
    uintptr_t user_stack;   // User ring-3 stack pointer
    uintptr_t entry;        // Process entry point instruction address
    struct process *next;
} process_t;
```

### B. Hardware Initialization & Boot
When booting, the system operates in 64-bit Long Mode with paging active (4-level PML4 table) and linear framebuffer support. All GDT, IDT, and TSS structures are native 64-bit descriptors (`tss_t` 104 bytes, 16-byte IDT gates).

---

## 2. Physical & Virtual Memory Management

### Physical Memory Manager (PMM)
The PMM (`kernel/mem/pmm.c`) tracks physical frame allocations using a bitmap allocator. Each bit represents a 4KB physical memory page frame (`PMM_FRAME_SIZE = 4096`).
- **64-Bit Mode**: Initializes physical frame allocations (`PMM_FRAME_SIZE = 4096`).

### Virtual Memory Manager & Paging
- **Identity Mapping**: The kernel protects runtime code execution by identity-mapping low physical memory (`0x00000000` through `0x08000000`).
- **Framebuffer Mapping**: The `paging_map_framebuffer` routine reserves linear address space for MMIO framebuffers discovered either through VBE Multiboot tags or UEFI GOP structure disclosures.

---

## 3. Filesystem Hierarchy & Device nodes (`/dev`)

AzamiOS integrates a Unix Filesystem Hierarchy Standard (FHS) tree within its Virtual Filesystem (`lib/fs/vfs.c`).

### Automatic Device Node Generation
During kernel boot, storage drivers register themselves with the VFS subsystem via `vfs_register_device(name, read_fn, write_fn)`. The VFS automatically constructs a virtual inode under the `/dev/` directory:
1. **ATA Driver**: Probes primary/secondary IDE buses. Registered as `/dev/hda`.
2. **SATA AHCI Driver**: Enters AHCI ABAR memory space, detects attached SATA drives. Registered as `/dev/sda`.
3. **VirtIO Block Driver**: Scans PCI vendor IDs (`0x1AF4`), negotiates paravirtualized ring buffers. Registered as `/dev/vda`.

Userspace commands and system utilities interact with hardware storage transparently by opening `/dev/hda` or `/dev/vda` using standard POSIX-style file descriptors.

---

## 4. Driver Framework & VirtIO Paravirtualization

### PCI Bus Enumeration
The PCI driver (`kernel/drivers/bus/pci.c`) scans all 256 buses, 32 devices, and 8 functions to identify hardware controllers. When discovered, class codes and vendor IDs are matched against registered drivers.

### VirtIO Architecture
To achieve near-native disk and network speeds inside hypervisors like QEMU, AzamiOS implements the **VirtIO 1.0/Legacy specification**:
- **Virtqueues**: Uses circular descriptor tables, available rings (`avail`), and used rings (`used`) allocated in contiguous physical RAM.
- **VirtIO-Net**: Transmits and receives raw Ethernet frames directly through shared hypervisor buffers without trap-and-emulate MMIO overhead.
- **VirtIO-Blk**: Submits asynchronous block read/write requests (`VIRTIO_BLK_T_IN` / `VIRTIO_BLK_T_OUT`) with status completion callbacks.

### MMIO Paging & Hardware Isolation
High-bandwidth hardware drivers such as Intel PRO/1000 Gigabit NIC (`e1000`) and SATA AHCI controllers communicate via Memory-Mapped I/O (MMIO) registers assigned high physical addresses (e.g., `0xfeb80000`). AzamiOS kernel paging maps these exact device page frames on demand (`paging_map_page`) prior to accessing register offsets (`RAL`/`RAH` or `ABAR`), preventing page faults while preserving memory access control across other address ranges.

---

## 5. Window Manager & Userspace Services

The AzamiOS Window Manager (`wm`) runs as a secure userspace process over the microkernel syscall interface.
- **Dynamic VFS Integration**: The built-in File Manager (`files.c`) dynamically accesses the root filesystem (`opendir("/")` and `readdir()`) to display live FHS directories (`/bin`, `/sbin`, `/usr`, `/etc`, `/dev`) alongside mounted FAT32 and initrd volumes.
- **Modular Service Registration**: Applications and desktop utilities register via standardized service definitions (`wm_service_t`), ensuring strict visual isolation and robust event handling without direct kernel access.
