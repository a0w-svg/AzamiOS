/* ============================================================================
 * AzamiOS — 64-bit ELF Executable Loader & User Process Spawner
 * File: kernel/sched/elf.c (Linux & System V AMD64 ABI Compliant)
 * ============================================================================ */

#define DEBUG 0
#include <azami/debug.h>
#include "elf.h"
#include "../../fs/vfs.h"
#include "../mm/kmalloc.h"
#include "../mm/pmm.h"
#include "../mm/vma.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/msr.h"   /* cpuid() — BUG-AB fix */
#include "../../drivers/char/console.h"
#include "../../include/azami/defs.h"
#include "../../kernel/lib/string.h"
#include "../../kernel/lib/random.h"
#include "../../arch/x86_64/cpu/hwaccel.h"
#include "../../arch/x86_64/vdso/vdso.h"

#define USER_STACK_TOP      0x00007fffffffe000ULL
#define USER_STACK_PAGES    64
#define USER_STACK_BASE     (USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE))
/* How far below the stack top the stack VMA reaches — the region the fault
 * handler will grow on demand. Matches the 4 MB window execve() used to
 * hard-code. */
#define USER_STACK_MAX_BYTES 0x800000ULL
extern u8 g_fsgsbase_enabled;

#define DYN_LOAD_BASE       0x0000555555554000ULL
#define INTERP_LOAD_BASE    0x00007ffff7dd5000ULL
#define MMAP_ARENA_START    0x0000600000000000ULL
#define MAX_RECURSION_DEPTH 4

/* ── ASLR ────────────────────────────────────────────────────────────────────
 * Entropy budgets, in pages, for each randomised region. These are the classic
 * x86-64 Linux magnitudes: enough that guessing a mapping is impractical, small
 * enough that no region can collide with its neighbours. Every offset is page
 * aligned, which the ELF and stack setup paths both require.
 *
 *   exec  28 bits of page entropy → 1 TB window for the PIE image
 *   mmap  28 bits                 → 1 TB window for the mmap arena
 *   interp/stack are deliberately smaller: both live in the top-of-userspace
 *   band that the demand-paging heuristic in isr_dispatch() recognises, so
 *   they must stay inside it.
 * -------------------------------------------------------------------------- */
#define ASLR_EXEC_RND_PAGES    (1U << 18)   /* 1 GB of slide for the image  */
#define ASLR_MMAP_RND_PAGES    (1U << 18)   /* 1 GB of slide for mmap       */
#define ASLR_INTERP_RND_PAGES  (1U << 14)   /* 64 MB for the dynamic linker */
#define ASLR_STACK_RND_PAGES   (1U << 11)   /* 8 MB for the initial stack   */

#define ADDR_NO_RANDOMIZE      0x0040000U   /* personality(2) bit            */

/* Page-aligned random offset in [0, max_pages) pages. */
static u64 aslr_offset(process_t *proc, u32 max_pages)
{
    if (proc && (proc->personality & ADDR_NO_RANDOMIZE)) return 0;
    if (max_pages == 0) return 0;
    return (u64)(krandom_u64() % max_pages) * PAGE_SIZE;
}

/* AT_RANDOM entropy comes straight from the kernel CSPRNG. */
static void get_random_bytes(void *buf, size_t n)
{
    krandom_bytes(buf, n);
}

