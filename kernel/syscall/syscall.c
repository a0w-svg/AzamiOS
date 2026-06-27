#include "include/syscall.h"
#include "../arch/include/isr.h"
#include "../klibc/include/stdio.h"
#include "../klibc/include/string.h"
#include "../drivers/include/keyboard.h"
#include "../drivers/include/rtc.h"
#include "../drivers/include/mouse.h"
#include "../drivers/include/gfx.h"
#include "../drivers/include/acpi.h"
#include "../drivers/include/rtl8139.h"
#include "../drivers/include/net_stack.h"
#include "../filesystem/include/vfs.h"
#include "../module/include/module.h"
#include "include/exec.h"

static fs_node_t *g_file_table[16];
static uint32_t   g_file_offsets[16];

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
    acpi_poweroff();
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

        case 12: /* put pixel: ebx=x, ecx=y, edx=color */
            gfx_put_pixel((int)r->ebx, (int)r->ecx, (uint32_t)r->edx);
            break;

        case 13: /* acpi info */
            acpi_print_info();
            break;

        case 14: /* acpi reboot */
            acpi_reboot();
            break;

        case 15: /* nic status */
            rtl8139_print_status();
            break;

        case 16: /* nic test broadcast */
            rtl8139_send_test_packet();
            break;

        case 17: /* ping gateway 10.0.2.2 */
            net_send_ping((10 << 24) | (0 << 16) | (2 << 8) | 2);
            break;

        case 18: /* arp cache table */
            net_print_arp_cache();
            break;

        case 19: /* sys_read: fd=ebx, buf=ecx, len=edx */
            if (r->ebx == 0 && r->edx > 0 && r->ecx) {
                char *b = (char*)r->ecx;
                char c = kb_getchar();
                if (c) { b[0] = c; r->eax = 1; }
                else { r->eax = 0; }
            } else if (r->ebx >= 3 && r->ebx < 16 && g_file_table[r->ebx]) {
                fs_node_t *fn = g_file_table[r->ebx];
                if (fn->read && r->ecx && r->edx > 0) {
                    uint32_t nread = fn->read(NULL, fn, g_file_offsets[r->ebx], r->edx, (uint8_t*)r->ecx);
                    g_file_offsets[r->ebx] += nread;
                    r->eax = nread;
                } else { r->eax = 0; }
            } else { r->eax = 0; }
            break;

        case 20: /* sys_write: fd=ebx, buf=ecx, len=edx */
            if ((r->ebx == 1 || r->ebx == 2) && r->ecx && r->edx > 0) {
                const char *b = (const char*)r->ecx;
                for (uint32_t i = 0; i < r->edx; i++) syscall_print_char(b[i]);
                r->eax = r->edx;
            } else { r->eax = 0; }
            break;

        case 21: /* sys_open: path=ebx */
            {
                const char *path = (const char*)r->ebx;
                if (path && fs_root && fs_root->finddir) {
                    fs_node_t *fn = fs_root->finddir(fs_root, (char*)path);
                    if (fn) {
                        for (int fd = 3; fd < 16; fd++) {
                            if (g_file_table[fd] == NULL) {
                                g_file_table[fd] = fn;
                                g_file_offsets[fd] = 0;
                                r->eax = fd;
                                goto open_done;
                            }
                        }
                    }
                }
                r->eax = -1;
            open_done: break;
            }

        case 22: /* sys_close: fd=ebx */
            if (r->ebx >= 3 && r->ebx < 16) {
                g_file_table[r->ebx] = NULL;
                g_file_offsets[r->ebx] = 0;
                r->eax = 0;
            } else { r->eax = -1; }
            break;

        case 23: /* sys_sbrk */
            r->eax = 0;
            break;

        case 24: /* sys_getpid */
            r->eax = 1;
            break;

        case 25: /* sys_lsmod: buf=ebx, max_len=ecx */
            if (r->ebx && r->ecx > 0) {
                r->eax = module_get_summary_table((char*)r->ebx, (int)r->ecx);
            } else { r->eax = 0; }
            break;

        case 26: /* draw line: ebx=x0, ecx=y0, edx=x1, esi=y1, edi=color */
            gfx_draw_line((int)r->ebx, (int)r->ecx, (int)r->edx, (int)r->esi, (uint32_t)r->edi);
            break;

        case 27: /* draw circle: ebx=xc, ecx=yc, edx=r, esi=color */
            gfx_draw_circle((int)r->ebx, (int)r->ecx, (int)r->edx, (uint32_t)r->esi);
            break;

        case 28: /* fill circle: ebx=xc, ecx=yc, edx=r, esi=color */
            gfx_fill_circle((int)r->ebx, (int)r->ecx, (int)r->edx, (uint32_t)r->esi);
            break;

        default:

            kprintf("syscall: unknown syscall %d\n", syscall_number);
    }
}

/* ─── initialization ──────────────────────────────────────────────────────── */

void init_syscalls() {
    register_interrupt_handler(128, syscall_handler);
}