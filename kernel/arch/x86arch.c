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
#include "../drivers/include/serial.h"
#include "../klibc/include/string.h"
#include "../syscall/include/syscall.h"
#include "../filesystem/include/tarfs.h"
#include "../filesystem/include/vfs.h"
extern uint32_t __end;
void x86_arch_init(unsigned long magic, unsigned long addr)
{
    // 1. basic terminal settings.
    terminal_clean();
    printf("Welcome to AzamiOS!\n");

    // 2. Verify Bootloader(GRUB)
    if(magic != 0x2BADB002){
        printf("PANIC: Bootloader Error! Wrong magic number.\n");
        return; // halt kernel 
    }

    // 3. initialize CPU tables (GDT and interrupts)
    gdt_init();
    init_isr();
    init_syscalls();
    // 4. Initialize memory management (PMM and VMM)
    multiboot_info_t* bootinfo = (multiboot_info_t*)addr;

    // obtain RAM memory capacity in KB.
    uint32_t mem_size_kb = bootinfo->mem_lower + bootinfo->mem_upper + 1024;
    printf("Found RAM: %d KB\n", mem_size_kb);

    // initialize Physical Memory Manager (bitmap right after kernel)
    pmm_init(mem_size_kb, (uint32_t)&__end);
    // initialize Paging (Identity Mapping on boot)
    paging_init();
    printf("test initrd: \n");
    if(bootinfo->flags & MULTIBOOT_INFO_MODS){
        if(bootinfo->mods_count > 0){
            multiboot_module_t *mod = (multiboot_module_t*)bootinfo->mods_addr;
            uint32_t initrd_location = mod->mod_start;
            tarfs_init(initrd_location);
            printf("found directory: %s", fs_root->finddir(fs_root, "test")->name);
        }
    }
}