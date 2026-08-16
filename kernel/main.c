/* ============================================================================
 * AzamiOS — Kernel Main Entry Point
 * File: kernel/main.c
 *
 * Initialisation sequence:
 *   1.  console_init_early()   — UART COM1 (usable immediately from ring 0)
 *   2.  pic_init()             — remap 8259 PICs away from exceptions
 *   3.  gdt_init_bsp()         — per-CPU GDT + TSS for the boot CPU
 *   4.  idt_init()             — full 256-entry IDT
 *   5.  pmm_init()             — buddy allocator from Limine memory map
 *   6.  vmm_init()             — 4-level PML4 + HHDM + SMEP/SMAP/NX
 *   7.  console_init_fb()      — framebuffer console (after VMM)
 *   8.  syscall_abi_init()     — write STAR/LSTAR/SFMASK MSRs
 *   9.  smp_init()             — wake APs, set up per-CPU GS base
 *   10. sched_init()           — CFS scheduler + idle threads
 *   11. vfs_init()             — virtual filesystem (ramfs + fat32)
 *   12. az_object_manager_init() — Azami NT-style object manager
 *   13. sti + sched_start()    — enable interrupts, enter idle loop
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "../include/azami/types.h"
#include "../include/azami/defs.h"
#include "panic.h"
#include "../drivers/char/console.h"
#include "../drivers/input/input.h"
#include "../drivers/char/uart.h"
#include "../drivers/misc/rtc.h"
#include "../drivers/char/lpt.h"
#include "../drivers/acpi/acpi.h"
#include "../drivers/acpi/ioapic.h"
#include "../drivers/sound/ac97.h"

/* Architecture layer */
#include "../arch/x86_64/boot/limine_req.h"
#include "../arch/x86_64/cpu/gdt.h"
#include "../arch/x86_64/cpu/idt.h"
#include "../arch/x86_64/cpu/pic.h"
#include "../arch/x86_64/cpu/msr.h"
#include "../arch/x86_64/mm/vmm.h"

/* Kernel layer */
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "syscall/syscall.h"
#include "sched/sched.h"
#include "sched/elf.h"
#include "lib/string.h"
#include "ipc/ipc.h"
#include "../fs/vfs.h"
#include "../fs/ext2/ext2.h"
#include "../drivers/block/block.h"
#include "security/security.h"
#include "object/object.h"
#include "../hal/hal.h"
#include "../arch/x86_64/cpu/smp.h"
#include "../arch/x86_64/cpu/lapic.h"


/* Syscall ABI init is in assembly; we declare the C wrapper here. */
extern void syscall_abi_init(void);

/* ── Boot banner ──────────────────────────────────────────────────────────── */
static void print_banner(void)
{
    kprintf("\n");
    kprintf("  ██████╗ ███████╗ █████╗ ███╗   ███╗██╗ ██████╗ ███████╗\n");
    kprintf("  ██╔══██╗╚════██║██╔══██╗████╗ ████║██║██╔═══██╗██╔════╝\n");
    kprintf("  ███████║    ██╔╝███████║██╔████╔██║██║██║   ██║███████╗\n");
    kprintf("  ██╔══██║   ██╔╝ ██╔══██║██║╚██╔╝██║██║██║   ██║╚════██║\n");
    kprintf("  ██║  ██║   ██║  ██║  ██║██║ ╚═╝ ██║██║╚██████╔╝███████║\n");
    kprintf("  ╚═╝  ╚═╝   ╚═╝  ╚═╝  ╚═╝╚═╝     ╚═╝╚═╝ ╚═════╝ ╚══════╝\n");
    kprintf("                          AzamiOS v7.0 — x86_64 Microkernel\n\n");
}

/* ── Limine base revision check ───────────────────────────────────────────── */
/* The Limine base revision struct is declared in limine_req.c.
 * We check it here to verify the bootloader filled in our requests. */
extern volatile struct limine_base_revision g_limine_base_rev;

/* ============================================================================
 * kernel_main() — called by az_boot_entry() in entry.asm
 * ============================================================================ */
