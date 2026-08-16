/* ============================================================================
 * AzamiOS — 64-bit ELF Executable Loader & User Process Spawner
 * File: kernel/sched/elf.c
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

static int setup_user_stack(vmm_space_t user_space, phys_addr_t top_page_phys,
                            const char *path, const char *const argv[], const char *const envp[],
                            u64 *out_rsp)
{
    (void)user_space;
    char *kstack = (char *)PHYS_TO_VIRT(top_page_phys);
    size_t top_offset = PAGE_SIZE; /* Start at top of the 4KB page */

    /* Determine argc and collect argv pointers */
    int argc = 0;
    if (argv) {
        while (argv[argc] && argc < 64) argc++;
    }
    if (argc == 0) argc = 1;

    /* Determine envc and collect envp pointers */
    int envc = 0;
    if (envp) {
        while (envp[envc] && envc < 64) envc++;
    }

    u64 u_envp[64];
    for (int i = 0; i < envc; i++) {
        const char *src = envp[i];
        size_t len = strlen(src) + 1;
        if (top_offset < len + 256) return -ENOMEM;
        top_offset -= len;
        memcpy(kstack + top_offset, src, len);
        u_envp[i] = 0x00007fffffffe000ULL + top_offset;
    }

    u64 u_argv[64];
    for (int i = 0; i < argc; i++) {
        const char *src = (argv && argv[i]) ? argv[i] : path;
        size_t len = strlen(src) + 1;
        if (top_offset < len + 256) return -ENOMEM;
        top_offset -= len;
        memcpy(kstack + top_offset, src, len);
        u_argv[i] = 0x00007fffffffe000ULL + top_offset;
    }

    /* Align down to 8 bytes */
    top_offset &= ~7ULL;

    /* Calculate total pointer slots needed on stack */
    /* AT_NULL (2 slots: type, val) + envp (envc + 1) + argv (argc + 1) + argc (1) */
    size_t num_slots = 2 + (envc + 1) + (argc + 1) + 1;

    /* Check 16-byte stack alignment: we want (top_offset - num_slots * 8) % 16 == 0 */
    if (((top_offset - (num_slots * 8)) & 0xF) != 0) {
        top_offset -= 8; /* Add 8-byte pad before auxiliary vectors */
        *(u64 *)(kstack + top_offset) = 0;
    }

    /* Auxv AT_NULL */
    top_offset -= 8; *(u64 *)(kstack + top_offset) = 0;
    top_offset -= 8; *(u64 *)(kstack + top_offset) = 0;

    /* Envp NULL terminator */
    top_offset -= 8; *(u64 *)(kstack + top_offset) = 0;
    for (int i = envc - 1; i >= 0; i--) {
        top_offset -= 8;
        *(u64 *)(kstack + top_offset) = u_envp[i];
    }

    /* Argv NULL terminator */
    top_offset -= 8; *(u64 *)(kstack + top_offset) = 0;
    for (int i = argc - 1; i >= 0; i--) {
        top_offset -= 8;
        *(u64 *)(kstack + top_offset) = u_argv[i];
    }

    /* Argc */
    top_offset -= 8;
    *(u64 *)(kstack + top_offset) = (u64)argc;

    *out_rsp = 0x00007fffffffe000ULL + top_offset;
    return 0;
}

