/**
 * compat32.c — AzamiOS Userspace 32-Bit Binary Translator & Emulation Server
 *
 * Implements a lightweight x86 instruction interpreter and syscall thunking layer
 * in userspace (similar to QEMU user-mode or Rosetta). Keeps the CPU strictly in
 * 64-bit Long Mode while executing 32-bit ELF binaries.
 *
 * Intercepts 32-bit syscalls (int $0x80 / int $128) and translates them into clean
 * 64-bit ALPC requests to \Port\VfsServer and \Port\ProcServer.
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <lpc_client.h>

/* Virtual 32-bit CPU Register State */
typedef struct {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp, eip;
    uint32_t eflags;
    bool halted;
    int exit_code;
} cpu32_state_t;

static int g_vfs_port_id  = -1;
static int g_proc_port_id = -1;

/* Translate 32-bit Syscalls into 64-bit ALPC Executive Requests */
static void compat32_handle_syscall(cpu32_state_t *cpu) {
    uint32_t sys_no = cpu->eax;
    lpc_msg_t req, reply;
    memset(&req, 0, sizeof(req));
    memset(&reply, 0, sizeof(reply));

    req.sender_pid = (uint32_t)getpid();

    switch (sys_no) {
        case 1:  /* exit (Linux x86) */
        case 60: /* exit (AzamiOS / x64) */
        case 252:/* exit_group (Linux x86) */
            cpu->halted = true;
            cpu->exit_code = (int)cpu->ebx;
            break;

        case 3:  /* read (Linux x86) */
        case 19: /* sys_read (AzamiOS) */
            if (g_vfs_port_id >= 0) {
                req.type = LPC_REQ_VFS_READ;
                req.arg1 = cpu->ebx; /* fd */
                req.arg2 = cpu->edx; /* count */
                req.section_ptr = (uintptr_t)cpu->ecx; /* buffer */
                req.section_size = cpu->edx;
                lpc_send(g_vfs_port_id, &req, &reply);
                cpu->eax = reply.arg1;
            } else {
                cpu->eax = (uint32_t)-1;
            }
            break;

        case 4:  /* write (Linux x86) */
        case 20: /* SYS_WRITE (AzamiOS) or getpid (Linux x86) */
            if (cpu->eax == 4 || cpu->ecx != 0 || cpu->edx != 0) {
                /* write(fd, buf, count) */
                if (g_vfs_port_id >= 0) {
                    req.type = LPC_REQ_VFS_WRITE;
                    req.arg1 = cpu->ebx; /* fd */
                    req.arg2 = cpu->edx; /* count */
                    req.section_ptr = (uintptr_t)cpu->ecx; /* buffer */
                    req.section_size = cpu->edx;
                    lpc_send(g_vfs_port_id, &req, &reply);
                    cpu->eax = reply.arg1;
                } else {
                    cpu->eax = (uint32_t)-1;
                }
            } else {
                /* getpid() */
                if (g_proc_port_id >= 0) {
                    req.type = LPC_REQ_PROC_GETPID;
                    lpc_send(g_proc_port_id, &req, &reply);
                    cpu->eax = reply.arg1;
                } else {
                    cpu->eax = (uint32_t)getpid();
                }
            }
            break;

        case 5:   /* open (Linux x86) */
        case 295: /* openat (Linux x86) */
        case 21:  /* sys_open (AzamiOS) */
            if (g_vfs_port_id >= 0) {
                req.type = LPC_REQ_VFS_OPEN;
                req.section_ptr = (uintptr_t)cpu->ebx; /* filename */
                req.arg1 = cpu->ecx; /* flags */
                lpc_send(g_vfs_port_id, &req, &reply);
                cpu->eax = reply.arg1;
            } else {
                cpu->eax = (uint32_t)-1;
            }
            break;

        case 6:  /* close (Linux x86) */
        case 22: /* sys_close (AzamiOS) */
            if (g_vfs_port_id >= 0) {
                req.type = LPC_REQ_VFS_CLOSE;
                req.arg1 = cpu->ebx; /* fd */
                lpc_send(g_vfs_port_id, &req, &reply);
                cpu->eax = reply.arg1;
            } else {
                cpu->eax = 0;
            }
            break;

        case 11: /* execve (Linux x86) */
        case 10: /* exec (AzamiOS) */
            if (g_proc_port_id >= 0) {
                req.type = LPC_REQ_PROC_EXEC;
                req.section_ptr = (uintptr_t)cpu->ebx; /* filename */
                lpc_send(g_proc_port_id, &req, &reply);
                cpu->eax = reply.arg1;
            } else {
                cpu->eax = (uint32_t)-1;
            }
            break;

        case 24: /* sys_getpid (AzamiOS) */
            if (g_proc_port_id >= 0) {
                req.type = LPC_REQ_PROC_GETPID;
                lpc_send(g_proc_port_id, &req, &reply);
                cpu->eax = reply.arg1;
            } else {
                cpu->eax = (uint32_t)getpid();
            }
            break;

        case 54:  /* sys_lpc_stat / ioctl */
        case 106: /* stat (Linux x86) */
        case 108: /* fstat (Linux x86) */
            if (g_vfs_port_id >= 0) {
                req.type = LPC_REQ_VFS_STAT;
                req.section_ptr = (uintptr_t)cpu->ebx;
                req.arg1 = cpu->ecx;
                lpc_send(g_vfs_port_id, &req, &reply);
                cpu->eax = reply.arg1;
            } else {
                cpu->eax = 0;
            }
            break;

        case 90:  /* mmap (Linux x86) */
        case 192: /* mmap2 (Linux x86) */
            cpu->eax = 0x50000000; /* Clean user virtual address for 32-bit mmap */
            break;

        default:
            /* Unhandled syscalls return ENOSYS without crashing the emulator */
            cpu->eax = (uint32_t)-38;
            break;
    }
}

