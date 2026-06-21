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
#include "../userspace/libc/include/stdio.h"
extern uint32_t __end;
uint32_t start_addr = (uint32_t)&__end;
extern void enter_usermode(uint32_t user_function, uint32_t user_stack_top);
void isolated_user_proccess(){
    while(1){
        asm volatile(
            "int $128\n"
            :
            : "a"(1), "b"('Z')
        );
    }
}
void setup_isolated_userspace(){
    // 1. Alloc free physical frames for usercode and user stack
    uint32_t user_code_phys = (uint32_t)pmm_alloc_block();
    uint32_t user_stack_phys = (uint32_t)pmm_alloc_block();
    kprintf("PMM Code: 0x%x, PMM Stack: 0x%x\n", user_code_phys, user_stack_phys);
    // map as high, virtual addresses Ring 3
    // is_kernel = 0, is_writable = 1;
    paging_map_page(user_code_phys, 0x40000000, 0, 1);
    paging_map_page(user_stack_phys, 0xBFFF000, 0, 1);
    //copy code from kernel to new isolated place
    uint8_t payload[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00,
        0xBB, 0x5A, 0x00, 0x00, 0x00,
        0xCD, 0x80,
        0xEB, 0xFE
    };
    extern page_directory_entry_t page_directory[];
    extern void switch_page_dir(void *page);
    switch_page_dir(page_directory);
    memcpy((void*)0x40000000, payload, sizeof(payload));
    enter_usermode(0x40000000, 0xC000000);
}

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
    // 4. Initialize memory management (PMM and VMM)
    multiboot_info_t* bootinfo = (multiboot_info_t*)addr;

    // obtain RAM memory capacity in KB.
    uint32_t mem_size_kb = bootinfo->mem_lower + bootinfo->mem_upper + 1024;
    kprintf("Found RAM: %d KB\n", mem_size_kb);

    // initialize Physical Memory Manager (bitmap right after kernel)
    pmm_init(mem_size_kb, (uint32_t)&__end);
    start_addr = (start_addr + 4096) & ~4096;
    pmm_deinit_region(start_addr, 8 * 1024 * 1024);
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
    setup_isolated_userspace();
}