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
#include "../../arch/x86_64/mm/vmm.h"
#include "../../drivers/char/console.h"
#include "../../include/azami/defs.h"
#include "../../kernel/lib/string.h"
#include "../../kernel/lib/random.h"

#define USER_STACK_TOP      0x00007fffffffe000ULL
#define USER_STACK_PAGES    64
#define USER_STACK_BASE     (USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE))

#define DYN_LOAD_BASE       0x0000555555554000ULL
#define INTERP_LOAD_BASE    0x00007ffff7dd5000ULL
#define MAX_RECURSION_DEPTH 4

/* AT_RANDOM entropy comes straight from the kernel CSPRNG. */
static void get_random_bytes(void *buf, size_t n)
{
    krandom_bytes(buf, n);
}

static int setup_user_stack(process_t *proc, vmm_space_t user_space, phys_addr_t top_page_phys,
                            const char *exec_path, const char *const argv[], const char *const envp[],
                            u64 main_entry, u64 phdr_vaddr, u64 phnum, u64 phent, u64 interp_base,
                            u64 *out_rsp)
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
    #define STK_NEED(n) do { if (top_offset < (size_t)(n)) goto out; } while (0)
    #define PUSHQ(v)    do { STK_NEED(8); top_offset -= 8; \
                             *(u64 *)(kstack + top_offset) = (u64)(v); } while (0)

    /* Number of auxv (key,value) pairs written below — keep in sync. */
    #define AUX_PAIRS 18

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
    u64 u_random_ptr = USER_STACK_TOP - (PAGE_SIZE - top_offset);

    /* 2. AT_PLATFORM string ("x86_64") */
    const char *platform_str = "x86_64";
    size_t plat_len = strlen(platform_str) + 1;
    STK_NEED(plat_len + fixed_bytes);
    top_offset -= plat_len;
    memcpy(kstack + top_offset, platform_str, plat_len);
    u64 u_platform_ptr = USER_STACK_TOP - (PAGE_SIZE - top_offset);

    /* 3. AT_EXECFN string (path to executable) */
    const char *execfn_src = exec_path ? exec_path : "/bin/sh.elf";
    size_t execfn_len = strlen(execfn_src) + 1;
    STK_NEED(execfn_len + fixed_bytes);
    top_offset -= execfn_len;
    memcpy(kstack + top_offset, execfn_src, execfn_len);
    u64 u_execfn_ptr = USER_STACK_TOP - (PAGE_SIZE - top_offset);

    /* 4. envp strings */
    for (int i = 0; i < envc; i++) {
        const char *src = envp[i] ? envp[i] : "";
        size_t len = strlen(src) + 1;
        STK_NEED(len + fixed_bytes);
        top_offset -= len;
        memcpy(kstack + top_offset, src, len);
        u_envp[i] = USER_STACK_TOP - (PAGE_SIZE - top_offset);
    }

    /* 5. argv strings */
    for (int i = 0; i < argc; i++) {
        const char *src = (argv && argv[i]) ? argv[i] : (exec_path ? exec_path : "");
        size_t len = strlen(src) + 1;
        STK_NEED(len + fixed_bytes);
        top_offset -= len;
        memcpy(kstack + top_offset, src, len);
        u_argv[i] = USER_STACK_TOP - (PAGE_SIZE - top_offset);
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
    PUSHQ(u_platform_ptr);                  PUSHQ(AT_PLATFORM);
    PUSHQ(u_execfn_ptr);                    PUSHQ(AT_EXECFN);
    PUSHQ(u_random_ptr);                    PUSHQ(AT_RANDOM);
    PUSHQ(0);                               PUSHQ(AT_SECURE);
    PUSHQ(100);                             PUSHQ(AT_CLKTCK);
    PUSHQ(0xbfebfbffULL);                   PUSHQ(AT_HWCAP);
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

    *out_rsp = USER_STACK_TOP - (PAGE_SIZE - top_offset);
    rc = 0;

out:
    kfree(u_argv);
    kfree(u_envp);
    #undef STK_NEED
    #undef PUSHQ
    #undef AUX_PAIRS
    return rc;
}

