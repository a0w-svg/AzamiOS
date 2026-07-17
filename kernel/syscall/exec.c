/**
 * exec.c  –  program execution dispatcher for AzamiOS
 *
 * Identifies the binary format of a file, loads it via load_elf32(), maps a
 * user-mode stack, and transfers control to the program via enter_usermode().
 */#include "include/exec.h"
#include "include/elf_loader.h"
#include "../filesystem/include/vfs.h"
#include "../mem/include/pmm.h"
#include "../mem/include/paging.h"
#include "../klibc/include/string.h"
#include "../klibc/include/stdio.h"
#include "../klibc/include/port.h"
#include "../arch/include/idt.h"
#include "../arch/include/gdt.h"
#include "../proc/include/process.h"
#include "../proc/include/scheduler.h"
#include "../proc/include/lpc.h"
#include "../proc/include/exec_server.h"

/* First four bytes of an ELF file: 0x7f 'E' 'L' 'F' */
#define ELF_MAGIC 0x464C457FU

/*
 * User stack layout:
 *   Virtual top  = USER_STACK_TOP   (exclusive, first address above the stack)
 *   Virtual base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE
 *
 * We push esp = USER_STACK_TOP - 4 so the stack pointer is valid on entry.
 */
#define USER_STACK_TOP    ((uintptr_t)0xC0000000ULL) /* Top of user stack */
#define USER_STACK_PAGES  128                        /* 128 × 4 KB = 512 KB user stack */

/* Defined in cpu.asm */
extern void enter_userspace(uintptr_t user_entry, uintptr_t user_stack_top);

char g_return_program[64] = "wm";
char g_current_program[64] = "wm";

void execute_program(char *filename) {
    kprintf("exec: executing '%s'...\n", filename);
    if (strcmp(filename, "wm") == 0 || strcmp(filename, "shell") == 0) {
        strncpy(g_return_program, filename, 63);
        g_return_program[63] = '\0';
    }
    strncpy(g_current_program, filename, 63);
    g_current_program[63] = '\0';

    /* ── 1. Locate file in VFS ───────────────────────────────────────── */
    kprintf("exec: step 1 finding dir, fs_root=0x%x...\n", (uint32_t)(uintptr_t)fs_root);
    if (fs_root) {
        kprintf("exec: fs_root flags=0x%x finddir=0x%x\n", fs_root->flags, (uint32_t)(uintptr_t)fs_root->finddir);
    }
    fs_node_t *file_node = fs_root ? finddir_fs(fs_root, filename) : 0;
    kprintf("exec: found node=0x%x\n", (uint32_t)(uintptr_t)file_node);

    if (file_node == 0 || file_node->flags != FS_FILE) {
        kprintf("exec: file not found: '%s'\n", filename);
        return;
    }

    /* ── 2. Read first four bytes to detect format ───────────────────── */
    block_device_t dummy = {0};
    uint32_t magic      = 0;
    uint32_t bytes_read = file_node->read(&dummy, file_node, 0,
                                          sizeof(uint32_t),
                                          (uint8_t *)&magic);

    kprintf("exec: read magic=0x%x\n", magic);

    if (bytes_read < sizeof(uint32_t)) {
        kprintf("exec: '%s' is too short\n", filename);
        return;
    }

    /* ── 3. Dispatch ────────────────────────────────────────────────────*/
    if (magic != ELF_MAGIC) {
        kprintf("exec: unknown format for '%s' (magic=0x%x)\n", filename, magic);
        return;
    }

    /* ── 4. Load ELF segments into virtual memory ───────────────────── */
    uintptr_t entry = 0;
    kprintf("exec: loading elf...\n");
    int rc = load_elf(file_node, &entry);
    kprintf("exec: load_elf rc=%d entry=0x%llx\n", rc, (unsigned long long)entry);

    if (rc != ELF_LOAD_OK) {
        kprintf("exec: ELF load failed for '%s' (err=%d)\n", filename, rc);
        return;
    }

    /* ── 5. Allocate and map the user-mode stack ────────────────────── */
    uintptr_t stack_virt = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;

    for (uint32_t i = 0; i < USER_STACK_PAGES; i++) {
        void *phys = pmm_alloc_block();
        if (!phys) {
            kprintf("exec: out of memory allocating user stack\n");
            return;
        }
        uintptr_t virt = stack_virt + i * PAGE_SIZE;
        /* user=1, writable=1 */
        paging_map_page((uintptr_t)phys, virt, 0, 1);
        process_t *cur_proc = scheduler_get_current();
        kprintf("exec: pid=%d stack page %d virt 0x%llx -> phys 0x%llx\n", cur_proc ? cur_proc->pid : 0, i, (unsigned long long)virt, (unsigned long long)(uintptr_t)phys);
        memset((void *)virt, 0, PAGE_SIZE);
    }

    kprintf("exec: user stack mapped at 0x%llx–0x%llx, entering ring 3\n",
            (unsigned long long)stack_virt, (unsigned long long)USER_STACK_TOP);

    /* Flush entire hardware TLB before jumping to new process */
    uintptr_t cr3;
    asm volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");

    kprintf("exec: entering ring 3 at entry=0x%llx\n", (unsigned long long)entry);

    process_t *cur = scheduler_get_current();
    if (!cur) {
        kprintf("exec: registering initial process '%s' in scheduler\n", filename);
        cur = process_create(filename, entry, cr3);
    } else {
        strncpy(cur->name, filename, 31);
        cur->entry = entry;
    }

    if (cur && cur->kernel_stack) {
        set_kernel_stack(cur->kernel_stack + 4096);
    }

    /* ── 6. Enter ring-3 ─────────────────────────────────────────────── */
    enter_userspace(entry, USER_STACK_TOP - sizeof(uintptr_t));
}