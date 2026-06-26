#include "include/syscall.h"
#include "../arch/include/isr.h"
#include "../klibc/include/stdio.h"
#include "../klibc/include/string.h"
#include "../drivers/include/keyboard.h"
#include "../drivers/include/rtc.h"
#include "../drivers/include/mouse.h"
#include "../drivers/include/gfx.h"
#include "include/exec.h"

/* ─── kernel-side syscall implementations ────────────────────────────────── */

static void syscall_print_string(char *str) {
    kprintf("%s", str);
}

static void syscall_print_char(char c) {
    putchar(c);
}

/**
 * syscall_exit — terminate the current user process.
 *
 * For now AzamiOS is single-process, so we just halt.  When a scheduler is
 * added this will become a yield / task-destroy call.
 */
static void syscall_exit(int code) {
    kprintf("\nexec: process exited with code %d\n", code);
    /* Halt the CPU until a scheduler reschedules or the system reboots */
    asm volatile("cli");
    for (;;) {
        asm volatile("hlt");
    }
}

/* ─── main interrupt handler (int $0x80) ────────────────────────────────── */

void syscall_handler(registers_t *r) {
    uint32_t syscall_number = r->eax;

    switch (syscall_number) {
        case 0: /* write string */
            syscall_print_string((char*)r->ebx);
            break;

        case 1: /* write single character */
            syscall_print_char((char)r->ebx);
            break;

        case 2: /* exit(code) */
            syscall_exit((int)r->ebx);
            break; /* not reached */

        case 3: /* read single character */
            r->eax = (uint32_t)kb_getchar();
            if (r->eax) {
                kprintf("[SYS_GETCHAR] Ring3 app dequeued char='%c' (0x%x)\n", (char)r->eax, r->eax);
            }
            break;

        case 4: /* get rtc time */
            rtc_get_time((time_t*)r->ebx);
            break;

        case 5: /* init Bochs VBE Linear Framebuffer */
            gfx_init_bga();
            break;

        case 6: /* flip graphics double buffer */
            gfx_flip();
            break;

        case 7: /* draw rect: ebx=x, ecx=y, edx=w, esi=h, edi=color */
            gfx_draw_rect((int)r->ebx, (int)r->ecx, (int)r->edx, (int)r->esi, (uint32_t)r->edi);
            break;

        case 8: /* get mouse state struct pointer in ebx */
            if (r->ebx) {
                memcpy((void*)r->ebx, mouse_get_state(), sizeof(mouse_state_t));
            }
            break;

        case 9: /* draw text: ebx=x, ecx=y, edx=str, esi=color, edi=bg_color */
            if (r->edx) {
                gfx_draw_text((int)r->ebx, (int)r->ecx, (const char*)r->edx, (uint32_t)r->esi, (uint32_t)r->edi);
            }
            break;

        case 10: /* exec(filename) */
            if (r->ebx) {
                execute_program((char*)r->ebx);
            }
            break;

        case 11: /* check kb buffer */
            r->eax = (uint32_t)kb_has_char();
            break;

        default:
            kprintf("syscall: unknown syscall %d\n", syscall_number);
    }
}

/* ─── initialization ──────────────────────────────────────────────────────── */

void init_syscalls() {
    register_interrupt_handler(128, syscall_handler);
}