void kernel_main(void)
{
    /* ── Step 1: Early console (UART COM1) ──────────────────────────────── */
    console_init_early();
    print_banner();
    pr_debug("[BOOT] AzamiOS kernel starting...\n");

    /* ── Verify Limine base revision ─────────────────────────────────────── */
    if (LIMINE_BASE_REVISION_SUPPORTED) {
        pr_debug("[BOOT] Limine base revision: supported\n");
    } else {
        pr_debug("[BOOT] WARNING: Limine base revision mismatch — some features may be unavailable\n");
    }

    /* ── Step 2: PIC remap ───────────────────────────────────────────────── */
    pic_init(0x20, 0x28);   /* Remap: IRQ0→vec32 … IRQ15→vec47 */
    pic_mask_all();          /* Mask all IRQs; we use LAPIC after init */
    pr_debug("[BOOT] PIC remapped and masked\n");

    /* ── Step 3: GDT / TSS (BSP) ────────────────────────────────────────── */
    gdt_init_bsp();
    pr_debug("[BOOT] GDT + TSS initialised for BSP\n");

    /* ── Step 4: IDT ─────────────────────────────────────────────────────── */
    idt_init();
    pr_debug("[BOOT] IDT loaded (256 entries)\n");

    /* ── Step 5: PMM (buddy allocator) ──────────────────────────────────── */
    struct limine_memmap_response *memmap = az_boot_memmap();
    if (!memmap) PANIC("Limine did not provide a memory map!");
    pmm_init(memmap);
    pr_debug("[BOOT] PMM: buddy allocator ready\n");

    /* ── Step 6: VMM (4-level PML4 + HHDM + SMEP/SMAP + NXE) ──────────── */
    u64 hhdm_base  = az_boot_hhdm_base();
    if (!hhdm_base) PANIC("Limine did not provide HHDM base!");

    /* Get kernel load addresses from Limine */
    u64 kern_phys = 0, kern_virt = 0;
    if (g_limine_kaddr_req.response) {
        kern_phys = g_limine_kaddr_req.response->physical_base;
        kern_virt = g_limine_kaddr_req.response->virtual_base;
    }

    vmm_init(hhdm_base, kern_phys, kern_virt, memmap);
    pr_debug("[BOOT] VMM: 4-level paging active, HHDM=0x%016llx\n",
            (unsigned long long)hhdm_base);

    /* ── Step 6.5: ACPI Initialization ───────────────────────────────────── */
    acpi_init();
    ioapic_init();

    /* ── Step 7: Framebuffer console ─────────────────────────────────────── */
    struct limine_framebuffer *fb = az_boot_framebuffer();
    if (fb) {
        console_init_fb((void *)(uintptr_t)fb->address,
                        (u32)fb->width, (u32)fb->height,
                        (u32)fb->pitch, (u8)fb->bpp);
        pr_debug("[BOOT] Framebuffer console: %ux%u %ubpp\n",
                (u32)fb->width, (u32)fb->height, (u8)fb->bpp);
    } else {
        pr_debug("[BOOT] No framebuffer available — UART only\n");
    }

    /* ── Step 8: SYSCALL / SYSRET ABI ────────────────────────────────────── */
    syscall_abi_init();
    syscall_init();
    pr_debug("[BOOT] SYSCALL/SYSRET ABI configured (STAR/LSTAR MSRs written)\n");

    /* ── Step 9: Kernel Dynamic Heap Allocator (kmalloc) ─────────────────── */
    kmalloc_init();

    /* ── Step 10: Security & Stack Canaries ──────────────────────────────── */
    security_init();

    /* ── Step 11: SMP & Local APIC ───────────────────────────────────────── */
    smp_init();

    /* ── Step 12: CFS Scheduler & Process/Thread Manager ─────────────────── */
    sched_init();

    /* ── Step 13: Inter-Process Communication (IPC) ──────────────────────── */
    ipc_init();

    /* ── Step 13b: Input Subsystem (PS/2 Keyboard + Mouse) ────────────────── */
    input_init();

    /* ── Step 14: Virtual File System (VFS) & Block Device Layer ─────────── */
    vfs_init();
    block_dev_init();

    /* Register character devices */
    uart_register_devfs();
    rtc_register_devfs();
    lpt_register_devfs();

    extern void devfs_init(void);
    devfs_init();

    extern void fat32_init(void);
    fat32_init();

    extern void memdevs_init(void);
    memdevs_init();

    extern void pcspeaker_init(void);
    pcspeaker_init();

    extern void procfs_init(void);
    procfs_init();

    ext2_init();

    /* ── Step 15: NT-Style Object Manager Namespace ──────────────────────── */
    az_object_manager_init();

    /* ── Step 16: Hardware Abstraction Layer (Device Tree + PCI) ──────────── */
    hal_init();
    extern int ata_init(void);
    ata_init();
    block_ahci_init();

    /* Probe VirtIO and legacy PCI devices */
    extern int virtio_blk_init(device_t *dev);
    extern int virtio_net_init(device_t *dev);
    extern int virtio_gpu_init(device_t *dev);
    extern int e1000_init(void);
    extern int rtl8139_init(void);
    extern void net_init(void);

    device_t *pci_bus = device_find("PCI0");
    if (pci_bus) {
        device_t *child = pci_bus->children;
        while (child) {
            virtio_blk_init(child);
            virtio_net_init(child);
            virtio_gpu_init(child);
            child = child->sibling;
        }
    }

    extern void bga_init(void);
    bga_init();
    ac97_init();

    /* Initialize Network Interface Drivers & Stack */
    if (e1000_init() == 0 || rtl8139_init() == 0) {
        net_init();
    } else {
        net_init();
    }

    /* Load initrd.ext2 module as ramdisk and mount */
    struct limine_file *initrd = az_boot_initrd();
    if (initrd && initrd->address) {
        pr_debug("[INITRD] Found initrd module at 0x%016llx (%llu bytes)\n",
                (unsigned long long)initrd->address, (unsigned long long)initrd->size);
                
        phys_addr_t initrd_phys = VIRT_TO_PHYS((virt_addr_t)initrd->address);
        block_dev_t *ram0 = block_ramdisk_init(initrd_phys, initrd->size);
        if (ram0) {
            if (vfs_mount("ram0", "/", "ext2", NULL) == 0) {
                pr_debug("[INITRD] Mounted ext2 initrd as root (/).\n");
                vfs_mkdir("/dev", 0755);
                if (vfs_mount("devfs", "/dev", "devfs", NULL) == 0) {
                    pr_debug("[DEVFS] Mounted devfs at /dev\n");
                }
                vfs_mkdir("/proc", 0755);
                if (vfs_mount("procfs", "/proc", "procfs", NULL) == 0) {
                    pr_debug("[PROCFS] Mounted procfs at /proc\n");
                }
                file_t *f = vfs_open("/sbin/init.elf", 0, 0);
                if (!f) f = vfs_open("/init.elf", 0, 0);
                if (f) {
                    u8 buf[4] = {0};
                    vfs_read(f, buf, 4);
                    pr_debug("[TEST] init.elf ELF magic: 0x%02x %c%c%c\n",
                             buf[0], buf[1], buf[2], buf[3]);
                    vfs_close(f);
                } else {
                    pr_debug("[TEST] Failed to open /sbin/init.elf\n");
                }
            } else {
                pr_debug("[INITRD] Failed to mount ext2 initrd.\n");
            }
        }
    } else {
        pr_debug("[INITRD] No initrd module passed by Limine.\n");
    }

    if (vfs_mount("sata0", "/hdd", "ext2", NULL) == 0) {
        pr_debug("[STORAGE] Mounted persistent SATA drive to /hdd\n");
    } else {
        pr_debug("[STORAGE] Warning: Failed to mount sata0 to /hdd\n");
    }

    pr_debug("\n[BOOT] All core microkernel subsystems initialized successfully.\n");

    /* Launch ring-3 userspace init process */
    pr_debug("[BOOT] Launching Userspace Init process (/sbin/init.elf)...\n");
    process_t *init_proc = sched_spawn_user("/sbin/init.elf");
    if (!init_proc) {
        init_proc = sched_spawn_user("/init.elf");
    }
    if (!init_proc) {
        init_proc = sched_spawn_user("/bin/sh.elf");
    }
    if (!init_proc) {
        init_proc = sched_spawn_user("/sh.elf");
    }
    if (init_proc) {
        pr_debug("[BOOT] Initial user process spawned successfully (PID %u).\n", init_proc->pid);
    } else {
        pr_debug("[BOOT] Warning: Could not launch /sbin/init.elf or /bin/sh.elf\n");
    }

    pr_debug("[BOOT] Starting preemptive CFS scheduling loop across all cores...\n\n");

    /* Enable Local APIC periodic timer for scheduler preemption (vec 48) */
    lapic_timer_start(100); /* 100 Hz = 10 ms tick */

    /* Start scheduling on the bootstrap processor (never returns) */
    sched_start();
}