int elf_load_exec(process_t *proc, const char *path, const char *const argv[], const char *const envp[],
                  uintptr_t *out_entry, phys_addr_t *out_space, u64 *out_rsp)
{
    if (!proc || !path || !out_entry || !out_space || !out_rsp) return -EINVAL;

    pr_debug("[ELF] elf_load_exec vfs_open '%s'\n", path);
    file_t *file = vfs_open(path, 0, 0);
    if (!file) {
        char fallback_path[256];
        if (path[0] == '/') {
            snprintf(fallback_path, sizeof(fallback_path), "/bin%s", path);
            file = vfs_open(fallback_path, 0, 0);
            if (!file) {
                snprintf(fallback_path, sizeof(fallback_path), "/sbin%s", path);
                file = vfs_open(fallback_path, 0, 0);
            }
            if (!file) {
                snprintf(fallback_path, sizeof(fallback_path), "/usr/bin%s", path);
                file = vfs_open(fallback_path, 0, 0);
            }
        } else {
            snprintf(fallback_path, sizeof(fallback_path), "/bin/%s", path);
            file = vfs_open(fallback_path, 0, 0);
            if (!file) {
                snprintf(fallback_path, sizeof(fallback_path), "/sbin/%s", path);
                file = vfs_open(fallback_path, 0, 0);
            }
            if (!file) {
                snprintf(fallback_path, sizeof(fallback_path), "/bin/%s.elf", path);
                file = vfs_open(fallback_path, 0, 0);
            }
            if (!file) {
                snprintf(fallback_path, sizeof(fallback_path), "/sbin/%s.elf", path);
                file = vfs_open(fallback_path, 0, 0);
            }
        }
    }
    if (!file) {
        pr_debug("[ELF] Failed to open ELF file: %s\n", path);
        return -ENOENT;
    }

    elf64_ehdr_t ehdr;
    file->f_pos = 0;
    if (vfs_read(file, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
        vfs_close(file);
        return -ENOEXEC;
    }

    if (ehdr.e_ident_magic != ELF_MAGIC || ehdr.e_machine != EM_X86_64) {
        pr_debug("[ELF] Invalid ELF header magic or architecture in %s\n", path);
        vfs_close(file);
        return -ENOEXEC;
    }

    /* Create clean user address space using VMM */
    vmm_space_t user_space = vmm_create_space();
    if (!user_space) {
        vfs_close(file);
        return -ENOMEM;
    }

    /* Read and map program segments */
    for (u16 i = 0; i < ehdr.e_phnum; i++) {
        elf64_phdr_t phdr;
        file->f_pos = ehdr.e_phoff + i * sizeof(phdr);
        if (vfs_read(file, &phdr, sizeof(phdr)) != sizeof(phdr)) {
            continue;
        }

        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0) continue;
        
        /* Prevent Integer Overflow */
        if (phdr.p_vaddr + phdr.p_memsz < phdr.p_vaddr) {
            vmm_destroy_space(user_space);
            vfs_close(file);
            return -EINVAL;
        }

        u64 start_vaddr = ALIGN_DOWN(phdr.p_vaddr, PAGE_SIZE);
        u64 end_vaddr = ALIGN_UP(phdr.p_vaddr + phdr.p_memsz, PAGE_SIZE);

        /* Security Boundary: Prevent mapping outside canonical lower half */
        if (end_vaddr > 0x00007FFFFFFFFFFF) {
            vmm_destroy_space(user_space);
            vfs_close(file);
            return -EINVAL;
        }

        u64 vmm_flags = VMM_F_PRESENT | VMM_F_USER;
        if (phdr.p_flags & PF_W) vmm_flags |= VMM_F_WRITE;
        if (!(phdr.p_flags & PF_X)) vmm_flags |= VMM_F_NX;

        for (u64 vaddr = start_vaddr; vaddr < end_vaddr; vaddr += PAGE_SIZE) {
            phys_addr_t phys = pmm_alloc_page();
            if (!phys) {
                vmm_destroy_space(user_space);
                vfs_close(file);
                return -ENOMEM;
            }

            void *page_buf = (void *)PHYS_TO_VIRT(phys);
            __builtin_memset(page_buf, 0, PAGE_SIZE);
            vmm_map(user_space, vaddr, phys, vmm_flags);

            /* Copy data from file if within p_filesz range */
            if (vaddr + PAGE_SIZE > phdr.p_vaddr && vaddr < phdr.p_vaddr + phdr.p_filesz) {
                u64 page_offset = (vaddr > phdr.p_vaddr) ? (vaddr - phdr.p_vaddr) : 0;
                u64 file_offset = phdr.p_offset + page_offset;
                u64 dest_offset = (vaddr > phdr.p_vaddr) ? 0 : (phdr.p_vaddr - vaddr);

                if (page_offset < phdr.p_filesz) {
                    u64 copy_len = MIN(PAGE_SIZE - dest_offset, phdr.p_filesz - page_offset);

                    if (copy_len > 0) {
                        file->f_pos = file_offset;
                        s64 nread = vfs_read(file, (char *)page_buf + dest_offset, (size_t)copy_len);
                        if (nread < 0) {
                            vmm_destroy_space(user_space);
                            vfs_close(file);
                            return -EIO;
                        }
                    }
                }
            }
        }
    }

    vfs_close(file);

    /* Allocate and map ring-3 user stack (16 KB at 0x00007fffffffb000) */
    u64 stack_base = 0x00007fffffffb000ULL;
    phys_addr_t top_page_phys = 0;
    for (u64 vaddr = stack_base; vaddr < stack_base + (4 * PAGE_SIZE); vaddr += PAGE_SIZE) {
        phys_addr_t phys = pmm_alloc_page();
        if (phys) {
            __builtin_memset((void *)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
            vmm_map(user_space, vaddr, phys, VMM_USER_RW);
            if (vaddr == stack_base + (3 * PAGE_SIZE)) {
                top_page_phys = phys;
            }
        } else {
            vmm_destroy_space(user_space);
            return -ENOMEM;
        }
    }

    /* Initialize System V AMD64 stack frame */
    if (setup_user_stack(user_space, top_page_phys, path, argv, envp, out_rsp) < 0) {
        vmm_destroy_space(user_space);
        return -ENOMEM;
    }

    proc->heap_start   = 0x10000000; /* 256 MB base for user heap */
    proc->heap_end     = proc->heap_start;
    proc->mmap_current = 0x0000600000000000ULL;

    *out_entry = (uintptr_t)ehdr.e_entry;
    *out_space = user_space;
    pr_debug("[ELF] Successfully loaded %s (Entry: 0x%016llx, RSP: 0x%016llx, PML4: 0x%016llx)\n",
            path, (unsigned long long)*out_entry, (unsigned long long)*out_rsp, (unsigned long long)user_space);
    return 0;
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
    const char *default_envp[] = { "PATH=/", "TERM=azami", "USER=root", "HOME=/", NULL };
    return sched_spawn_user_args(path, default_argv, default_envp);
}