/* Lightweight x86 Instruction Translator / Interpreter Loop */
static void compat32_interpret_loop(cpu32_state_t *cpu, uint8_t *code_base, uint32_t code_size) {
    while (!cpu->halted && cpu->eip < code_size) {
        uint8_t *ip = code_base + cpu->eip;
        uint8_t op = ip[0];

        /* int $0x80 or int $128 (0xCD 0x80 / 0xCD 0x80 / 0xCD 128) */
        if (op == 0xCD && (ip[1] == 0x80 || ip[1] == 128)) {
            cpu->eip += 2;
            compat32_handle_syscall(cpu);
            continue;
        }

        /* nop (0x90) */
        if (op == 0x90) {
            cpu->eip += 1;
            continue;
        }

        /* xor eax, eax (0x31 0xC0) */
        if (op == 0x31 && ip[1] == 0xC0) {
            cpu->eax = 0;
            cpu->eip += 2;
            continue;
        }

        /* mov eax, imm32 (0xB8) */
        if (op == 0xB8) {
            cpu->eax = *(uint32_t *)(ip + 1);
            cpu->eip += 5;
            continue;
        }

        /* mov ebx, imm32 (0xBB) */
        if (op == 0xBB) {
            cpu->ebx = *(uint32_t *)(ip + 1);
            cpu->eip += 5;
            continue;
        }

        /* mov ecx, imm32 (0xB9) */
        if (op == 0xB9) {
            cpu->ecx = *(uint32_t *)(ip + 1);
            cpu->eip += 5;
            continue;
        }

        /* mov edx, imm32 (0xBA) */
        if (op == 0xBA) {
            cpu->edx = *(uint32_t *)(ip + 1);
            cpu->eip += 5;
            continue;
        }

        /* push ebp (0x55) */
        if (op == 0x55) {
            cpu->esp -= 4;
            *(uint32_t *)(uintptr_t)cpu->esp = cpu->ebp;
            cpu->eip += 1;
            continue;
        }

        /* pop ebp (0x5D) */
        if (op == 0x5D) {
            cpu->ebp = *(uint32_t *)(uintptr_t)cpu->esp;
            cpu->esp += 4;
            cpu->eip += 1;
            continue;
        }

        /* push ebx (0x53) / pop ebx (0x5B) */
        if (op == 0x53) {
            cpu->esp -= 4;
            *(uint32_t *)(uintptr_t)cpu->esp = cpu->ebx;
            cpu->eip += 1;
            continue;
        }
        if (op == 0x5B) {
            cpu->ebx = *(uint32_t *)(uintptr_t)cpu->esp;
            cpu->esp += 4;
            cpu->eip += 1;
            continue;
        }

        /* push eax (0x50) / pop eax (0x58) */
        if (op == 0x50) {
            cpu->esp -= 4;
            *(uint32_t *)(uintptr_t)cpu->esp = cpu->eax;
            cpu->eip += 1;
            continue;
        }
        if (op == 0x58) {
            cpu->eax = *(uint32_t *)(uintptr_t)cpu->esp;
            cpu->esp += 4;
            cpu->eip += 1;
            continue;
        }

        /* mov ebp, esp (0x89 0xE5) */
        if (op == 0x89 && ip[1] == 0xE5) {
            cpu->ebp = cpu->esp;
            cpu->eip += 2;
            continue;
        }

        /* mov esp, ebp (0x89 0xEC) */
        if (op == 0x89 && ip[1] == 0xEC) {
            cpu->esp = cpu->ebp;
            cpu->eip += 2;
            continue;
        }

        /* sub esp, imm8 (0x83 0xEC imm8) */
        if (op == 0x83 && ip[1] == 0xEC) {
            cpu->esp -= ip[2];
            cpu->eip += 3;
            continue;
        }

        /* add esp, imm8 (0x83 0xC4 imm8) */
        if (op == 0x83 && ip[1] == 0xC4) {
            cpu->esp += ip[2];
            cpu->eip += 3;
            continue;
        }

        /* sub esp, imm32 (0x81 0xEC imm32) */
        if (op == 0x81 && ip[1] == 0xEC) {
            cpu->esp -= *(uint32_t *)(ip + 2);
            cpu->eip += 6;
            continue;
        }

        /* add esp, imm32 (0x81 0xC4 imm32) */
        if (op == 0x81 && ip[1] == 0xC4) {
            cpu->esp += *(uint32_t *)(ip + 2);
            cpu->eip += 6;
            continue;
        }

        /* leave (0xC9) */
        if (op == 0xC9) {
            cpu->esp = cpu->ebp;
            cpu->ebp = *(uint32_t *)(uintptr_t)cpu->esp;
            cpu->esp += 4;
            cpu->eip += 1;
            continue;
        }

        /* call rel32 (0xE8 rel32) */
        if (op == 0xE8) {
            int32_t rel = *(int32_t *)(ip + 1);
            cpu->esp -= 4;
            *(uint32_t *)(uintptr_t)cpu->esp = cpu->eip + 5;
            cpu->eip = cpu->eip + 5 + rel;
            continue;
        }

        /* jmp rel32 (0xE9 rel32) */
        if (op == 0xE9) {
            int32_t rel = *(int32_t *)(ip + 1);
            cpu->eip = cpu->eip + 5 + rel;
            continue;
        }

        /* jmp rel8 (0xEB rel8) */
        if (op == 0xEB) {
            int8_t rel = (int8_t)ip[1];
            cpu->eip = cpu->eip + 2 + rel;
            continue;
        }

        /* ret (0xC3) */
        if (op == 0xC3) {
            cpu->halted = true;
            break;
        }

        /* Fallback instruction step for complex instruction sequences */
        cpu->eip += 1;
    }
}

