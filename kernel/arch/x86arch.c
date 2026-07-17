#include "./include/x86arch.h"
#include "../boot/boot_info.h"
#include "./include/gdt.h"
#include "./include/isr.h"
#include "../drivers/include/terminal.h"
#include "../klibc/include/stdio.h"
#include "../drivers/include/pit.h"
#include "../drivers/include/keyboard.h"
#include "../drivers/include/kbc.h"
#include "../mem/include/pmm.h"
#include "../mem/include/paging.h"
#include "../mem/include/security.h"
#include "../../thirdparty/multiboot.h"
#include "../drivers/include/rtc.h"
#include "../drivers/include/mouse.h"
#include "../drivers/include/serial.h"
#include "../klibc/include/string.h"
#include "../syscall/include/syscall.h"
#include "../syscall/include/exec.h"
#include "../filesystem/include/tarfs.h"
#include "../filesystem/include/vfs.h"
#include "../drivers/include/ata.h"
#include "../filesystem/include/fat32.h"
#include "../proc/include/process.h"
#include "../proc/include/scheduler.h"
#include "../proc/include/lpc.h"
#include "../proc/include/exec_server.h"
#include "./include/smp.h"
#include "./include/cpu_check.h"
#include "../drivers/include/ac97.h"
#include "../drivers/include/acpi.h"
#include "../drivers/include/rtl8139.h"
#include "../drivers/include/e1000.h"
#include "../drivers/include/sb16.h"
#include "../drivers/include/gameport.h"
#include "../drivers/include/pcnet.h"
#include "../drivers/include/ne2k.h"
#include "../drivers/include/ahci.h"
#include "../drivers/include/nvme.h"
#include "../drivers/include/es1370.h"
#include "../drivers/include/uhci.h"
#include "../drivers/include/xhci.h"
#include "../drivers/include/dma.h"
#include "../drivers/include/lpt.h"
#include "../drivers/include/floppy.h"
#include "../drivers/include/virtio.h"
#include "../drivers/include/gfx.h"
#include "../module/include/module.h"
#include "../drivers/include/net_stack.h"
#include "../drivers/include/pci.h"
#include "../klibc/include/port.h"
#include "../include/uefi.h"

static uint32_t g_mem_size_kb = 0;
static uintptr_t g_bitmap_addr = 0;
static uintptr_t g_free_mem_start = 0;
static uintptr_t g_initrd_loc = 0;
static block_device_t *g_ata_dev = (block_device_t*)0;
bool g_is_uefi = false;

static int mod_cpu_init(void) { cpu_check_init(); return 0; }
static int mod_rtc_init(void) { rtc_init(); init_mouse(); init_keyboard(); return 0; }
static int mod_pmm_init(void) { 
    pmm_init(g_mem_size_kb, g_bitmap_addr); 
    uint32_t pool_size = 64 * 1024 * 1024;
    if (g_free_mem_start + pool_size > g_mem_size_kb * 1024) {
        pool_size = (g_mem_size_kb * 1024) - g_free_mem_start;
    }
    pmm_deinit_region(g_free_mem_start, pool_size); 
    return 0; 
}
static int mod_paging_init(void) { if (!g_is_uefi) paging_init(); return 0; }
static int mod_tarfs_init(void) { if (g_initrd_loc) tarfs_init(g_initrd_loc); return 0; }
static int mod_ata_init(void) { int r = ata_init(); if (r == 0) g_ata_dev = ata_get_device(); return r; }
static int mod_fat32_init(void) { if (g_ata_dev) fat32_init(g_ata_dev); return 0; }
static int mod_acpi_init(void) { acpi_init(); return 0; }
static int mod_ac97_init(void) { ac97_init(); return 0; }
static int mod_rtl8139_init(void) { rtl8139_init(); return 0; }
static int mod_e1000_init(void) { e1000_init(); return 0; }
static int mod_sb16_init(void) { sb16_init(); return 0; }
static int mod_gameport_init(void) { gameport_init(); return 0; }
static int mod_pcnet_init(void) { pcnet_init(); return 0; }
static int mod_ne2k_init(void) { ne2k_init(); return 0; }
static int mod_ahci_init(void) { ahci_init(); return 0; }
static int mod_nvme_init(void) { nvme_init(); return 0; }
static int mod_es1370_init(void) { es1370_init(); return 0; }
static int mod_uhci_init(void) { uhci_init(); return 0; }
static int mod_xhci_init(void) { xhci_init(); return 0; }
static int mod_netstack_init(void) { net_stack_init(); return 0; }
static int mod_sched_init(void) { process_init(); scheduler_init(); smp_init(); return 0; }
static int mod_alpc_init(void) { lpc_init(); exec_server_init(); return 0; }
static int mod_floppy_init(void) { floppy_init(); return 0; }
static int mod_virtio_init(void) { virtio_init(); return 0; }

