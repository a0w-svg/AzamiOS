#include "include/syscall.h"
#include "../arch/include/isr.h"
#include "../arch/include/smp.h"
#include "../klibc/include/stdio.h"
#include "../klibc/include/string.h"
#include "../klibc/include/port.h"
#include "../drivers/include/keyboard.h"
#include "../drivers/include/rtc.h"
#include "../drivers/include/mouse.h"
#include "../drivers/include/gfx.h"
#include "../drivers/include/acpi.h"
#include "../drivers/include/rtl8139.h"
#include "../drivers/include/net_stack.h"
#include "../filesystem/include/vfs.h"
#include "../filesystem/include/tarfs.h"
#include "../module/include/module.h"
#include "include/exec.h"
#include "../proc/include/process.h"
#include "../proc/include/scheduler.h"
#include "../mem/include/access_control.h"
#include "../proc/include/lpc.h"
#include "../proc/include/exec_server.h"


static vfs_file_t *g_file_table[16];

/*
 * VALIDATE_PTR(ptr, size) — check user-supplied pointer before kernel touch.
 * If invalid: log warning, set return value to -1, and return from syscall_handler.
 * size=0 means "just check it's not NULL and not in kernel range".
 */
#define VALIDATE_PTR(ptr, size)                                            \
    do {                                                                    \
        if (!syscall_validate_ptr(r, (uintptr_t)(ptr), (uintptr_t)(size))) {\
            r->eax = (uintptr_t)-1;                                         \
            return cpu->active_context;                                      \
        }                                                                   \
    } while (0)

/*
 * DENY_IF_PORT_DISALLOWED — block unauthorized hardware I/O port access from ring-3.
 * Returns -1 to the calling process if port is not in allowlist.
 */
#define DENY_IF_PORT_DISALLOWED(port)                                       \
    do {                                                                    \
        if (!io_port_allowed(r, (uint16_t)(port))) {                        \
            kprintf("[AC] DENIED: ring-3 attempt to use disallowed I/O port 0x%x (syscall %u)\n", \
                    (unsigned)(uint16_t)(port), (unsigned)r->eax);           \
            r->eax = (uintptr_t)-1;                                         \
            return cpu->active_context;                                      \
        }                                                                   \
    } while (0)


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
    process_t *cur = scheduler_get_current();
    if (cur) {
        kprintf("\nexec: process '%s' (PID %d) exited with code %d\n", cur->name, cur->pid, code);
        cur->state = PROC_DEAD;
        if (cur->pid > 1) {
            scheduler_schedule();
            return;
        }
    }
    kprintf("\nexec: process '%s' exited with code %d\n", g_current_program, code);
    if (strcmp(g_current_program, g_return_program) != 0 && g_return_program[0] != '\0') {
        kprintf("exec: returning control to parent shell '%s'...\n", g_return_program);
        execute_program(g_return_program);
        return;
    }
    acpi_poweroff();
}

/* ─── main interrupt handler (int $0x80) ────────────────────────────────── */