static int setup_user_stack(process_t *proc, vmm_space_t user_space, phys_addr_t top_page_phys,
                            const char *exec_path, const char *const argv[], const char *const envp[],
                            u64 main_entry, u64 phdr_vaddr, u64 phnum, u64 phent, u64 interp_base,
                            u64 stack_top, u64 vdso_base, u64 *out_rsp)
{
    (void)user_space;
    char *kstack = (char *)PHYS_TO_VIRT(top_page_phys);
    size_t top_offset = PAGE_SIZE; /* Top of stack page */
    int rc = -ENOMEM;

    /* Pointer arrays live on the heap, not the 16 KB kernel stack. */
    u64 *u_argv = NULL;
    u64 *u_envp = NULL;

    /* Guard: bail out (unwinding to the caller, which tears down the address
     * space) rather than letting an unsigned `top_offset` wrap and scribble
     * over adjacent HHDM memory. */
    #define STK_NEED(n) do { if (top_offset < (size_t)(n)) { rc = -E2BIG; goto out; } } while (0)
    #define PUSHQ(v)    do { STK_NEED(8); top_offset -= 8; \
                             *(u64 *)(kstack + top_offset) = (u64)(v); } while (0)

    /* Number of auxv (key,value) pairs written below — keep in sync. */
    #define AUX_PAIRS 20

    /* Determine argc / envc up front so the string loops can reserve the exact
     * space the fixed structure (argc + pointer arrays + auxv) will need. */
    int envc = 0;
    if (envp) { while (envp[envc] && envc < 512) envc++; }
    int argc = 0;
    if (argv) { while (argv[argc] && argc < 512) argc++; }
    if (argc == 0) argc = 1;

    /* argc(1) + argv[argc] + NULL(1) + envp[envc] + NULL(1) + auxv(2*AUX_PAIRS)
     * + up to one 8-byte alignment pad. */
    size_t fixed_bytes = 8u * (size_t)(1 + argc + 1 + envc + 1 + 2 * AUX_PAIRS + 1);

    u_argv = (u64 *)kmalloc((size_t)argc * sizeof(u64));
    u_envp = (u64 *)kmalloc((size_t)(envc ? envc : 1) * sizeof(u64));
    if (!u_argv || !u_envp) goto out;

    /* 1. AT_RANDOM 16-byte entropy block */
    STK_NEED(16 + fixed_bytes);
    top_offset -= 16;
    get_random_bytes(kstack + top_offset, 16);
    u64 u_random_ptr = stack_top - (PAGE_SIZE - top_offset);

    /* 2. AT_PLATFORM string ("x86_64") */
    const char *platform_str = "x86_64";
    size_t plat_len = strlen(platform_str) + 1;
    STK_NEED(plat_len + fixed_bytes);
    top_offset -= plat_len;
    memcpy(kstack + top_offset, platform_str, plat_len);
    u64 u_platform_ptr = stack_top - (PAGE_SIZE - top_offset);

    /* 3. AT_EXECFN string (path to executable) */
    const char *execfn_src = exec_path ? exec_path : "/bin/sh.elf";
    size_t execfn_len = strlen(execfn_src) + 1;
    STK_NEED(execfn_len + fixed_bytes);
    top_offset -= execfn_len;
    memcpy(kstack + top_offset, execfn_src, execfn_len);
    u64 u_execfn_ptr = stack_top - (PAGE_SIZE - top_offset);

    /* 4. envp strings */
    for (int i = 0; i < envc; i++) {
        const char *src = envp[i] ? envp[i] : "";
        size_t len = strlen(src) + 1;
        STK_NEED(len + fixed_bytes);
        top_offset -= len;
        memcpy(kstack + top_offset, src, len);
        u_envp[i] = stack_top - (PAGE_SIZE - top_offset);
    }

    /* 5. argv strings */
    for (int i = 0; i < argc; i++) {
        const char *src = (argv && argv[i]) ? argv[i] : (exec_path ? exec_path : "");
        size_t len = strlen(src) + 1;
        STK_NEED(len + fixed_bytes);
        top_offset -= len;
        memcpy(kstack + top_offset, src, len);
        u_argv[i] = stack_top - (PAGE_SIZE - top_offset);
    }

    /* Align string area down to 8 bytes */
    top_offset &= ~7ULL;

    size_t num_slots = (size_t)(2 * AUX_PAIRS) + (envc + 1) + (argc + 1) + 1;

    /* Ensure 16-byte System V AMD64 ABI stack alignment at the entry point */
    if (((top_offset - (num_slots * 8)) & 0xF) != 0) {
        PUSHQ(0);
    }

    /* ── Auxiliary Vector Table (Standard Linux AMD64 auxv, written high→low) ─ */
    PUSHQ(0);                               PUSHQ(AT_NULL);
    /* Without a vDSO the pair is AT_IGNORE, keeping AUX_PAIRS exact. */
    PUSHQ(vdso_base);                       PUSHQ(vdso_base ? AT_SYSINFO_EHDR : AT_IGNORE);
    PUSHQ(u_platform_ptr);                  PUSHQ(AT_PLATFORM);
    PUSHQ(u_execfn_ptr);                    PUSHQ(AT_EXECFN);
    PUSHQ(u_random_ptr);                    PUSHQ(AT_RANDOM);
    PUSHQ(0);                               PUSHQ(AT_SECURE);
    PUSHQ(100);                             PUSHQ(AT_CLKTCK);
    /* Populate AT_HWCAP from CPUID(1).EDX and AT_HWCAP2 from CPUID(7).EBX */
    u32 cpuid_eax, cpuid_ebx, cpuid_ecx, cpuid_edx;
    cpuid(1, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    PUSHQ((u64)cpuid_edx);              PUSHQ(AT_HWCAP);
    u64 hwcap2 = 0;
    if (g_fsgsbase_enabled) hwcap2 |= (1ULL << 1); /* HWCAP2_FSGSBASE */
    PUSHQ(hwcap2);                      PUSHQ(AT_HWCAP2);
    PUSHQ(proc ? (u64)proc->egid : 0);      PUSHQ(AT_EGID);
    PUSHQ(proc ? (u64)proc->gid  : 0);      PUSHQ(AT_GID);
    PUSHQ(proc ? (u64)proc->euid : 0);      PUSHQ(AT_EUID);
    PUSHQ(proc ? (u64)proc->uid  : 0);      PUSHQ(AT_UID);
    PUSHQ(main_entry);                      PUSHQ(AT_ENTRY);
    PUSHQ(0);                               PUSHQ(AT_FLAGS);
    PUSHQ(interp_base);                     PUSHQ(AT_BASE);
    PUSHQ(PAGE_SIZE);                       PUSHQ(AT_PAGESZ);
    PUSHQ(phnum);                           PUSHQ(AT_PHNUM);
    PUSHQ(phent);                           PUSHQ(AT_PHENT);
    PUSHQ(phdr_vaddr);                      PUSHQ(AT_PHDR);

    /* Envp NULL terminator and pointers */
    PUSHQ(0);
    for (int i = envc - 1; i >= 0; i--) PUSHQ(u_envp[i]);

    /* Argv NULL terminator and pointers */
    PUSHQ(0);
    for (int i = argc - 1; i >= 0; i--) PUSHQ(u_argv[i]);

    /* Argc */
    PUSHQ((u64)argc);

    *out_rsp = stack_top - (PAGE_SIZE - top_offset);
    rc = 0;

out:
    kfree(u_argv);
    kfree(u_envp);
    #undef STK_NEED
    #undef PUSHQ
    #undef AUX_PAIRS
    return rc;
}