extern int az_kernel_main(void);
static azami_boot_info_t* g_cached_boot_info = (azami_boot_info_t*)0;

static int mod_ntos_init(void) {
    az_kernel_main();
    return 0;
}


static bool pci_match(uint16_t vendor, uint16_t device) {
    uint8_t b, s, f;
    return pci_find_device(vendor, device, &b, &s, &f);
}

static bool mod_ahci_probe(void) {
    for (uint16_t bus = 0; bus < 8; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            if (pci_config_read16(bus, slot, 0, 0x00) == 0xFFFF) continue;
            uint32_t class_rev = pci_config_read32(bus, slot, 0, 0x08);
            if ((class_rev >> 24) == 0x01 && ((class_rev >> 16) & 0xFF) == 0x06) return true;
        }
    }
    return false;
}
static bool mod_ac97_probe(void) { return pci_match(0x8086, 0x2415); }
static bool mod_es1370_probe(void) { return pci_match(0x1274, 0x5000); }
static bool mod_rtl8139_probe(void) { return pci_match(0x10EC, 0x8139); }
static bool mod_e1000_probe(void) { return pci_match(0x8086, 0x100E); }
static bool mod_pcnet_probe(void) { return pci_match(0x1022, 0x2000); }
static bool mod_ne2k_probe(void) { return pci_match(0x10EC, 0x8029); }
static bool mod_uhci_probe(void) {
    for (uint16_t bus = 0; bus < 8; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            if (pci_config_read16(bus, slot, 0, 0x00) == 0xFFFF) continue;
            for (uint8_t func = 0; func < 8; func++) {
                if (pci_config_read16(bus, slot, func, 0x00) == 0xFFFF) continue;
                uint32_t class_rev = pci_config_read32(bus, slot, func, 0x08);
                if ((class_rev >> 24) == 0x0C && ((class_rev >> 16) & 0xFF) == 0x03 && ((class_rev >> 8) & 0xFF) == 0x00) return true;
            }
        }
    }
    return false;
}
static bool mod_nvme_probe(void) {
    for (uint16_t bus = 0; bus < 8; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            if (pci_config_read16(bus, slot, 0, 0x00) == 0xFFFF) continue;
            for (uint8_t func = 0; func < 8; func++) {
                if (pci_config_read16(bus, slot, func, 0x00) == 0xFFFF) continue;
                uint32_t class_rev = pci_config_read32(bus, slot, func, 0x08);
                if ((class_rev >> 24) == 0x01 && ((class_rev >> 16) & 0xFF) == 0x08 && ((class_rev >> 8) & 0xFF) == 0x02) return true;
            }
        }
    }
    return false;
}
static bool mod_xhci_probe(void) {
    for (uint16_t bus = 0; bus < 8; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            if (pci_config_read16(bus, slot, 0, 0x00) == 0xFFFF) continue;
            for (uint8_t func = 0; func < 8; func++) {
                if (pci_config_read16(bus, slot, func, 0x00) == 0xFFFF) continue;
                uint32_t class_rev = pci_config_read32(bus, slot, func, 0x08);
                if ((class_rev >> 24) == 0x0C && ((class_rev >> 16) & 0xFF) == 0x03 && ((class_rev >> 8) & 0xFF) == 0x30) return true;
            }
        }
    }
    return false;
}
static bool mod_sb16_probe(void) {
    outb(0x226, 1);
    for (volatile int i = 0; i < 1000; i++);
    outb(0x226, 0);
    for (volatile int i = 0; i < 1000; i++);
    int timeout = 1000;
    while (timeout-- > 0) {
        if (inb(0x22E) & 0x80) {
            if (inb(0x22A) == 0xAA) return true;
        }
    }
    return false;
}
static bool mod_gameport_probe(void) { return inb(0x201) != 0xFF; }