/* Helper to map and load ELF PT_LOAD segments into a virtual address space */
static int load_elf_segments(vmm_space_t user_space, file_t *file, const elf64_ehdr_t *ehdr,
                             u64 load_bias, u64 *out_phdr_vaddr, u64 *out_max_vaddr)
{
    u64 phdr_user_vaddr = 0;
    u64 max_vaddr = 0;

    for (u16 i = 0; i < ehdr->e_phnum; i++) {
        elf64_phdr_t phdr;
        file->f_pos = ehdr->e_phoff + (u64)i * sizeof(phdr);
        if (vfs_read(file, &phdr, sizeof(phdr)) != sizeof(phdr)) {
            continue;
        }

        if (phdr.p_type == PT_PHDR) {
            phdr_user_vaddr = load_bias + phdr.p_vaddr;
        }

        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0) continue;

        u64 seg_vaddr = load_bias + phdr.p_vaddr;

        /* Prevent Integer Overflow */
        if (seg_vaddr + phdr.p_memsz < seg_vaddr) {
            return -EINVAL;
        }

        if (seg_vaddr + phdr.p_memsz > max_vaddr) {
            max_vaddr = seg_vaddr + phdr.p_memsz;
        }

        u64 start_vaddr = ALIGN_DOWN(seg_vaddr, PAGE_SIZE);
        u64 end_vaddr = ALIGN_UP(seg_vaddr + phdr.p_memsz, PAGE_SIZE);

        /* Boundary Check: Lower half user space */
        if (end_vaddr > 0x00007FFFFFFFFFFF) {
            return -EINVAL;
        }

        u64 vmm_flags = VMM_F_PRESENT | VMM_F_USER;
        if (phdr.p_flags & PF_W) vmm_flags |= VMM_F_WRITE;
        if (!(phdr.p_flags & PF_X)) vmm_flags |= VMM_F_NX;

        /* 1. Allocate & map all pages for this segment */
        for (u64 vaddr = start_vaddr; vaddr < end_vaddr; vaddr += PAGE_SIZE) {
            phys_addr_t phys = vmm_translate(user_space, vaddr);
            if (phys) {
                /* Page already mapped by an adjacent segment (page-unaligned
                 * segment boundary). Merge permissions conservatively rather
                 * than forcing RWX: writable if either segment needs it,
                 * executable only if either segment is executable. */
                phys = phys & VMM_PHYS_MASK;
                u64 old_flags = vmm_query_flags(user_space, vaddr);
                u64 merged = VMM_F_PRESENT | VMM_F_USER;
                if ((old_flags & VMM_F_WRITE) || (vmm_flags & VMM_F_WRITE))
                    merged |= VMM_F_WRITE;
                if ((old_flags & VMM_F_NX) && (vmm_flags & VMM_F_NX))
                    merged |= VMM_F_NX;   /* NX only when neither part is code */
                vmm_map(user_space, vaddr, phys, merged);
            } else {
                phys = pmm_alloc_page();
                if (!phys) return -ENOMEM;

                void *page_buf = (void *)PHYS_TO_VIRT(phys);
                __builtin_memset(page_buf, 0, PAGE_SIZE);
                vmm_map(user_space, vaddr, phys, vmm_flags);
            }
        }

        /* 2. Stream file data directly into mapped memory pages */
        if (phdr.p_filesz > 0) {
            file->f_pos = phdr.p_offset;
            u64 bytes_loaded = 0;
            while (bytes_loaded < phdr.p_filesz) {
                u64 cur_vaddr = seg_vaddr + bytes_loaded;
                phys_addr_t phys = vmm_translate(user_space, cur_vaddr) & VMM_PHYS_MASK;
                if (!phys) return -EIO;

                void *page_buf = (void *)PHYS_TO_VIRT(phys);
                u64 page_offset = cur_vaddr & (PAGE_SIZE - 1);
                size_t chunk = (size_t)MIN(PAGE_SIZE - page_offset, phdr.p_filesz - bytes_loaded);

                s64 nread = vfs_read(file, (char *)page_buf + page_offset, chunk);
                if (nread <= 0) break;
                bytes_loaded += (u64)nread;
            }
        }
    }

    if (phdr_user_vaddr == 0 && ehdr->e_phoff != 0) {
        phdr_user_vaddr = load_bias + ehdr->e_phoff;
    }
    if (out_phdr_vaddr) *out_phdr_vaddr = phdr_user_vaddr;
    if (out_max_vaddr) *out_max_vaddr = max_vaddr;
    return 0;
}