/* Helper to map and load ELF PT_LOAD segments into a virtual address space.
 * Reads all program headers in a single batched vfs_read rather than issuing
 * individual seeks and reads per phdr, eliminating dozens of redundant VFS
 * round-trips per execve. */
static int load_elf_segments(process_t *proc, vmm_space_t user_space, file_t *file, const elf64_ehdr_t *ehdr,
                             u64 load_bias, u64 *out_phdr_vaddr, u64 *out_max_vaddr,
                             u64 *out_interp_offset, u64 *out_interp_filesz, bool *out_exec_stack)
{
    u64 phdr_user_vaddr = 0;
    u64 max_vaddr = 0;
    /* Fallback for images with no PT_PHDR: the program header table is at file
     * offset e_phoff, so it is visible to the process only if some PT_LOAD
     * happens to cover that offset. Remember where it lands. */
    u64 phdr_from_load = 0;

    if (out_interp_offset) *out_interp_offset = 0;
    if (out_interp_filesz) *out_interp_filesz = 0;
    if (out_exec_stack) *out_exec_stack = false;

    if (ehdr->e_phentsize != sizeof(elf64_phdr_t) ||
        ehdr->e_phnum == 0 || ehdr->e_phnum > 256 ||
        ehdr->e_phoff > (u64)file->f_inode->i_size ||
        (u64)ehdr->e_phnum * sizeof(elf64_phdr_t) > (u64)file->f_inode->i_size - ehdr->e_phoff) {
        return -ENOEXEC;
    }

    size_t phdr_table_size = (size_t)ehdr->e_phnum * sizeof(elf64_phdr_t);
    elf64_phdr_t phdr_stack[32];
    elf64_phdr_t *phdrs = phdr_stack;
    bool heap_alloc = false;
    if (ehdr->e_phnum > 32) {
        phdrs = (elf64_phdr_t *)kmalloc(phdr_table_size);
        if (!phdrs) return -ENOMEM;
        heap_alloc = true;
    }

    file->f_pos = ehdr->e_phoff;
    s64 nread_hdrs = vfs_read(file, phdrs, phdr_table_size);
    if (nread_hdrs < (s64)phdr_table_size) {
        if (heap_alloc) kfree(phdrs);
        return nread_hdrs < 0 ? (int)nread_hdrs : -ENOEXEC;
    }

    int ret = 0;
    /* The detached registry already contains the main executable when loading
     * an interpreter. Reject collisions before either image can overwrite the
     * other's pages. Overlapping PT_LOAD pages within one image remain valid. */
    for (u16 i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD || !ph->p_memsz) continue;
        u64 start = load_bias + ph->p_vaddr;
        if (start < load_bias || start >= TASK_SIZE_MAX ||
            ph->p_memsz > TASK_SIZE_MAX - start) { ret = -EINVAL; goto cleanup; }
        u64 end = ALIGN_UP(start + ph->p_memsz, PAGE_SIZE);
        start = ALIGN_DOWN(start, PAGE_SIZE);
        for (vm_area_t *v = proc->vma_list; v; v = v->next) {
            if (start < v->end && end > v->start) { ret = -EINVAL; goto cleanup; }
        }
    }
    for (u16 i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t *phdr = &phdrs[i];

        if (phdr->p_type == PT_INTERP && out_interp_filesz && !*out_interp_filesz) {
            if (phdr->p_filesz < 2 || phdr->p_filesz > 4096) {
                ret = -ENOEXEC;
                goto cleanup;
            }
            if (out_interp_offset) *out_interp_offset = phdr->p_offset;
            if (out_interp_filesz) *out_interp_filesz = phdr->p_filesz;
        }

        if (phdr->p_type == PT_GNU_STACK) {
            if (out_exec_stack && (phdr->p_flags & PF_X)) {
                *out_exec_stack = true;
            }
        }

        if (phdr->p_type == PT_PHDR) {
            phdr_user_vaddr = load_bias + phdr->p_vaddr;
        }

        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0) continue;

        /* Does this segment's file image contain the program header table? If
         * so its run-time address is the segment's, shifted by the same amount
         * e_phoff sits past p_offset. Linking without PT_INTERP (a plain static
         * binary — musl's, for one) leaves out PT_PHDR entirely, and taking
         * `load_bias + e_phoff` as the answer hands userspace a near-zero
         * address: musl's __init_tls walks it and faults before main(). */
        if (phdr_from_load == 0 &&
            ehdr->e_phoff >= phdr->p_offset &&
            ehdr->e_phoff + phdr_table_size <= phdr->p_offset + phdr->p_filesz) {
            phdr_from_load = load_bias + phdr->p_vaddr + (ehdr->e_phoff - phdr->p_offset);
        }

        /* A segment whose file image is larger than its memory image is
         * malformed — the streaming loop below would run off the mapped range. */
        if (phdr->p_filesz > phdr->p_memsz) {
            ret = -ENOEXEC;
            goto cleanup;
        }

        u64 seg_vaddr = load_bias + phdr->p_vaddr;

        /* Check additions before alignment and signed VFS offsets. */
        if (seg_vaddr < load_bias || seg_vaddr >= TASK_SIZE_MAX ||
            phdr->p_memsz > TASK_SIZE_MAX - seg_vaddr ||
            phdr->p_offset > 0x7fffffffffffffffULL ||
            phdr->p_filesz > 0x7fffffffffffffffULL - phdr->p_offset) {
            ret = -EINVAL;
            goto cleanup;
        }

        if (seg_vaddr + phdr->p_memsz > max_vaddr) {
            max_vaddr = seg_vaddr + phdr->p_memsz;
        }

        u64 start_vaddr = ALIGN_DOWN(seg_vaddr, PAGE_SIZE);
        u64 end_vaddr = ALIGN_UP(seg_vaddr + phdr->p_memsz, PAGE_SIZE);

        /* Canonical boundary enforcement */
        if (end_vaddr >= TASK_SIZE_MAX) {
            ret = -EINVAL;
            goto cleanup;
        }

        u64 vmm_flags = VMM_F_PRESENT | VMM_F_USER;
        if (phdr->p_flags & PF_W) vmm_flags |= VMM_F_WRITE;
        if (!(phdr->p_flags & PF_X)) vmm_flags |= VMM_F_NX;

        if (proc && end_vaddr > start_vaddr) {
            u32 vma_prot = 0;
            if (phdr->p_flags & PF_R) vma_prot |= VMA_PROT_READ;
            if (phdr->p_flags & PF_W) vma_prot |= VMA_PROT_WRITE;
            if (phdr->p_flags & PF_X) vma_prot |= VMA_PROT_EXEC;
            if (vma_add(proc, start_vaddr, end_vaddr, vma_prot, VMA_F_FILE) < 0) {
                ret = -ENOMEM;
                goto cleanup;
            }
        }

        /* Single pass: allocate, stream file data directly into page, zero padding/bss, and map */
        for (u64 vaddr = start_vaddr; vaddr < end_vaddr; vaddr += PAGE_SIZE) {
            phys_addr_t phys = vmm_translate(user_space, vaddr);
            bool already_mapped = (phys != 0);
            void *page_buf = NULL;

            if (already_mapped) {
                phys = phys & VMM_PHYS_MASK;
                u64 old_flags = vmm_query_flags(user_space, vaddr);
                u64 merged = VMM_F_PRESENT | VMM_F_USER;
                if ((old_flags & VMM_F_WRITE) || (vmm_flags & VMM_F_WRITE))
                    merged |= VMM_F_WRITE;
                if ((old_flags & VMM_F_NX) && (vmm_flags & VMM_F_NX))
                    merged |= VMM_F_NX;
                if (vmm_map(user_space, vaddr, phys, merged) != 0) {
                    ret = -ENOMEM;
                    goto cleanup;
                }
                page_buf = (void *)PHYS_TO_VIRT(phys);
            } else {
                phys = pmm_alloc_page();
                if (!phys) {
                    ret = -ENOMEM;
                    goto cleanup;
                }
                page_buf = (void *)PHYS_TO_VIRT(phys);
                if (vmm_map(user_space, vaddr, phys, vmm_flags) != 0) {
                    pmm_free_page(phys);
                    ret = -ENOMEM;
                    goto cleanup;
                }
            }

            /* Overlap calculation between this page [vaddr, page_end) and file data [seg_vaddr, file_data_end) */
            u64 page_end = vaddr + PAGE_SIZE;
            u64 file_data_end = seg_vaddr + phdr->p_filesz;
            u64 file_start = (vaddr < seg_vaddr) ? seg_vaddr : vaddr;
            u64 file_end   = (page_end < file_data_end) ? page_end : file_data_end;

            if (file_end > file_start) {
                u64 page_off = file_start - vaddr;
                size_t read_bytes = (size_t)(file_end - file_start);

                if (!already_mapped && page_off > 0) {
                    __builtin_memset(page_buf, 0, (size_t)page_off);
                }

                file->f_pos = phdr->p_offset + (file_start - seg_vaddr);
                s64 nread = vfs_read(file, (char *)page_buf + page_off, read_bytes);
                if (nread < (s64)read_bytes) {
                    pr_debug("[ELF] ERROR: Short read on segment %u at vaddr 0x%llx\n",
                             (unsigned int)i, (unsigned long long)vaddr);
                    ret = nread < 0 ? (int)nread : -EIO;
                    goto cleanup;
                }

                if (page_off + read_bytes < PAGE_SIZE) {
                    __builtin_memset((char *)page_buf + page_off + read_bytes, 0,
                                     PAGE_SIZE - (page_off + read_bytes));
                }
            } else {
                /* Pure BSS page */
                hw_clear_page(page_buf);
            }
        }
    }

    if (phdr_user_vaddr == 0) {
        phdr_user_vaddr = phdr_from_load;
    }
    if (out_phdr_vaddr) *out_phdr_vaddr = phdr_user_vaddr;
    if (out_max_vaddr) *out_max_vaddr = max_vaddr;
    ret = 0;