static kernel_module_t kmods[] = {
    { "cpu",       "x86 CPU Topology & Feature Checker", MOD_CORE, 0,       mod_cpu_init,      0, 0, 0 },
    { "rtc",       "RTC & Peripherals",         MOD_CORE, 0,               mod_rtc_init,      0, 0, 0 },
    { "pmm",       "Physical Memory Manager",   MOD_MEM,  0,               mod_pmm_init,      0, 0, 0 },
    { "paging",    "Virtual Memory Manager",    MOD_MEM,  0,               mod_paging_init,   0, 0, 0 },
    { "tarfs",     "TarFS RAMDisk Initrd",      MOD_FS,   0,               mod_tarfs_init,    0, 0, 0 },
    { "ata",       "ATA IDE Storage Driver",    MOD_DRV,  0,               mod_ata_init,      0, 0, 0 },
    { "floppy",    "Floppy Disk Controller",    MOD_DRV,  0,               mod_floppy_init,   0, 0, 0 },
    { "virtio",    "VirtIO Paravirtual Bus",    MOD_DRV,  virtio_probe,    mod_virtio_init,   0, 0, 0 },
    { "fat32",     "FAT32 Storage Driver",      MOD_FS,   0,               mod_fat32_init,    0, 0, 0 },
    { "ahci",      "SATA AHCI Controller",      MOD_DRV,  mod_ahci_probe,  mod_ahci_init,     0, 0, 0 },
    { "nvme",      "NVMe PCIe Storage Controller", MOD_DRV, mod_nvme_probe, mod_nvme_init,    0, 0, 0 },
    { "acpi",      "ACPI Power Management",     MOD_DRV,  0,               mod_acpi_init,     0, 0, 0 },
    { "ac97",      "AC'97 Audio Controller",    MOD_DRV,  mod_ac97_probe,  mod_ac97_init,     0, 0, 0 },
    { "es1370",    "Ensoniq AudioPCI ES1370",   MOD_DRV,  mod_es1370_probe,mod_es1370_init,   0, 0, 0 },
    { "sb16",      "Sound Blaster 16 DSP",      MOD_DRV,  mod_sb16_probe,  mod_sb16_init,     0, 0, 0 },
    { "rtl8139",   "Fast Ethernet NIC",         MOD_DRV,  mod_rtl8139_probe,mod_rtl8139_init, 0, 0, 0 },
    { "e1000",     "PRO/1000 Gigabit NIC",      MOD_DRV,  mod_e1000_probe, mod_e1000_init,    0, 0, 0 },
    { "pcnet",     "AMD PCnet FAST III NIC",    MOD_DRV,  mod_pcnet_probe, mod_pcnet_init,    0, 0, 0 },
    { "ne2k",      "NE2000 / RTL8029 NIC",      MOD_DRV,  mod_ne2k_probe,  mod_ne2k_init,     0, 0, 0 },
    { "uhci",      "USB UHCI Root Hub",         MOD_DRV,  mod_uhci_probe,  mod_uhci_init,     0, 0, 0 },
    { "xhci",      "USB 3.0 xHCI Root Hub",     MOD_DRV,  mod_xhci_probe,  mod_xhci_init,     0, 0, 0 },
    { "gameport",  "Analog Gameport Joystick",  MOD_DRV,  mod_gameport_probe,mod_gameport_init,0, 0, 0 },
    { "tcpip",     "TCP/IP Network Stack",      MOD_NET,  0,               mod_netstack_init, 0, 0, 0 },
    { "scheduler", "SMP Multitasking Engine",   MOD_PROC, 0,               mod_sched_init,    0, 0, 0 },
    { "alpc",      "NT ALPC Executive Subsystem", MOD_PROC, 0,             mod_alpc_init,     0, 0, 0 },
    { "ntos",      "NT Executive & Win32k/LXSS/3D Engine", MOD_PROC, 0,    mod_ntos_init,     0, 0, 0 }
};


