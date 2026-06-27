#include "./include/x86arch.h"
#include "./include/gdt.h"
#include "./include/isr.h"
#include "../drivers/include/terminal.h"
#include "../klibc/include/stdio.h"
#include "../drivers/include/pit.h"
#include "../drivers/include/keyboard.h"
#include "../drivers/include/kbc.h"
#include "../mem/include/pmm.h"
#include "../mem/include/paging.h"
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
#include "./include/smp.h"
#include "../drivers/include/ac97.h"
#include "../drivers/include/acpi.h"
#include "../drivers/include/rtl8139.h"
#include "../drivers/include/e1000.h"
#include "../drivers/include/sb16.h"
#include "../drivers/include/gameport.h"
#include "../drivers/include/pcnet.h"
#include "../drivers/include/ne2k.h"
#include "../drivers/include/ahci.h"
#include "../drivers/include/es1370.h"
#include "../drivers/include/uhci.h"
#include "../drivers/include/net_stack.h"
#include "../drivers/include/pci.h"
#include "../klibc/include/port.h"
#include "../module/include/module.h"

static uint32_t g_mem_size_kb = 0;
static uint32_t g_bitmap_addr = 0;
static uint32_t g_free_mem_start = 0;
static uint32_t g_initrd_loc = 0;
static block_device_t *g_ata_dev = (block_device_t*)0;

static int mod_rtc_init(void) { rtc_init(); init_mouse(); init_keyboard(); return 0; }
static int mod_pmm_init(void) { pmm_init(g_mem_size_kb, g_bitmap_addr); pmm_deinit_region(g_free_mem_start, 16*1024*1024); return 0; }
static int mod_paging_init(void) { paging_init(); return 0; }
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
static int mod_es1370_init(void) { es1370_init(); return 0; }
static int mod_uhci_init(void) { uhci_init(); return 0; }
static int mod_netstack_init(void) { net_stack_init(); return 0; }
static int mod_sched_init(void) { process_init(); scheduler_init(); smp_init(); return 0; }

static bool pci_match(uint16_t vendor, uint16_t device) {
    uint8_t b, s, f;
    return pci_find_device(vendor, device, &b, &s, &f);
}

static bool mod_ahci_probe(void) {
    for (uint16_t bus = 0; bus < 256; bus++) {
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
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                if (pci_config_read16(bus, slot, func, 0x00) == 0xFFFF) continue;
                uint32_t class_rev = pci_config_read32(bus, slot, func, 0x08);
                if ((class_rev >> 24) == 0x0C && ((class_rev >> 16) & 0xFF) == 0x03 && ((class_rev >> 8) & 0xFF) == 0x00) return true;
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
    { "rtc",       "RTC & Peripherals",         MOD_CORE, 0,               mod_rtc_init,      0, 0, 0 },
    { "pmm",       "Physical Memory Manager",   MOD_MEM,  0,               mod_pmm_init,      0, 0, 0 },
    { "paging",    "Virtual Memory Manager",    MOD_MEM,  0,               mod_paging_init,   0, 0, 0 },
    { "tarfs",     "TarFS RAMDisk Initrd",      MOD_FS,   0,               mod_tarfs_init,    0, 0, 0 },
    { "ata",       "ATA IDE Storage Driver",    MOD_DRV,  0,               mod_ata_init,      0, 0, 0 },
    { "fat32",     "FAT32 Storage Driver",      MOD_FS,   0,               mod_fat32_init,    0, 0, 0 },
    { "ahci",      "SATA AHCI Controller",      MOD_DRV,  mod_ahci_probe,  mod_ahci_init,     0, 0, 0 },
    { "acpi",      "ACPI Power Management",     MOD_DRV,  0,               mod_acpi_init,     0, 0, 0 },
    { "ac97",      "AC'97 Audio Controller",    MOD_DRV,  mod_ac97_probe,  mod_ac97_init,     0, 0, 0 },
    { "es1370",    "Ensoniq AudioPCI ES1370",   MOD_DRV,  mod_es1370_probe,mod_es1370_init,   0, 0, 0 },
    { "sb16",      "Sound Blaster 16 DSP",      MOD_DRV,  mod_sb16_probe,  mod_sb16_init,     0, 0, 0 },
    { "rtl8139",   "Fast Ethernet NIC",         MOD_DRV,  mod_rtl8139_probe,mod_rtl8139_init, 0, 0, 0 },
    { "e1000",     "PRO/1000 Gigabit NIC",      MOD_DRV,  mod_e1000_probe, mod_e1000_init,    0, 0, 0 },
    { "pcnet",     "AMD PCnet FAST III NIC",    MOD_DRV,  mod_pcnet_probe, mod_pcnet_init,    0, 0, 0 },
    { "ne2k",      "NE2000 / RTL8029 NIC",      MOD_DRV,  mod_ne2k_probe,  mod_ne2k_init,     0, 0, 0 },
    { "uhci",      "USB UHCI Root Hub",         MOD_DRV,  mod_uhci_probe,  mod_uhci_init,     0, 0, 0 },
    { "gameport",  "Analog Gameport Joystick",  MOD_DRV,  mod_gameport_probe,mod_gameport_init,0, 0, 0 },
    { "tcpip",     "TCP/IP Network Stack",      MOD_NET,  0,               mod_netstack_init, 0, 0, 0 },
    { "scheduler", "SMP Multitasking Engine",   MOD_PROC, 0,               mod_sched_init,    0, 0, 0 }
};


extern uint32_t __end;
uint32_t start_addr = (uint32_t)&__end;

void x86_arch_init(unsigned long magic, unsigned long addr)
{
    // 1. basic terminal settings.
    terminal_clean();
    kprintf("Welcome to AzamiOS!\n");

    // 2. Verify Bootloader(GRUB)
    if(magic != 0x2BADB002){
        kprintf("PANIC: Bootloader Error! Wrong magic number.\n");
        return; // halt kernel 
    }

    // 3. initialize CPU core tables
    gdt_init();
    init_isr();
    init_syscalls();

    multiboot_info_t* bootinfo = (multiboot_info_t*)addr;
    g_mem_size_kb = bootinfo->mem_lower + bootinfo->mem_upper + 1024;

    uint32_t free_mem_start = (uint32_t)&__end;
    if (bootinfo->flags & MULTIBOOT_INFO_MODS) {
        if (bootinfo->mods_count > 0) {
            multiboot_module_t *mods = (multiboot_module_t*)bootinfo->mods_addr;
            g_initrd_loc = mods[0].mod_start;
            for (uint32_t m = 0; m < bootinfo->mods_count; m++) {
                if (mods[m].mod_end > free_mem_start) free_mem_start = mods[m].mod_end;
            }
        }
    }
    free_mem_start = (free_mem_start + 4095) & ~4095;
    g_bitmap_addr = free_mem_start;
    uint32_t bitmap_size_bytes = ((g_mem_size_kb * 1024 / 4096) + 7) / 8;
    g_free_mem_start = (g_bitmap_addr + bitmap_size_bytes + 4095) & ~4095;

    /* Register and bootstrap all modular kernel subsystems */
    for (uint32_t i = 0; i < sizeof(kmods)/sizeof(kmods[0]); i++) {
        module_register(&kmods[i]);
    }
    module_init_all();

    /* Launch Window Manager userspace environment */
    execute_program("wm");
}