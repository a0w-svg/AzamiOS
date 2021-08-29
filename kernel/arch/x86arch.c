#include "./include/x86arch.h"
#include "./include/gdt.h"
#include "./include/isr.h"
#include "../drivers/include/terminal.h"
#include "../klibc/include/stdio.h"
#include "../drivers/include/pit.h"
#include "../drivers/include/keyboard.h"
#include "../drivers/include/kbc.h"
#include "../mem/include/mmp.h"
#include "../../thirdparty/multiboot.h"
#include "../drivers/include/rtc.h"
void x86_arch_init(unsigned long magic, unsigned long addr)
{
    gdt_init();
    terminal_clean();
    init_isr();
    __asm__ volatile("sti");
    init_pit();
    set_pit_phase(50);
    heap_init();
    multiboot_info_t* bootinfo = (multiboot_info_t*)addr;
    uint32_t mem_size = 1024 + bootinfo->mem_lower + bootinfo->mem_upper * 64;
    printf("mem_size: %d KB\n", mem_size);
    uint32_t *a = (uint32_t*)kmalloc(5 * sizeof(uint32_t));
    printf("%x\n", a);
    printf("test heap: \n");
    for(int i = 0; i < 5; i++)
        a[i] = i+1;
    printf("values stored into allocated memory space: ");
    for(int i = 0; i < 5; i++)
        printf("%d ", a[i]);
    init_keyboard();
    rtc_init();
    printf("\n%x", kb_device_check_type());
    time_t *time = kmalloc(sizeof(time_t));
    while(true)
    {
        rtc_get_time(time);
        printf("%d:%d:%d\n", time->hour, time->minute, time->second);
    }
}