uintptr_t syscall_handler(registers_t *r) {
    cpu_data_t *cpu = smp_get_current_cpu();
    cpu->active_context = (uintptr_t)r;

    uint32_t syscall_number = r->eax;
    process_t *cur = scheduler_get_current();
    if (cur) {
        kprintf("sys: pid=%u call=%u ebx=0x%x\n", cur->pid, syscall_number, (uint32_t)r->ebx);
    }

    switch (syscall_number) {
        case 0: /* write string */
            VALIDATE_PTR(r->ebx, 0);
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
            break;

        case 4: /* get rtc time — writes to user buffer */
            VALIDATE_PTR(r->ebx, sizeof(time_t));
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
                VALIDATE_PTR(r->ebx, sizeof(mouse_state_t));
                memcpy((void*)r->ebx, mouse_get_state(), sizeof(mouse_state_t));
            }
            break;

        case 9: /* draw text */
            VALIDATE_PTR(r->edx, 0);
            gfx_draw_text((int)r->ebx, (int)r->ecx, (const char*)r->edx, (uint32_t)r->esi, (uint32_t)r->edi);
            break;

        case 10: /* exec */
            VALIDATE_PTR(r->ebx, 0);
            execute_program((char*)r->ebx);
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
                VALIDATE_PTR(r->ecx, r->edx);
                char *b = (char*)r->ecx;
                char c = kb_getchar();
                if (c) { b[0] = c; r->eax = 1; }
                else { r->eax = 0; }
            } else if (r->ebx >= 3 && r->ebx < 16 && g_file_table[r->ebx]) {
                if (r->ecx && r->edx > 0) {
                    VALIDATE_PTR(r->ecx, r->edx);
                    if (g_vfs_port && g_vfs_port->active) {
                        lpc_msg_t req, reply;
                        memset(&req, 0, sizeof(req));
                        req.type = LPC_REQ_VFS_READ;
                        req.arg1 = (uint32_t)(uintptr_t)g_file_table[r->ebx];
                        req.section_ptr = (uintptr_t)r->ecx;
                        req.section_size = (uint32_t)r->edx;
                        if (lpc_send_request(g_vfs_port, &req, &reply) >= 0) {
                            r->eax = reply.arg1;
                        } else { r->eax = 0; }
                    } else {
                        r->eax = vfs_file_read(g_file_table[r->ebx], r->edx, (uint8_t*)r->ecx);
                    }
                } else { r->eax = 0; }
            } else { r->eax = 0; }
            break;

        case 20: /* sys_write: fd=ebx, buf=ecx, len=edx */
            if ((r->ebx == 1 || r->ebx == 2) && r->ecx && r->edx > 0) {
                VALIDATE_PTR(r->ecx, r->edx);
                const char *b = (const char*)r->ecx;
                for (uint32_t i = 0; i < r->edx; i++) syscall_print_char(b[i]);
                r->eax = r->edx;
            } else if (r->ebx >= 3 && r->ebx < 16 && g_file_table[r->ebx]) {
                if (r->ecx && r->edx > 0) {
                    VALIDATE_PTR(r->ecx, r->edx);
                    if (g_vfs_port && g_vfs_port->active) {
                        lpc_msg_t req, reply;
                        memset(&req, 0, sizeof(req));
                        req.type = LPC_REQ_VFS_WRITE;
                        req.arg1 = (uint32_t)(uintptr_t)g_file_table[r->ebx];
                        req.section_ptr = (uintptr_t)r->ecx;
                        req.section_size = (uint32_t)r->edx;
                        if (lpc_send_request(g_vfs_port, &req, &reply) >= 0) {
                            r->eax = reply.arg1;
                        } else { r->eax = 0; }
                    } else {
                        r->eax = vfs_file_write(g_file_table[r->ebx], r->edx, (uint8_t*)r->ecx);
                    }
                } else { r->eax = 0; }
            } else { r->eax = 0; }
            break;

        case 21: /* sys_open: path=ebx, flags=ecx */
            {
                const char *path = (const char*)r->ebx;
                if (path) {
                    VALIDATE_PTR(r->ebx, 0);
                    vfs_file_t *f = NULL;
                    if (g_vfs_port && g_vfs_port->active) {
                        lpc_msg_t req, reply;
                        memset(&req, 0, sizeof(req));
                        req.type = LPC_REQ_VFS_OPEN;
                        req.section_ptr = (uintptr_t)path;
                        req.arg1 = (uint32_t)r->ecx;
                        if (lpc_send_request(g_vfs_port, &req, &reply) == 0) {
                            f = (vfs_file_t*)(uintptr_t)reply.arg1;
                        }
                    } else {
                        f = vfs_open_file(path, (uint32_t)r->ecx);
                    }
                    if (f) {
                        for (int fd = 3; fd < 16; fd++) {
                            if (g_file_table[fd] == NULL) {
                                g_file_table[fd] = f;
                                r->eax = fd;
                                goto open_done;
                            }
                        }
                        vfs_file_close(f);
                    }
                }
                r->eax = -1;
            open_done: break;
            }

        case 22: /* sys_close: fd=ebx */
            if (r->ebx >= 3 && r->ebx < 16 && g_file_table[r->ebx]) {
                if (g_vfs_port && g_vfs_port->active) {
                    lpc_msg_t req, reply;
                    memset(&req, 0, sizeof(req));
                    req.type = LPC_REQ_VFS_CLOSE;
                    req.arg1 = (uint32_t)(uintptr_t)g_file_table[r->ebx];
                    lpc_send_request(g_vfs_port, &req, &reply);
                } else {
                    vfs_file_close(g_file_table[r->ebx]);
                }
                g_file_table[r->ebx] = NULL;
                r->eax = 0;
            } else { r->eax = -1; }
            break;

        case 23: /* sys_sbrk */
            r->eax = 0;
            break;

        case 24: /* sys_getpid */
            if (g_proc_port && g_proc_port->active) {
                lpc_msg_t req, reply;
                memset(&req, 0, sizeof(req));
                req.type = LPC_REQ_PROC_GETPID;
                lpc_send_request(g_proc_port, &req, &reply);
                r->eax = reply.arg1;
            } else {
                r->eax = 1;
            }
            break;

        case 25: /* sys_lsmod: buf=ebx, max_len=ecx */
            if (r->ebx && r->ecx > 0) {
                VALIDATE_PTR(r->ebx, r->ecx);
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

        case 29: /* sys_modreload: name=ebx */
            if (r->ebx) {
                VALIDATE_PTR(r->ebx, 0);
                r->eax = module_reload((const char*)r->ebx);
            } else { r->eax = -1; }
            break;

        case 30: /* sys_poweroff */
            acpi_poweroff();
            break;

        case 31: /* sys_map_fb */
            r->eax = gfx_map_backbuffer();
            break;

        case 32: /* sys_ipc_send / sys_lpc_send */
        case 53: /* sys_lpc_send: port_id=ebx, req=ecx, reply=edx */
            if (r->ecx) {
                VALIDATE_PTR(r->ecx, sizeof(lpc_msg_t));
                if (r->edx) VALIDATE_PTR(r->edx, sizeof(lpc_msg_t));
                lpc_port_t *port = lpc_get_port_by_id((uint32_t)r->ebx);
                if (port) {
                    r->eax = lpc_send_request(port, (lpc_msg_t*)r->ecx, (lpc_msg_t*)r->edx);
                } else { r->eax = (uint32_t)-1; }
            } else { r->eax = (uint32_t)-1; }
            break;

        case 33: /* sys_ipc_recv */
            if (r->ecx) {
                VALIDATE_PTR(r->ecx, sizeof(lpc_msg_t));
                lpc_port_t *port = lpc_get_port_by_id((uint32_t)r->ebx);
                if (port) {
                    r->eax = lpc_receive(port, (lpc_msg_t*)r->ecx);
                } else { r->eax = (uint32_t)-1; }
            } else { r->eax = (uint32_t)-1; }
            break;

        case 34: /* sys_inb: port=ebx */
            DENY_IF_PORT_DISALLOWED(r->ebx);
            r->eax = inb((uint16_t)r->ebx);
            break;

        case 35: /* sys_outb: port=ebx, val=ecx */
            DENY_IF_PORT_DISALLOWED(r->ebx);
            outb((uint16_t)r->ebx, (uint8_t)r->ecx);
            break;

        case 36: /* sys_inw: port=ebx */
            DENY_IF_PORT_DISALLOWED(r->ebx);
            r->eax = inw((uint16_t)r->ebx);
            break;

        case 37: /* sys_outw: port=ebx, val=ecx */
            DENY_IF_PORT_DISALLOWED(r->ebx);
            outw((uint16_t)r->ebx, (uint16_t)r->ecx);
            break;

        case 38: /* sys_inl: port=ebx */
            DENY_IF_PORT_DISALLOWED(r->ebx);
            r->eax = inl((uint16_t)r->ebx);
            break;

        case 39: /* sys_outl */
            DENY_IF_PORT_DISALLOWED(r->ebx);
            outl((uint16_t)r->ebx, (uint32_t)r->ecx);
            break;

        case 40: /* sys_mount: dev=ebx, mnt=ecx, type=edx */
            VALIDATE_PTR(r->ebx, 0);
            VALIDATE_PTR(r->ecx, 0);
            if (r->edx) VALIDATE_PTR(r->edx, 0);
            if (g_vfs_port && g_vfs_port->active) {
                lpc_msg_t req, reply;
                memset(&req, 0, sizeof(req));
                req.type = LPC_REQ_VFS_MOUNT;
                req.section_ptr = (uintptr_t)r->ebx;
                req.arg1 = (uint32_t)r->ecx;
                req.arg2 = (uint32_t)r->edx;
                r->eax = lpc_send_request(g_vfs_port, &req, &reply);
            } else {
                r->eax = vfs_mount((const char*)r->ebx, (const char*)r->ecx, (const char*)r->edx);
            }
            break;

        case 41: /* sys_unmount: mnt=ebx */
            VALIDATE_PTR(r->ebx, 0);
            if (g_vfs_port && g_vfs_port->active) {
                lpc_msg_t req, reply;
                memset(&req, 0, sizeof(req));
                req.type = LPC_REQ_VFS_UNMOUNT;
                req.section_ptr = (uintptr_t)r->ebx;
                r->eax = lpc_send_request(g_vfs_port, &req, &reply);
            } else {
                r->eax = vfs_unmount((const char*)r->ebx);
            }
            break;

        case 42: /* sys_gfx_vsync */
            gfx_vsync();
            break;

        case 43: /* sys_net_socket: proto=ebx */
            r->eax = net_tcp_socket_open(NULL);
            break;

        case 44: /* sys_net_bind: sock=ebx, port=ecx */
            r->eax = net_tcp_bind((int)r->ebx, (uint16_t)r->ecx);
            break;

        case 45: /* sys_net_listen: sock=ebx */
            r->eax = net_tcp_listen((int)r->ebx);
            break;

        case 46: /* sys_net_connect: sock=ebx, ip=ecx, port=edx */
            r->eax = net_tcp_connect((int)r->ebx, (uint32_t)r->ecx, (uint16_t)r->edx);
            break;

        case 47: /* sys_net_send: sock=ebx, buf=ecx, len=edx */
            if (r->ecx && r->edx > 0) {
                VALIDATE_PTR(r->ecx, r->edx);
            }
            r->eax = net_tcp_send((int)r->ebx, (const uint8_t*)r->ecx, (uint32_t)r->edx);
            break;

        case 48: /* sys_net_close: sock=ebx */
            net_tcp_close((int)r->ebx);
            r->eax = 0;
            break;

        case 49: /* sys_thread_create: entry=ebx, arg=ecx, stack=edx */
            VALIDATE_PTR(r->ebx, 0);
            if (r->edx) VALIDATE_PTR(r->edx - sizeof(uintptr_t), sizeof(uintptr_t));
            extern int process_thread_create(uintptr_t entry, uintptr_t arg, uintptr_t user_stack);
            r->eax = process_thread_create((uintptr_t)r->ebx, (uintptr_t)r->ecx, (uintptr_t)r->edx);
            break;

        case 50: /* sys_yield */
            scheduler_schedule();
            break;

        case 51: /* sys_fork */
            extern int process_fork(registers_t *regs);
            r->eax = process_fork(r);
            break;

        case 52: /* sys_lpc_connect: name=ebx */
            if (r->ebx) {
                VALIDATE_PTR(r->ebx, 0);
                lpc_port_t *port = lpc_connect_port((const char*)r->ebx);
                r->eax = port ? port->port_id : (uint32_t)-1;
            } else { r->eax = (uint32_t)-1; }
            break;

        case 54: /* sys_lpc_stat */
            VALIDATE_PTR(r->ebx, r->ecx);
            if (r->ecx > 0 && r->ecx <= 0x10000) {
                r->eax = lpc_get_status_table((char*)r->ebx, (int)r->ecx);
            } else { r->eax = (uint32_t)-1; }
            break;

        default:
            r->eax = (uint32_t)-1; /* ENOSYS for unimplemented syscalls */
    }
    return cpu->active_context;
}