cleanup:
    if (heap_alloc) kfree(phdrs);
    return ret;
}


static bool elf_header_valid(const elf64_ehdr_t *eh)
{
    return eh->e_ident_magic == ELF_MAGIC && eh->e_ident_class == ELFCLASS64 &&
           eh->e_ident_data == ELFDATA2LSB && eh->e_machine == EM_X86_64 &&
           (eh->e_type == ET_EXEC || eh->e_type == ET_DYN);
}

/* Interpreter names belong to the caller's root/cwd. The VFS walks raw
 * components, including symlinks and .., relative to that root. */
static file_t *elf_open_interpreter(process_t *proc, const char *path, s64 *err)
{
    char cwd[512];
    int n = snprintf(cwd, sizeof(cwd), "%s%s", proc->root[0] ? proc->root : "/",
                     proc->cwd[0] == '/' ? proc->cwd + 1 : proc->cwd);
    /* root need not have a trailing slash. */
    if (proc->root[0] && strcmp(proc->root, "/"))
        n = snprintf(cwd, sizeof(cwd), "%s/%s", proc->root,
                     proc->cwd[0] == '/' ? proc->cwd + 1 : proc->cwd);
    if (n < 0 || (size_t)n >= sizeof(cwd)) { *err = -ENAMETOOLONG; return NULL; }
    return vfs_openat_resolved(NULL, cwd, proc->root[0] ? proc->root : "/",
                               path, 0, 0, 0, err);
}

