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
#include "../include/azami/debug.h"
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
#include "../drivers/gpu/drm/drm_core.h"

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
#include "ipc/sysvipc.h"
#include "ipc/mqueue.h"
#include "ktimer.h"
#include "../fs/vfs.h"
#include "../fs/ext2/ext2.h"
#include "../fs/tmpfs.h"
#include "../arch/x86_64/mm/kprotect.h"
#include "mm/kmodmem.h"
#include "../drivers/block/block.h"
#include "security/security.h"
#include "object/object.h"
#include "../hal/hal.h"
#include "../drivers/base/base.h"
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
/* no_stack_protector: this function's own frame spans security_init()'s
 * reseed of __stack_chk_guard (see the matching comment there) and its
 * final call, sched_start(), is not declared noreturn, so GCC still emits
 * a real epilogue + canary check after it — one that fires with a stale
 * canary if that instruction ever actually executes. Exempting kernel_main
 * costs nothing: every function it calls keeps its own, independently
 * correct, canary. */
static void kernel_init_thread(void *arg)
{
    (void)arg;

    /* ── Step 16: Hardware Abstraction Layer (Device Tree + PCI) ──────────── */
    driver_core_init();
    hal_init();

    /* Driver model: the platform bus declares the fixed PC/AT devices, the
     * PCI bus adopts everything the HAL enumerated, and /dev/kevent starts
     * publishing the hotplug events both of them generate. */
    platform_bus_init();
    pci_bus_init();
    uevent_init();

    /* Every driver below binds through the driver model instead of scanning
     * the bus itself: each call just registers a pci_driver_t (id_table +
     * probe/remove), and the core immediately matches it against the
     * devices pci_bus_init() already enumerated above. A driver whose
     * hardware is not present therefore never runs its bring-up code at
     * all — not even the cheap "check one ID and bail" a few of these used
     * to do — instead of the previous mix of (a) an unconditional call for
     * every driver regardless of whether its device existed, and (b) a
     * couple of drivers (AHCI, NVMe, AC97, BGA) walking the entire device
     * tree by hand to find their own hardware.
     *
     * Registration order still matters for exactly two things this refactor
     * has to preserve:
     *   - virtio_gpu's transport (drivers/video/virtio_gpu.c) is brought up
     *     from *inside* virtgpu_drm's own probe() now, not as a separate
     *     registration — the device model only lets one driver bind a given
     *     PCI function, and virtgpu_drm already owns that match. See the
     *     comment on virtio_gpu_init() for why.
     *   - e1000 registers before rtl8139/ne2k so it still wins "primary
     *     network interface" when more than one NIC is present, matching the
     *     old `if (e1000_init() != 0) rtl8139_init();` intent — each driver
     *     just binds to its own vendor:device ids now, so there is no real
     *     exclusivity between them to preserve, only the tie-break order. */
    extern void virtio_input_init(void);
    extern void virtio_console_init(void);
    extern void i6300esb_init(void);
    extern void virtio_balloon_init(void);
    extern void i2c_core_init(void);
    extern void i801_init(void);
    extern void debugcon_init(void);
    extern void fw_cfg_init(void);
    extern void pvpanic_init(void);
    extern void pci_serial_init(void);
    extern void pcspeaker_init(void);

    debugcon_init();
    fw_cfg_init();
    pvpanic_init();
    pcspeaker_init();
    virtio_input_init();
    virtio_console_init();
    pci_serial_init();
    i6300esb_init();
    virtio_balloon_init();

    /* I2C/SMBus: the core registers the class, then controllers bind to it. */
    i2c_core_init();
    i801_init();
    block_ahci_init();

    /* NVMe controllers (PCI class 0x01 subclass 0x08 prog-if 0x02). */
    extern void block_nvme_init(void);
    block_nvme_init();

    extern int fdc_init(void);
    fdc_init();

    extern void virtio_blk_init(void);
    extern void virtio_scsi_init(void);
    extern void virtio_net_init(void);
    extern void virtio_rng_init(void);
    extern int  virtio_9p_init(void);
    extern void pcnet_init(void);
    extern void hda_init(void);
    extern void es1370_init(void);
    extern int  mpu401_init(void);
    extern int  uhci_init(void);
    extern int  ehci_init(void);
    extern int  pm_timer_init(void);
    extern int  piix4_pm_init(void);
    extern void e1000_init(void);
    extern int  e100_init(void);
    extern void rtl8139_init(void);
    extern void rtl8169_init(void);
    extern int  vmxnet3_init(void);
    extern void net_init(void);
    extern void coretemp_init(void);

    virtio_blk_init();
    virtio_scsi_init();
    virtio_net_init();
    virtio_rng_init();
    virtio_9p_init();
    pcnet_init();
    hda_init();
    es1370_init();
    mpu401_init();
    uhci_init();
    ehci_init();
    pm_timer_init();
    piix4_pm_init();

    extern void bga_init(void);
    bga_init();

    extern void vboxguest_init(void);
    vboxguest_init();
    /* virtgpu_drm's own probe() brings up the virtio-gpu transport (see the
     * long comment above), so this is what actually drives virtio-gpu now —
     * fbdev_init() below reads g_gpu's fields, hence the reordering. */
    drm_subsystem_init();
    extern void fbdev_init(void);
    fbdev_init();
    ac97_init();
    extern void sb16_init(void);
    sb16_init();
    extern void loop_init(void);
    loop_init();

    /* Initialize Network Interface Drivers & Stack. */
    extern void ne2k_pci_init(void);
    e1000_init();
    e100_init();
    rtl8139_init();
    rtl8169_init();
    vmxnet3_init();
    ne2k_pci_init();
    net_init();

    /* Hardware monitoring: CPU digital thermal sensor */
    coretemp_init();

    /* Wait for all async PCI driver probes to finish */
    extern void wait_for_device_probe(void);
    wait_for_device_probe();

    /* ── Mount Root Filesystem (Partitioned Disk or Initrd Fallback) ────── */
    bool root_mounted = false;

    /* 1. Try mounting from disk partition 2 (sata0p2 / sda2) */
    if (block_dev_get("sata0p2") && vfs_mount("sata0p2", "/", "ext2", NULL) == 0) {
        pr_debug("[BOOT] Mounted disk partition 'sata0p2' as root (/)\n");
        root_mounted = true;
    } else if (block_dev_get("sata0") && vfs_mount("sata0", "/", "ext2", NULL) == 0) {
        pr_debug("[BOOT] Mounted unpartitioned 'sata0' as root (/)\n");
        root_mounted = true;
    } else if (block_dev_get("hda2") && vfs_mount("hda2", "/", "ext2", NULL) == 0) {
        pr_debug("[BOOT] Mounted disk partition 'hda2' as root (/)\n");
        root_mounted = true;
    }

    /* 2. Fallback to initrd ramdisk (ram0) if disk root was not mounted */
    if (!root_mounted) {
        struct limine_file *initrd = az_boot_initrd();
        if (initrd && initrd->address) {
            pr_debug("[INITRD] Found initrd module at 0x%016llx (%llu bytes)\n",
                    (unsigned long long)initrd->address, (unsigned long long)initrd->size);
            phys_addr_t initrd_phys = VIRT_TO_PHYS((virt_addr_t)initrd->address);
            block_dev_t *ram0 = block_ramdisk_init(initrd_phys, initrd->size);
            if (ram0 && vfs_mount("ram0", "/", "ext2", NULL) == 0) {
                pr_debug("[INITRD] Mounted ext2 initrd as root (/)\n");
                root_mounted = true;
            } else {
                pr_debug("[INITRD] Failed to mount ext2 initrd\n");
            }
        } else {
            pr_debug("[INITRD] No initrd module passed by Limine\n");
        }
    }

    if (root_mounted) {
        vfs_mkdir("/dev", 0755);
        if (vfs_mount("devfs", "/dev", "devfs", NULL) == 0) {
            pr_debug("[DEVFS] Mounted devfs at /dev\n");
        }
        vfs_mkdir("/proc", 0755);
        if (vfs_mount("procfs", "/proc", "procfs", NULL) == 0) {
            pr_debug("[PROCFS] Mounted procfs at /proc\n");
        }
        vfs_mkdir("/sys", 0755);
        if (vfs_mount("sysfs", "/sys", "sysfs", NULL) == 0) {
            pr_debug("[SYSFS] Mounted sysfs at /sys\n");
        }
        vfs_mkdir("/dev/pts", 0755);
        if (vfs_mount("devpts", "/dev/pts", "devpts", NULL) == 0) {
            pr_debug("[DEVPTS] Mounted devpts at /dev/pts\n");
        }

        /* Mount boot partition at /boot if present */
        vfs_mkdir("/boot", 0755);
        if (block_dev_get("sata0p1") && vfs_mount("sata0p1", "/boot", "ext2", NULL) == 0) {
            pr_debug("[BOOT] Mounted boot partition (sata0p1) at /boot\n");
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
        PANIC("VFS: Unable to mount root filesystem on any block device!");
    }

    /* Mount tmpfs at /tmp so processes get a writable RAM scratch area */
    vfs_mkdir("/tmp", 01777);
    if (vfs_mount("tmpfs", "/tmp", "tmpfs", NULL) == 0) {
        pr_debug("[TMPFS] Mounted tmpfs at /tmp\n");
    } else {
        pr_debug("[TMPFS] Warning: failed to mount tmpfs at /tmp\n");
    }

    /* Mount persistent /hdd if available */
    vfs_mkdir("/hdd", 0755);
    if (block_dev_get("sata0p3") && vfs_mount("sata0p3", "/hdd", "ext2", NULL) == 0) {
        pr_debug("[STORAGE] Mounted persistent SATA partition to /hdd\n");
    } else if (block_dev_get("sata1") && vfs_mount("sata1", "/hdd", "ext2", NULL) == 0) {
        pr_debug("[STORAGE] Mounted secondary SATA drive to /hdd\n");
    }

    /*
     * Everything that registers a handler, a driver or a table has now done
     * so, and nothing has entered ring 3 yet. This is the only moment where
     * "written during boot" and "never written again" are the same statement,
     * so it is where the kernel's own image stops being writable: .text loses
     * WRITE, .rodata and every __ro_after_init table lose it too, .data and
     * .bss lose execute, and the HHDM — which aliases all of physical memory,
     * kernel image included — loses execute across the whole window. See
     * arch/x86_64/mm/kprotect.c.
     *
     * Deliberately after the filesystem mounts and before sched_spawn_user():
     * a mount can still allocate and populate driver state, and PID 1 is the
     * first thing in the system that is not trusted.
     */
    /* Build and seal a tiny generated function *before* the seal, so all of
     * the test's page-table work (and the TLB shootdowns that go with it) is
     * done and settled by the time kprotect_seal() issues its own. */
    kmod_selftest_prepare();

    kprotect_seal();

    /* Now prove the one thing kprotect_seal() could plausibly have broken:
     * that code generated at runtime still runs, from its own mapping, with
     * the direct map non-executable everywhere. */
    kmod_selftest_verify();

    pr_debug("\n[BOOT] All core microkernel subsystems initialized successfully.\n");

    /* Launch ring-3 userspace init process */
    pr_debug("[BOOT] Launching Userspace Init process (/sbin/init.elf)...\n");
    process_t *init_proc = sched_spawn_user("/sbin/init.elf");
    if (!init_proc) {
        init_proc = sched_spawn_user("/init.elf");
    }
    /* No init: fall back to a bare shell so the machine is still usable.
     * /bin/sh is GNU bash on this image and /bin/azami-sh.elf is the small
     * native shell that ships alongside it; try both rather than one, since
     * this path exists precisely for an image that is already incomplete. */
    if (!init_proc) {
        init_proc = sched_spawn_user("/bin/sh");
    }
    if (!init_proc) {
        init_proc = sched_spawn_user("/bin/azami-sh.elf");
    }
    if (!init_proc) {
        init_proc = sched_spawn_user("/sh.elf");
    }
    if (init_proc) {
        pr_debug("[BOOT] Initial user process spawned successfully (PID %u).\n", init_proc->pid);
    } else {
        pr_debug("[BOOT] Warning: Could not launch /sbin/init.elf or a shell\n");
    }

}

void kernel_main(void) __attribute__((no_stack_protector));
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

    /* HPET: nanosecond monotonic time source (optional; needs VMM up) */
    extern void hpet_init(void);
    hpet_init();

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

    /* ── Kernel CSPRNG (ChaCha20) — backs getrandom(2), /dev/[u]random,
     *    stack canaries and AT_RANDOM ──────────────────────────────────── */
    extern void krandom_init(void);
    krandom_init();
    pr_debug("[BOOT] CSPRNG seeded\n");

    /* ── Step 10: Security & Stack Canaries ──────────────────────────────── */
    security_init();

    /* ── Step 11: SMP & Local APIC ───────────────────────────────────────── */
    smp_init();

    /* Every core now has a valid GS base, so the allocators can move their
     * fast paths onto per-CPU structures and stop serialising on shared locks:
     * the PMM caches order-0 frames per (CPU, zone), and kmalloc()/kfree()
     * cache bucket objects per (CPU, size class). Order matters only in that
     * both must come after smp_init(); before it the BSP is single-threaded
     * and both allocate straight from their shared pools. */
    pmm_enable_percpu();
    kmalloc_enable_percpu();

    /* ── Timekeeping & vDSO ──────────────────────────────────────────────
     * After smp_init(): the TSC frequency is calibrated against the HPET
     * there, and every AP has programmed IA32_TSC_AUX for the vDSO's
     * getcpu(). Before anything reads the wall clock (the VFS stamps
     * inode times from it below). */
    extern void timekeeping_init(void);
    extern void vdso_init(void);
    timekeeping_init();
    vdso_init();

    /* ── Performance counters ────────────────────────────────────────────
     * After smp_init(), because programming a counter is per-logical-processor
     * and pmu_sync_local() needs this core's id from the per-CPU block. */
    extern void pmu_init(void);
    extern void perf_init(void);
    extern void ktrace_init(void);
    pmu_init();
    perf_init();
    ktrace_init();

    /* ── Step 12: CFS Scheduler & Process/Thread Manager ─────────────────── */
    sched_init();

    /* Heap reaper: hands fully-free slab pages back to the PMM on a timer.
     * Needs the scheduler — it runs as a kernel thread. */
    kmalloc_start_reaper();

    /* ── Step 13: Inter-Process Communication (IPC) ──────────────────────── */
    ipc_init();

    /* System V IPC (XSI shared memory, semaphores, message queues) and the
     * POSIX per-process timer engine.  The timer engine needs the scheduler,
     * which is up by now, because it runs as a kernel thread. */
    sysvipc_init();
    /* POSIX message queues are independent of the System V table above; they
     * only need the fd layer, which the scheduler has already brought up. */
    mqueue_init();
    extern void posix_sem_init(void);
    posix_sem_init();
    ktimer_init();

    /* ── Step 13b: Input Subsystem (PS/2 Keyboard + Mouse) ────────────────── */
    input_init();

    /* Linux input UAPI on top of it (/dev/input/event0). */
    extern void evdev_init(void);
    evdev_init();

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

    extern void procfs_init(void);
    procfs_init();

    extern void sysfs_init(void);
    sysfs_init();

    extern void pty_init(void);
    pty_init();

    extern void devpts_init(void);
    devpts_init();

    /* ── tmpfs: RAM-backed volatile filesystem ("/tmp") ──────────────────── */
    tmpfs_init();

    ext2_init();

    /* ── squashfs: read-only compressed filesystem ────────────────────────── */
    extern void squashfs_init(void);
    squashfs_init();

    /* ── Step 15: NT-Style Object Manager Namespace ──────────────────────── */
    az_object_manager_init();

    extern void acl_init(void);
    acl_init();

    thread_create(sched_kernel_process(), (uintptr_t)kernel_init_thread, 0, true);

    pr_debug("[BOOT] Starting preemptive CFS scheduling loop across all cores...\n\n");

    /* Release the W^X self-test page. Deferred to here on purpose — see
     * kmod_selftest_verify(): unmapping it costs a global TLB shootdown, and
     * doing that immediately behind kprotect_seal()'s own measurably cost
     * half a second of boot. */
    kmod_selftest_release();

    /*
     * Spread device interrupts across the cores that came up.
     *
     * Every driver probed above ran on the BSP, and hal_irq_enable() routes
     * an interrupt to a CPU chosen at the moment it is called — which, while
     * the APs were still parked, could only be the BSP. Now that they are
     * online, redistribute: a busy NIC or AHCI controller otherwise delivers
     * every completion to the same core that runs the timekeeping tick.
     */
    extern void hal_irq_rebalance(void);
    hal_irq_rebalance();

    /* Enable Local APIC periodic timer for scheduler preemption (vec 48) */
    lapic_timer_start(100); /* 100 Hz = 10 ms tick */

    /* Signal all APs to start their local APIC timer and enter the CFS scheduler */
    extern volatile bool g_smp_sched_active;
    __atomic_store_n(&g_smp_sched_active, true, __ATOMIC_SEQ_CST);

    /* Start scheduling on the bootstrap processor (never returns) */
    sched_start();
}