extern uintptr_t __end;
uintptr_t start_addr = (uintptr_t)&__end;

void x86_arch_init(unsigned long magic, unsigned long addr)
{
    /* 1. Absolute earliest C-level COM1 serial init and diagnostic logging */
    early_serial_init();
    early_serial_puts("\r\n[KERNEL-C] Reached x86_arch_init. Performing structural validation on boot info...\r\n");

    /* 2. Validate boot structures and bounds before any subsystem setup */
    azami_boot_info_t boot_data;
    if (!boot_info_validate_and_parse(magic, addr, &boot_data)) {
        early_panic("Boot info structure validation failed (bad magic or out-of-bounds pointer)!");
    }

    /* 3. Basic terminal settings */
    terminal_clean();
    kprintf("\n"
            "  ____ _____  _    __  __ ___ ___  ____  \n"
            " / \\  /__  / / \\  |  \\/  |_ _/ _ \\/ ___| \n"
            "/ _ \\   / / / _ \\ | |\\/| || | | | \\___ \\ \n"
            "/ ___ \\ / /_/ ___ \\| |  | || | |_| |___) |\n"
            "/_/   \\_\\____/_/   \\_\\_|  |_|___\\___/|____/ \n"
            "AzamiOS v2.0 Modern Kernel — Fast, Modular & Easy to Debug\n\n");

    /* 4. Display validated boot protocol and memory layout */
    kprintf("Validated Boot Protocol ID %d (Magic: 0x%x, Info Addr: 0x%x)\n",
            (int)boot_data.protocol, (uint32_t)boot_data.raw_magic, (uint32_t)boot_data.raw_addr);
    kprintf("Memory Status: Lower=%u KB, Upper=%u KB, Total=%u KB (~%u MB)\n",
            (uint32_t)boot_data.mem_lower_kb, (uint32_t)boot_data.mem_upper_kb,
            (uint32_t)boot_data.total_mem_kb, (uint32_t)(boot_data.total_mem_kb / 1024));
    if (boot_data.has_framebuffer) {
        kprintf("Framebuffer at 0x%x (%ux%u, %u bpp, pitch %u)\n",
                (uint32_t)boot_data.fb_addr, boot_data.fb_width, boot_data.fb_height,
                boot_data.fb_bpp, boot_data.fb_pitch);
    }
    if (boot_data.initrd_paddr) {
        kprintf("Initrd/Ramdisk at 0x%x (size: %u bytes)\n",
                (uint32_t)boot_data.initrd_paddr, (uint32_t)boot_data.initrd_size);
    }

    g_is_uefi = (boot_data.protocol == BOOT_PROTO_UEFI);
    g_mem_size_kb = (uint32_t)boot_data.total_mem_kb;
    g_initrd_loc = (uintptr_t)boot_data.initrd_paddr;
    g_cached_boot_info = &boot_data;

    /* 5. Initialize CPU core tables */
    gdt_init();
    init_isr();
    init_syscalls();
    stack_guard_init();
    aslr_init();

    uintptr_t free_mem_start = (uintptr_t)boot_data.safe_free_mem_start;
    free_mem_start = (free_mem_start + 4095) & ~4095;
    g_bitmap_addr = free_mem_start;
    uintptr_t bitmap_size_bytes = ((g_mem_size_kb * 1024 / 4096) + 7) / 8;
    g_free_mem_start = (g_bitmap_addr + bitmap_size_bytes + 4095) & ~4095;

    /* Register and bootstrap all modular kernel subsystems */
    for (uint32_t i = 0; i < sizeof(kmods)/sizeof(kmods[0]); i++) {
        module_register(&kmods[i]);
    }
    module_init_all();

    /* Launch Window Manager userspace environment */
    execute_program("wm");
}