/* Recursive ELF / Script loader */
static int elf_load_exec_internal(process_t *proc, const char *path, const char *const argv[], const char *const envp[],
                                 uintptr_t *out_entry, phys_addr_t *out_space, u64 *out_rsp, int recursion_depth)
{
    if (recursion_depth > MAX_RECURSION_DEPTH) return -ELOOP;
    if (!proc || !path || !out_entry || !out_space || !out_rsp) return -EINVAL;

    pr_debug("[ELF] Loading binary or script '%s' (depth %d)\n", path, recursion_depth);
    s64 open_err = -ENOENT;
    file_t *file = recursion_depth ? elf_open_interpreter(proc, path, &open_err)
                                  : vfs_open_err(path, 0, 0, &open_err);
    if (!file) return (int)open_err;
    if (!S_ISREG(file->f_inode->i_mode) || !(file->f_inode->i_mode & 0111)) {
        vfs_close(file);
        return -EACCES;
    }

    /* Read header bytes to check for Shebang (#!) or ELF magic */
    char hdr_buf[128];
    file->f_pos = 0;
    s64 nread = vfs_read(file, hdr_buf, sizeof(hdr_buf));
    if (nread < 4) {
        vfs_close(file);
        return nread < 0 ? (int)nread : -ENOEXEC;
    }

    /* ── 1. Linux Shebang / Script Execution (binfmt_script) ──────────────── */
    if (hdr_buf[0] == '#' && hdr_buf[1] == '!') {
        vfs_close(file);

        /* Parse interpreter line (e.g. "#!/bin/sh" or "#!/usr/bin/env sh") */
        char interp_line[128];
        size_t hlen = (size_t)nread;
        size_t idx = 2;
        while (idx < hlen && (hdr_buf[idx] == ' ' || hdr_buf[idx] == '\t')) idx++;

        size_t out_i = 0;
        while (idx < hlen && hdr_buf[idx] != '\n' && hdr_buf[idx] != '\r' && out_i < sizeof(interp_line) - 1) {
            interp_line[out_i++] = hdr_buf[idx++];
        }
        interp_line[out_i] = '\0';
        /* Do not execute a prefix of an interpreter name cut off by the
         * fixed shebang buffer. A complete name needs a delimiter. */
        if (idx == hlen && hlen == sizeof(hdr_buf) &&
            !memchr(interp_line, ' ', out_i) && !memchr(interp_line, '\t', out_i))
            return -ENOEXEC;
        while (out_i && (interp_line[out_i - 1] == ' ' || interp_line[out_i - 1] == '\t'))
            interp_line[--out_i] = '\0';
        if (!out_i) return -ENOEXEC;

        /* Split interpreter and optional argument */
        char *interp = interp_line;
        while (*interp == ' ' || *interp == '\t') interp++;
        char *arg = NULL;
        char *space = interp;
        while (*space && *space != ' ' && *space != '\t') space++;
        if (!*space) space = NULL;
        if (space) {
            *space = '\0';
            arg = space + 1;
            while (*arg == ' ' || *arg == '\t') arg++;
        }

        /* Build new argv: [interp, (arg), path, argv[1], argv[2], ..., NULL] */
        const char *new_argv[64];
        int new_argc = 0;
        new_argv[new_argc++] = interp;
        if (arg && *arg) {
            new_argv[new_argc++] = arg;
        }
        new_argv[new_argc++] = path;

        if (argv) {
            int src_i = 1;
            while (argv[src_i] && new_argc < 62) {
                new_argv[new_argc++] = argv[src_i++];
            }
            if (argv[src_i]) return -E2BIG;
        }
        new_argv[new_argc] = NULL;

        return elf_load_exec_internal(proc, interp, new_argv, envp, out_entry, out_space, out_rsp, recursion_depth + 1);
    }

    /* ── 2. Standard 64-bit ELF Execution ─────────────────────────────────── */
    /* BUG-W fix: check for short read BEFORE copying into ehdr so we never
     * memcpy uninitialized hdr_buf bytes into the header struct.  Use the
     * type name rather than sizeof(ehdr) so the variable can be declared
     * after the guard without triggering a "used before declared" diagnostic. */
    if (nread < (s64)sizeof(elf64_ehdr_t)) {
        vfs_close(file);
        return -ENOEXEC;
    }
    elf64_ehdr_t ehdr;
    memcpy(&ehdr, hdr_buf, sizeof(ehdr));


    if (!elf_header_valid(&ehdr)) {
        pr_debug("[ELF] Invalid ELF header magic/class/machine in %s\n", path);
        vfs_close(file);
        return -ENOEXEC;
    }

    /* Load bias. ET_EXEC is fixed by its own program headers and cannot move;
     * only ET_DYN (PIE) can be slid, which is exactly why PIE is what makes
     * ASLR meaningful for the main image. */
    u64 load_bias = (ehdr.e_type == ET_DYN)
                        ? DYN_LOAD_BASE + aslr_offset(proc, ASLR_EXEC_RND_PAGES)
                        : 0;
    u64 main_entry = load_bias + ehdr.e_entry;

    /* Stack placement is randomised per exec. Everything downstream reads this
     * variable rather than the USER_STACK_TOP constant. */
    u64 stack_top  = USER_STACK_TOP - aslr_offset(proc, ASLR_STACK_RND_PAGES);
    u64 stack_base = stack_top - (USER_STACK_PAGES * PAGE_SIZE);

    /* Create clean user address space */
    vmm_space_t user_space = vmm_create_space();
    if (!user_space) {
        vfs_close(file);
        return -ENOMEM;
    }

    /* Load main executable segments */
    u64 phdr_user_vaddr = 0;
    u64 max_exec_vaddr = 0;
    u64 interp_offset = 0;
    u64 interp_filesz = 0;
    bool exec_stack = false;
    int err = load_elf_segments(proc, user_space, file, &ehdr, load_bias,
                                &phdr_user_vaddr, &max_exec_vaddr,
                                &interp_offset, &interp_filesz, &exec_stack);
    if (err < 0) {
        vmm_destroy_space(user_space);
        vfs_close(file);
        return err;
    }

    /* ── 3. Check for PT_INTERP (Dynamic Linker / ld.so) ─────────────────── */
    u64 final_entry = main_entry;
    u64 interp_base = 0;

    /* A PT_INTERP is a requirement, never a hint to search for another libc.
     * Linux permits up to PATH_MAX bytes including the terminating NUL. */
    if (interp_filesz) {
        if (interp_offset > 0x7fffffffffffffffULL ||
            interp_filesz > 0x7fffffffffffffffULL - interp_offset) {
            err = -EINVAL;
            goto image_fail;
        }
        char *interp_path = kmalloc((size_t)interp_filesz);
        if (!interp_path) { err = -ENOMEM; goto image_fail; }
        file->f_pos = interp_offset;
        s64 ilen = vfs_read(file, interp_path, (size_t)interp_filesz);
        if (ilen != (s64)interp_filesz) {
            err = ilen < 0 ? (int)ilen : -EIO;
            kfree(interp_path);
            goto image_fail;
        }
        if (interp_path[interp_filesz - 1] != '\0') {
            kfree(interp_path);
            err = -ENOEXEC;
            goto image_fail;
        }
        file_t *ifile = elf_open_interpreter(proc, interp_path, &open_err);
        kfree(interp_path);
        if (!ifile) { err = (int)open_err; goto image_fail; }
        if (!S_ISREG(ifile->f_inode->i_mode) || !(ifile->f_inode->i_mode & 0111)) {
            err = -EACCES;
        } else {
            elf64_ehdr_t iehdr;
            s64 nr = vfs_read(ifile, &iehdr, sizeof(iehdr));
            if (nr != sizeof(iehdr)) err = nr < 0 ? (int)nr : -EIO;
            else if (!elf_header_valid(&iehdr)) err = -ELIBBAD;
            else {
                interp_base = iehdr.e_type == ET_DYN
                            ? INTERP_LOAD_BASE + aslr_offset(proc, ASLR_INTERP_RND_PAGES) : 0;
                err = load_elf_segments(proc, user_space, ifile, &iehdr, interp_base,
                                        NULL, NULL, NULL, NULL, NULL);
                if (err == -ENOEXEC) err = -ELIBBAD;
                final_entry = interp_base + iehdr.e_entry;
            }
        }
        vfs_close(ifile);
        if (err < 0) goto image_fail;
    }

    vfs_close(file);

    /* NOTE: PT_GNU_RELRO is intentionally NOT applied here.
     *
     * On Linux the kernel NEVER applies RELRO write-protection at exec time.
     * The RELRO region (typically the GOT, .init_array, and other read-after-
     * relocation data) must remain writable so the dynamic linker can patch all
     * PLT/GOT entries during its startup relocation pass.  Only after ld.so has
     * finished every relocation does it call mprotect(PROT_READ) on the range
     * — which our mprotect(2) syscall already handles correctly.
     *
     * Applying RELRO in the kernel (as this code once did) write-protects the
     * GOT before ld.so can touch it, producing an immediate #PF with err=0x7
     * (present + write + user) in the dynamic linker's relocation loop — which
     * is exactly the fault that triggered this correction. */

    /* ── 4. Allocate the initial Ring-3 stack ─────────────────────────────── */
    for (vm_area_t *v = proc->vma_list; v; v = v->next) {
        if (stack_top - USER_STACK_MAX_BYTES < v->end && stack_top > v->start) {
            vmm_destroy_space(user_space);
            return -EINVAL;
        }
    }
    u64 stack_flags = VMM_USER_RW;
    u32 stack_prot  = VMA_PROT_READ | VMA_PROT_WRITE;
    if (exec_stack) {
        stack_flags &= ~VMM_F_NX;
        stack_prot  |= VMA_PROT_EXEC;
    }

    phys_addr_t top_page_phys = 0;
    for (u64 vaddr = stack_base; vaddr < stack_top; vaddr += PAGE_SIZE) {
        phys_addr_t phys = pmm_alloc_page();
        if (!phys) {
            vmm_destroy_space(user_space);
            return -ENOMEM;
        }
        hw_clear_page((void *)PHYS_TO_VIRT(phys));
        if (vmm_translate(user_space, vaddr) ||
            vmm_map(user_space, vaddr, phys, stack_flags) != 0) {
            pmm_free_page(phys);
            vmm_destroy_space(user_space);
            return -ENOMEM;
        }
        if (vaddr == stack_top - PAGE_SIZE) {
            top_page_phys = phys;
        }
    }

    /* ── 4b. vDSO: [vvar] + [vdso] (linux-vdso.so.1), AT_SYSINFO_EHDR ─────── */
    u64 vdso_base = 0;
    err = vdso_map(proc, user_space, &vdso_base);
    if (err < 0) { vmm_destroy_space(user_space); return err; }

    /* ── 5. Initialize Linux System V AMD64 Stack Frame ───────────────────── */
    err = setup_user_stack(proc, user_space, top_page_phys, path, argv, envp,
                         main_entry, phdr_user_vaddr, (u64)ehdr.e_phnum,
                         (u64)sizeof(elf64_phdr_t), interp_base, stack_top,
                         vdso_base, out_rsp);
    if (err < 0) {
        vmm_destroy_space(user_space);
        return err;
    }

    proc->heap_start   = ALIGN_UP(max_exec_vaddr, PAGE_SIZE);
    if (proc->heap_start == 0) proc->heap_start = 0x10000000;
    proc->heap_end     = proc->heap_start;
    proc->mmap_current = MMAP_ARENA_START + aslr_offset(proc, ASLR_MMAP_RND_PAGES);

    /* Publish the stack extent so execve() can register the matching VMA and
     * the fault handler's stack-growth window lines up with reality. The VMA
     * is deliberately wider than the pages mapped above, leaving room for the
     * stack to grow on demand. */
    proc->stack_high = stack_top;
    proc->stack_low  = stack_top - USER_STACK_MAX_BYTES;
    if (vma_add(proc, proc->stack_low, proc->stack_high,
                stack_prot, VMA_F_ANON | VMA_F_STACK) < 0) {
        vmm_destroy_space(user_space);
        return -ENOMEM;
    }

    *out_entry = (uintptr_t)final_entry;
    *out_space = user_space;
    pr_debug("[ELF] Successfully loaded %s (Entry: 0x%016llx, RSP: 0x%016llx, PML4: 0x%016llx, Heap: 0x%016llx)\n",
             path, (unsigned long long)*out_entry, (unsigned long long)*out_rsp, (unsigned long long)user_space,
             (unsigned long long)proc->heap_start);
    return 0;

image_fail:
    vfs_close(file);
    vmm_destroy_space(user_space);
    return err;
}