/* ─── initialization ──────────────────────────────────────────────────────── */

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

extern void syscall_entry(void);

void init_syscalls(void) {
    /* int 0x80 soft-syscall path is handled by isr_common_stub -> exception_handler.
     * The fast SYSCALL path uses the dedicated syscall_entry asm stub.
     * No separate register_interrupt_handler needed; isr_128 routes through
     * exception_handler which dispatches to interrupt_handlers[128] if registered. */

    /* Enable SCE (System Call Enable) in IA32_EFER MSR (0xC0000080, bit 0) */
    uint64_t efer = rdmsr(0xC0000080);
    efer |= 1;
    wrmsr(0xC0000080, efer);

    /*
     * Configure IA32_STAR MSR (0xC0000081):
     * Bits 32-47: SYSCALL CS/SS target selector (0x08 -> Kernel Code 0x08, Kernel Data 0x10)
     * Bits 48-63: SYSRET base selector (0x18 -> SS is 0x18+8=0x20|3=0x23, CS is 0x18+16=0x28|3=0x2B)
     */
    uint64_t star = ((uint64_t)0x0018 << 48) | ((uint64_t)0x0008 << 32);
    wrmsr(0xC0000081, star);

    /* Configure IA32_LSTAR MSR (0xC0000082): target RIP for SYSCALL instruction */
    wrmsr(0xC0000082, (uintptr_t)syscall_entry);

    /* Configure IA32_FMASK MSR (0xC0000084): clear IF (bit 9), TF (bit 8), DF (bit 10) on entry */
    wrmsr(0xC0000084, 0x700);

    kprintf("syscall: MSRs configured (STAR=0x%llx, LSTAR=0x%llx, FMASK=0x700)\n",
            (unsigned long long)star, (unsigned long long)(uintptr_t)syscall_entry);
}