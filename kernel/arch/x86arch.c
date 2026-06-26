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

    // 3. initialize CPU tables (GDT and interrupts)
    gdt_init();
    init_isr();
    init_syscalls();
    rtc_init();
    init_mouse();
    time_t now;
    rtc_get_time(&now);
    kprintf("RTC Time: %d:%d:%d %d/%d/%d\n", now.hour, now.minute, now.second, now.day, now.month, now.year);
    // 4. Initialize memory management (PMM and VMM)
    multiboot_info_t* bootinfo = (multiboot_info_t*)addr;

    // obtain RAM memory capacity in KB.
    uint32_t mem_size_kb = bootinfo->mem_lower + bootinfo->mem_upper + 1024;
    kprintf("Found RAM: %d KB\n", mem_size_kb);

    uint32_t free_mem_start = (uint32_t)&__end;
    if (bootinfo->flags & MULTIBOOT_INFO_MODS) {
        if (bootinfo->mods_count > 0) {
            multiboot_module_t *mods = (multiboot_module_t*)bootinfo->mods_addr;
            for (uint32_t m = 0; m < bootinfo->mods_count; m++) {
                if (mods[m].mod_end > free_mem_start) {
                    free_mem_start = mods[m].mod_end;
                }
            }
        }
    }
    free_mem_start = (free_mem_start + 4095) & ~4095;
    uint32_t bitmap_addr = free_mem_start;
    uint32_t bitmap_size_bytes = ((mem_size_kb * 1024 / 4096) + 7) / 8;
    free_mem_start = (bitmap_addr + bitmap_size_bytes + 4095) & ~4095;

    pmm_init(mem_size_kb, bitmap_addr);
    pmm_deinit_region(free_mem_start, 16 * 1024 * 1024);
    // initialize Paging (Identity Mapping on boot)
    paging_init();
    kprintf("test initrd: \n");
    if(bootinfo->flags & MULTIBOOT_INFO_MODS){
        if(bootinfo->mods_count > 0){
            multiboot_module_t *mod = (multiboot_module_t*)bootinfo->mods_addr;
            uint32_t initrd_location = mod->mod_start;
            tarfs_init(initrd_location);
            kprintf("List (tarfs):\n");
            uint32_t i = 0;
            directory_entry_t *dirent = NULL;
            while((dirent = fs_root->readdir(fs_root, i)) != 0){
                kprintf(" - %s (inode: %d)\n",dirent->name, dirent->inode);
                i++;
            }
        }
    }

    /* ── 5. Detect ATA primary master and mount FAT32 ──────────────────── */
    kprintf("\nATA init:\n");
    if (ata_init() == 0) {
        block_device_t *ata_dev = ata_get_device();
        fs_node_t *froot = fat32_init(ata_dev);
        if (froot) {
            kprintf("FAT32 mounted. Root directory:\n");
            uint32_t fi = 0;
            directory_entry_t *fe;
            while ((fe = froot->readdir(froot, fi)) != 0) {
                kprintf("  [FAT32] %s\n", fe->name);
                fi++;
            }
        } else {
            kprintf("fat32: mount failed\n");
        }
    }

    /* ── 6. Initialize Multi-process Scheduler & SMP Cores ───────────────── */
    process_init();
    scheduler_init();
    smp_init();

    /* ── 7. Launch the Window Manager user-space program from initrd ──────── */
    //execute_program("wm");
}