int elf_prepare_exec(process_t *proc, const char *path, const char *const argv[],
                     const char *const envp[], uintptr_t *out_entry,
                     phys_addr_t *out_space, u64 *out_rsp, process_t **out_image)
{
    if (!proc || !path || !out_entry || !out_space || !out_rsp || !out_image)
        return -EINVAL;
    *out_image = NULL;
    process_t *image = kzalloc(sizeof(*image));
    if (!image) return -ENOMEM;
    image->vma_lock = (spinlock_t)SPINLOCK_INIT;
    image->personality = proc->personality;
    image->uid = proc->uid; image->euid = proc->euid;
    image->gid = proc->gid; image->egid = proc->egid;
    vfs_namespace_lock();
    memcpy(image->root, proc->root, sizeof(image->root));
    memcpy(image->cwd, proc->cwd, sizeof(image->cwd));
    vfs_namespace_unlock();
    int err = elf_load_exec_internal(image, path, argv, envp, out_entry, out_space, out_rsp, 0);
    if (err < 0) { vma_reset(image); kfree(image); return err; }
    *out_image = image;
    return 0;
}

/* Caller has stopped siblings before publishing an exec image. Spawned
 * processes have no runnable thread yet. There are no fallible operations here. */
void elf_commit_exec(process_t *proc, process_t *image)
{
    vma_reset(proc);
    irqflags_t flags = spinlock_lock_irqsave(&proc->vma_lock);
    proc->vma_list = image->vma_list;
    proc->vma_tree = image->vma_tree;
    proc->vmacache_seqnum++;
    /* No set-ID or file-capability exec exists yet: a committed ordinary
     * image resets dumpability just as Linux does. Failed exec preserves it. */
    __atomic_store_n(&proc->dumpable,true,__ATOMIC_RELEASE);
    proc->heap_start = image->heap_start;
    proc->heap_end = image->heap_end;
    proc->mmap_current = image->mmap_current;
    proc->stack_low = image->stack_low;
    proc->stack_high = image->stack_high;
    spinlock_unlock_irqrestore(&proc->vma_lock, flags);
    kfree(image);
}

