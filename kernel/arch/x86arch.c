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
#include "../drivers/include/serial.h"
#include "../klibc/include/string.h"
void x86_arch_init(unsigned long magic, unsigned long addr)
{
    gdt_init();
    terminal_clean();
    init_isr();
    init_pit();
    set_pit_phase(50);
    heap_init();
    rtc_init();
    multiboot_info_t* bootinfo = (multiboot_info_t*)addr;
    uint32_t mem_size = 1024 + bootinfo->mem_lower + bootinfo->mem_upper * 64;
    printf("mem_size: %d KB\n", mem_size);
    init_keyboard();
    time_t *time = kmalloc(sizeof(time_t));
    init_serial();
    while(1)
    {
        rtc_get_time(time);
        printf("%d:%d:%d\n", time->hour, time->minute, time->second);
    }
}