/* Recursive ELF / Script loader */
static int elf_load_exec_internal(process_t *proc, const char *path, const char *const argv[], const char *const envp[],
                                 uintptr_t *out_entry, phys_addr_t *out_space, u64 *out_rsp, int recursion_depth)
{
    if (recursion_depth > MAX_RECURSION_DEPTH) return -ELOOP;
    if (!proc || !path || !out_entry || !out_space || !out_rsp) return -EINVAL;

    pr_debug("[ELF] Loading binary or script '%s' (depth %d)\n", path, recursion_depth);
    file_t *file = vfs_open(path, 0, 0);
    if (!file) {
        char fallback_path[256];
        if (path[0] == '/') {
            snprintf(fallback_path, sizeof(fallback_path), "%s.elf", path);
            file = vfs_open(fallback_path, 0, 0);
            if (!file) {
                snprintf(fallback_path, sizeof(fallback_path), "/bin%s", path);
                file = vfs_open(fallback_path, 0, 0);
            }
            if (!file) {
                snprintf(fallback_path, sizeof(fallback_path), "/sbin%s", path);
                file = vfs_open(fallback_path, 0, 0);
            }
        } else {
            snprintf(fallback_path, sizeof(fallback_path), "/bin/%s", path);
            file = vfs_open(fallback_path, 0, 0);
            if (!file) {
                snprintf(fallback_path, sizeof(fallback_path), "/bin/%s.elf", path);
                file = vfs_open(fallback_path, 0, 0);
            }
            if (!file) {
                snprintf(fallback_path, sizeof(fallback_path), "/sbin/%s", path);
                file = vfs_open(fallback_path, 0, 0);
            }
            if (!file) {
                snprintf(fallback_path, sizeof(fallback_path), "/sbin/%s.elf", path);
                file = vfs_open(fallback_path, 0, 0);
            }
        }
    }
    if (!file) {
        pr_debug("[ELF] Failed to open binary: %s\n", path);
        return -ENOENT;
    }

    /* Read header bytes to check for Shebang (#!) or ELF magic */
    char hdr_buf[128];
    file->f_pos = 0;
    s64 nread = vfs_read(file, hdr_buf, sizeof(hdr_buf));
    if (nread < 4) {
        vfs_close(file);
        return -ENOEXEC;
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

        /* Split interpreter and optional argument */
        char *interp = interp_line;
        while (*interp == ' ' || *interp == '\t') interp++;
        char *arg = NULL;
        char *space = strchr(interp, ' ');
        if (!space) space = strchr(interp, '\t');
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
        }
        new_argv[new_argc] = NULL;

        return elf_load_exec_internal(proc, interp, new_argv, envp, out_entry, out_space, out_rsp, recursion_depth + 1);
    }

    /* ── 2. Standard 64-bit ELF Execution ─────────────────────────────────── */
    elf64_ehdr_t ehdr;
    memcpy(&ehdr, hdr_buf, sizeof(ehdr));
    if (nread < (s64)sizeof(ehdr)) {
        vfs_close(file);
        return -ENOEXEC;
    }

    if (ehdr.e_ident_magic != ELF_MAGIC || ehdr.e_ident_class != ELFCLASS64 ||
        ehdr.e_ident_data != ELFDATA2LSB || ehdr.e_machine != EM_X86_64) {
        pr_debug("[ELF] Invalid ELF header magic/class/machine in %s\n", path);
        vfs_close(file);
        return -ENOEXEC;
    }

    if (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN) {
        pr_debug("[ELF] Unsupported ELF type %u in %s (only ET_EXEC and ET_DYN supported)\n", ehdr.e_type, path);
        vfs_close(file);
        return -ENOEXEC;
    }

    /* Calculate load bias: ET_DYN (PIE) relocates to DYN_LOAD_BASE, ET_EXEC is 0 */
    u64 load_bias = (ehdr.e_type == ET_DYN) ? DYN_LOAD_BASE : 0;
    u64 main_entry = load_bias + ehdr.e_entry;

    /* Create clean user address space */
    vmm_space_t user_space = vmm_create_space();
    if (!user_space) {
        vfs_close(file);
        return -ENOMEM;
    }

    /* Load main executable segments */
    u64 phdr_user_vaddr = 0;
    u64 max_exec_vaddr = 0;
    int err = load_elf_segments(user_space, file, &ehdr, load_bias, &phdr_user_vaddr, &max_exec_vaddr);
    if (err < 0) {
        vmm_destroy_space(user_space);
        vfs_close(file);
        return err;
    }

    /* ── 3. Check for PT_INTERP (Dynamic Linker / ld.so) ─────────────────── */
    u64 final_entry = main_entry;
    u64 interp_base = 0;

    for (u16 i = 0; i < ehdr.e_phnum; i++) {
        elf64_phdr_t phdr;
        file->f_pos = ehdr.e_phoff + (u64)i * sizeof(phdr);
        if (vfs_read(file, &phdr, sizeof(phdr)) != sizeof(phdr)) continue;

        if (phdr.p_type == PT_INTERP && phdr.p_filesz > 0 && phdr.p_filesz < 256) {
            char interp_path[256];
            file->f_pos = phdr.p_offset;
            s64 ilen = vfs_read(file, interp_path, phdr.p_filesz);
            if (ilen > 0) {
                if (ilen >= (s64)sizeof(interp_path)) ilen = sizeof(interp_path) - 1;
                interp_path[ilen] = '\0';

                file_t *ifile = vfs_open(interp_path, 0, 0);
                if (!ifile) ifile = vfs_open("/lib64/ld-linux-x86-64.so.2", 0, 0);
                if (!ifile) ifile = vfs_open("/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2", 0, 0);
                if (!ifile) ifile = vfs_open("/lib/ld.so", 0, 0);
                if (!ifile) ifile = vfs_open("/bin/ld.so", 0, 0);

                if (ifile) {
                    elf64_ehdr_t iehdr;
                    if (vfs_read(ifile, &iehdr, sizeof(iehdr)) == sizeof(iehdr)) {
                        if (iehdr.e_ident_magic == ELF_MAGIC && iehdr.e_machine == EM_X86_64) {
                            interp_base = (iehdr.e_type == ET_DYN) ? INTERP_LOAD_BASE : 0;
                            u64 iphdr_vaddr = 0;
                            u64 imax_vaddr = 0;
                            if (load_elf_segments(user_space, ifile, &iehdr, interp_base, &iphdr_vaddr, &imax_vaddr) == 0) {
                                final_entry = interp_base + iehdr.e_entry;
                                pr_debug("[ELF] Loaded dynamic linker %s (Base: 0x%016llx, Entry: 0x%016llx)\n",
                                         interp_path, (unsigned long long)interp_base, (unsigned long long)final_entry);
                            }
                        }
                    }
                    vfs_close(ifile);
                }
            }
            break;
        }
    }

    vfs_close(file);

    /* ── 4. Allocate 16 KB Ring-3 User Stack ──────────────────────────────── */
    phys_addr_t top_page_phys = 0;
    for (u64 vaddr = USER_STACK_BASE; vaddr < USER_STACK_TOP; vaddr += PAGE_SIZE) {
        phys_addr_t phys = pmm_alloc_page();
        if (!phys) {
            vmm_destroy_space(user_space);
            return -ENOMEM;
        }
        __builtin_memset((void *)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
        vmm_map(user_space, vaddr, phys, VMM_USER_RW);
        if (vaddr == USER_STACK_TOP - PAGE_SIZE) {
            top_page_phys = phys;
        }
    }

    /* ── 5. Initialize Linux System V AMD64 Stack Frame ───────────────────── */
    if (setup_user_stack(proc, user_space, top_page_phys, path, argv, envp,
                         main_entry, phdr_user_vaddr, (u64)ehdr.e_phnum,
                         (u64)sizeof(elf64_phdr_t), interp_base, out_rsp) < 0) {
        vmm_destroy_space(user_space);
        return -ENOMEM;
    }

    proc->heap_start   = ALIGN_UP(max_exec_vaddr, PAGE_SIZE);
    if (proc->heap_start == 0) proc->heap_start = 0x10000000;
    proc->heap_end     = proc->heap_start;
    proc->mmap_current = 0x0000600000000000ULL;

    *out_entry = (uintptr_t)final_entry;
    *out_space = user_space;
    pr_debug("[ELF] Successfully loaded %s (Entry: 0x%016llx, RSP: 0x%016llx, PML4: 0x%016llx, Heap: 0x%016llx)\n",
             path, (unsigned long long)*out_entry, (unsigned long long)*out_rsp, (unsigned long long)user_space,
             (unsigned long long)proc->heap_start);
    return 0;
}


int elf_load_exec(process_t *proc, const char *path, const char *const argv[], const char *const envp[],
                  uintptr_t *out_entry, phys_addr_t *out_space, u64 *out_rsp)
{
    return elf_load_exec_internal(proc, path, argv, envp, out_entry, out_space, out_rsp, 0);
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
    const char *default_envp[] = { "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/", "TERM=azami", "USER=root", "HOME=/root", "SHELL=/bin/sh.elf", NULL };
    return sched_spawn_user_args(path, default_argv, default_envp);
}