int elf_load_exec(process_t *proc, const char *path, const char *const argv[], const char *const envp[],
                  uintptr_t *out_entry, phys_addr_t *out_space, u64 *out_rsp)
{
    process_t *image = NULL;
    int err = elf_prepare_exec(proc, path, argv, envp, out_entry, out_space, out_rsp, &image);
    if (!err) elf_commit_exec(proc, image);
    return err;
}

process_t *sched_spawn_user_args(const char *path, const char *const argv[], const char *const envp[])
{
    pr_debug("[ELF] sched_spawn_user calling proc_create for '%s'\n", path);
    process_t *proc = proc_create(path, 0);
    if (!proc) return NULL;

    uintptr_t entry = 0;
    phys_addr_t new_space = 0;
    u64 new_rsp = 0;
    if (elf_load_exec(proc, path, argv, envp, &entry, &new_space, &new_rsp) < 0) {
        pr_debug("[ELF] sched_spawn_user elf_load_exec failed for '%s'\n", path);
        proc_destroy(proc);
        return NULL;
    }
    
    proc->pml4_phys = new_space;

    /* Initialize standard I/O file descriptors (0=stdin, 1=stdout, 2=stderr) */
    for (int fd = 0; fd < 3; fd++) {
        file_t *con = vfs_open("/dev/console", 0, 0);
        if (con) proc->handle_table[fd] = con;
    }

    /* Create user thread starting at entry with RSP pointing to System V AMD64 initial stack */
    thread_t *t = thread_create(proc, entry, new_rsp, false);
    if (!t) {
        proc_destroy(proc);
        return NULL;
    }

    pr_debug("[ELF] sched_spawn_user finished for '%s' (PID %u)\n", path, proc->pid);
    return proc;
}