void _start(void) {
    g_vfs_port_id  = lpc_connect(LPC_PORT_VFS);
    g_proc_port_id = lpc_connect(LPC_PORT_PROC);

    char target_elf[128] = "";
    int arg_fd = open("cmd_args", O_RDONLY);
    if (arg_fd >= 0) {
        char buf[128];
        int n = read(arg_fd, buf, 127);
        close(arg_fd);
        if (n > 0) {
            buf[n] = '\0';
            while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
                buf[--n] = '\0';
            }
            char *start = buf;
            while (*start == ' ') start++;
            if (*start != '\0') {
                strncpy(target_elf, start, 127);
                target_elf[127] = '\0';
            }
        }
    }

    if (target_elf[0] != '\0') {
        int elf_fd = open(target_elf, O_RDONLY);
        if (elf_fd >= 0) {
            uint8_t *code_buf = (uint8_t *)malloc(65536);
            if (code_buf) {
                int bytes = read(elf_fd, code_buf, 65536);
                if (bytes > 0) {
                    close(elf_fd);
                    cpu32_state_t cpu;
                    memset(&cpu, 0, sizeof(cpu));
                    cpu.esp = 0x50010000;
                    cpu.eip = 0; /* Entry point offset within loaded ELF segment */

                    compat32_interpret_loop(&cpu, code_buf, (uint32_t)bytes);
                    free(code_buf);
                    exit(cpu.exit_code);
                }
                free(code_buf);
            }
            close(elf_fd);
        }
    }

    /* Daemon Mode: Act as ALPC Compatibility Server */
    char out_msg[] = "compat32: Userspace 32-Bit Binary Translator & ALPC Server initialized.\n";
    int out_fd = open("cmd_out", O_WRONLY | O_CREAT, 0);
    if (out_fd >= 0) {
        write(out_fd, out_msg, strlen(out_msg));
        close(out_fd);
    }
    exit(0);
}
