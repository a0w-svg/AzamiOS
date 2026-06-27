/**
 * exec.c  –  program execution dispatcher for AzamiOS
 *
 * Identifies the binary format of a file, loads it via load_elf32(), maps a
 * user-mode stack, and transfers control to the program via enter_usermode().
 */

#include "include/exec.h"
#include "include/elf_loader.h"
#include "../filesystem/include/vfs.h"
#include "../mem/include/pmm.h"
#include "../mem/include/paging.h"
#include "../klibc/include/string.h"
#include "../klibc/include/stdio.h"

/* First four bytes of an ELF file: 0x7f 'E' 'L' 'F' */
#define ELF_MAGIC 0x464C457FU

/*
 * User stack layout:
 *   Virtual top  = USER_STACK_TOP   (exclusive, first address above the stack)
 *   Virtual base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE
 *
 * We push esp = USER_STACK_TOP - 4 so the stack pointer is valid on entry.
 */
#define USER_STACK_TOP    0xC0000000u   /* 3 GB mark – top of user address space */
#define USER_STACK_PAGES  128           /* 128 × 4 KB = 512 KB user stack */

/* Defined in cpu.asm */
extern void enter_usermode(uint32_t user_entry, uint32_t user_stack_top);

char g_return_program[64] = "wm";
char g_current_program[64] = "wm";

void execute_program(char *filename) {
    if (strcmp(filename, "wm") == 0 || strcmp(filename, "shell") == 0) {
        strncpy(g_return_program, filename, 63);
        g_return_program[63] = '\0';
    }
    strncpy(g_current_program, filename, 63);
    g_current_program[63] = '\0';

    /* ── 1. Locate file in VFS ───────────────────────────────────────── */
    fs_node_t *file_node = fs_root->finddir(fs_root, filename);

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
    uint32_t entry = 0;
    int rc = load_elf32(file_node, &entry);

    if (rc != ELF_LOAD_OK) {
        kprintf("exec: ELF load failed for '%s' (err=%d)\n", filename, rc);
        return;
    }

    /* ── 5. Allocate and map the user-mode stack ────────────────────── */
    uint32_t stack_virt = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;

    for (uint32_t i = 0; i < USER_STACK_PAGES; i++) {
        void *phys = pmm_alloc_block();
        if (!phys) {
            kprintf("exec: out of memory allocating user stack\n");
            return;
        }
        uint32_t virt = stack_virt + i * PAGE_SIZE;
        /* user=1, writable=1 */
        paging_map_page((uint32_t)phys, virt, 0, 1);
        memset((void *)virt, 0, PAGE_SIZE);
    }

    kprintf("exec: user stack mapped at 0x%x–0x%x, entering ring 3\n",
            stack_virt, USER_STACK_TOP);

    /* Flush entire hardware TLB before jumping to new process */
    uint32_t cr3;
    asm volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");

    /* ── 6. Enter ring-3 ─────────────────────────────────────────────── */
    enter_usermode(entry, USER_STACK_TOP - 4);
}