process_t *sched_spawn_user(const char *path)
{
    const char *default_argv[] = { path, NULL };
    const char *default_envp[] = {
        "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin:/",
        "TERM=azami",
        "USER=root",
        "HOME=/root",
        "SHELL=/bin/sh",
        "TMPDIR=/tmp",
        "COMPILER_PATH=/usr/libexec/gcc/x86_64-elf/14.2.0/:/usr/libexec:/usr/bin:/bin",
        "LIBRARY_PATH=/usr/lib/gcc/x86_64-elf/14.2.0/:/usr/lib:/lib:/lib64:/usr/local/lib",
        NULL
    };
    return sched_spawn_user_args(path, default_argv, default_envp);
}

process_t *sched_spawn_user_arg(const char *path, const char *arg)
{
    const char *default_envp[] = {
        "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin:/",
        "TERM=azami",
        "USER=root",
        "HOME=/root",
        "SHELL=/bin/sh",
        "TMPDIR=/tmp",
        "COMPILER_PATH=/usr/libexec/gcc/x86_64-elf/14.2.0/:/usr/libexec:/usr/bin:/bin",
        "LIBRARY_PATH=/usr/lib/gcc/x86_64-elf/14.2.0/:/usr/lib:/lib:/lib64:/usr/local/lib",
        NULL
    };
    if (arg && arg[0]) {
        const char *argv[] = { path, arg, NULL };
        return sched_spawn_user_args(path, argv, default_envp);
    }
    const char *argv[] = { path, NULL };
    return sched_spawn_user_args(path, argv, default_envp);
}
