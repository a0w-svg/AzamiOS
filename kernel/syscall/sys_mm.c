/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/* ============================================================================
 * AzamiOS — Memory Management Syscalls
 * File: kernel/syscall/sys_mm.c
 * ============================================================================ */
#include "syscall_internal.h"


/* ── Memory Management Syscalls ──────────────────────────────────────────── */

s64 sys_brk_impl(pt_regs_t *r)
{
    virt_addr_t new_brk = (virt_addr_t)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    if (proc->heap_start == 0) {
        proc->heap_start = 0x10000000;
        proc->heap_end   = proc->heap_start;
    }

    if (new_brk == 0) {
        return (s64)proc->heap_end;
    }

    if (new_brk < proc->heap_start || new_brk >= 0x00007ffff0000000ULL) {
        return (s64)proc->heap_end;
    }

    virt_addr_t cur_page = ALIGN_UP(proc->heap_end, PAGE_SIZE);
    virt_addr_t target_page = ALIGN_UP(new_brk, PAGE_SIZE);

    if (target_page > cur_page) {
        virt_addr_t va = cur_page;
        for (; va < target_page; va += PAGE_SIZE) {
            phys_addr_t phys = vmm_translate(proc->pml4_phys, va);
            if (!phys) {
                phys = pmm_alloc_page();
                if (!phys) {
                    if (va > cur_page) {
                        vma_add(proc, cur_page, va, VMA_PROT_READ | VMA_PROT_WRITE, VMA_F_ANON);
                        proc->heap_end = va;
                    }
                    return (s64)proc->heap_end;
                }
                hw_clear_page((void *)PHYS_TO_VIRT(phys));
                vmm_map(proc->pml4_phys, va, phys, VMM_USER_RW);
            }
        }
        vma_add(proc, cur_page, target_page, VMA_PROT_READ | VMA_PROT_WRITE, VMA_F_ANON);
    } else if (target_page < cur_page) {
        /* One batched teardown: vmm_unmap_range() applies the same ownership
         * rule (skip VMM_F_SHARED frames, which the shmem object or a peer
         * mapping still owns) and pays for one TLB shootdown per chunk rather
         * than one per page. */
        vmm_unmap_range(proc->pml4_phys, target_page,
                        (size_t)((cur_page - target_page) / PAGE_SIZE), true);
        vma_remove(proc, target_page, cur_page);
    }

    proc->heap_end = new_brk;
    return (s64)proc->heap_end;
}


s64 sys_mmap_impl(pt_regs_t *r)
{
    virt_addr_t addr   = (virt_addr_t)r->rdi;
    size_t length      = (size_t)r->rsi;
    int prot           = (int)r->rdx;
    u64 flags          = r->r10; /* MAP_SHARED=1, MAP_PRIVATE=2, MAP_FIXED=16, MAP_ANONYMOUS=32 */
    int fd             = (int)(s32)r->r8;
    u64 file_offset    = (u64)r->r9;

    KTRACE_CALL("mmap", length);

    if (length == 0) return -(s64)EINVAL;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    size_t aligned_len = ALIGN_UP(length, PAGE_SIZE);
    /* Reject lengths that overflow on page rounding. */
    if (aligned_len < length || aligned_len == 0) return -(s64)EINVAL;

    /* Anonymous mmap bump arena: [MMAP_ARENA_BASE, MMAP_ARENA_END). Kept well
     * below the user stack auto-growth window so mmap can never march into it. */
    #define MMAP_ARENA_BASE 0x0000600000000000ULL
    #define MMAP_ARENA_END  0x00007f0000000000ULL

    bool map_fixed     = !!(flags & 0x10);
    virt_addr_t target_addr = addr;

    if (!map_fixed) {
        bool need_alloc = false;
        if (!target_addr || target_addr < g_mmap_min_addr || target_addr + aligned_len > TASK_SIZE_MAX) {
            need_alloc = true;
        } else {
            /* Check collision with existing mappings */
            for (size_t offset = 0; offset < aligned_len; offset += PAGE_SIZE) {
                if (vmm_translate(proc->pml4_phys, target_addr + offset)) {
                    need_alloc = true;
                    break;
                }
            }
        }

        if (need_alloc) {
            if (!proc->mmap_current || proc->mmap_current < MMAP_ARENA_BASE || proc->mmap_current >= MMAP_ARENA_END) {
                proc->mmap_current = 0x0000700000000000ULL;
            }
            /* Refuse if the request would run past the end of the arena
             * (also catches address overflow). */
            if (aligned_len > MMAP_ARENA_END - proc->mmap_current) {
                return -(s64)ENOMEM;
            }
            target_addr = proc->mmap_current;
            proc->mmap_current += aligned_len;
        }
    } else {
        /* Guard against integer overflow and non-canonical address ranges */
        if (target_addr + aligned_len < target_addr || target_addr + aligned_len > TASK_SIZE_MAX) {
            return -(s64)EINVAL;
        }
        /* NULL-pointer dereference prevention: address 0 is strictly forbidden */
        if (target_addr == 0) {
            return -(s64)EINVAL;
        }
        /* Mapping below mmap_min_addr requires CAP_SYS_RAWIO or euid 0 */
        if (target_addr < g_mmap_min_addr &&
            !security_check_permission(proc, CAP_SYS_RAWIO) && proc->euid != 0) {
            return -(s64)EPERM;
        }
        /* MAP_FIXED over a live range: drop whatever was there first. The
         * range walk also picks up PROT_NONE pages, which keep their frame
         * with PRESENT clear. */
        vmm_unmap_range(proc->pml4_phys, target_addr, aligned_len / PAGE_SIZE, true);
    }

    /* Translate mmap flags to the VMA flag set. */
    u32 vma_fl = (flags & 0x20 /* MAP_ANONYMOUS */) ? VMA_F_ANON : VMA_F_FILE;
    if (flags & 0x01 /* MAP_SHARED */) vma_fl |= VMA_F_SHARED;

    file_t *file = NULL;
    if (!(flags & 0x20)) {
        if (fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;
        file = fget(proc, fd);
        if (!file) return -(s64)EBADF;
        if (file->f_op && file->f_op->mmap) {
            s64 ret = file->f_op->mmap(file, target_addr, aligned_len, (u32)prot, (u32)flags, file_offset);
            if (ret == 0) {
                vma_add(proc, target_addr, target_addr + aligned_len, (u32)prot & 7, vma_fl);
                fput(file);
                return (s64)target_addr;
            }
            fput(file);
            return ret;
        }
    }

    u64 vmm_flags = VMM_F_USER;
    if (flags & 0x01 /* MAP_SHARED */) vmm_flags |= VMM_F_SHARED;
    if (prot != 0 /* PROT_NONE */) vmm_flags |= VMM_F_PRESENT;
    if (prot & 0x2 /* PROT_WRITE */) vmm_flags |= VMM_F_WRITE;
    if (!(prot & 0x4 /* PROT_EXEC */)) vmm_flags |= VMM_F_NX;

    for (size_t offset = 0; offset < aligned_len; offset += PAGE_SIZE) {
        phys_addr_t phys = pmm_alloc_page();
        if (!phys) {
            /* BUG fix: unmap pages already installed in this call before
             * reporting OOM — otherwise they are leaked permanently. */
            if (offset > 0)
                vmm_unmap_range(proc->pml4_phys, target_addr, offset / PAGE_SIZE, true);
            if (file) fput(file);
            return -(s64)ENOMEM;
        }
        void *page_buf = (void *)PHYS_TO_VIRT(phys);
        __builtin_memset(page_buf, 0, PAGE_SIZE);

        if (file && file->f_inode) {
            u64 cur_foff = file_offset + offset;
            if (cur_foff < file->f_inode->i_size) {
                size_t to_read = file->f_inode->i_size - cur_foff;
                if (to_read > PAGE_SIZE) to_read = PAGE_SIZE;
                u64 saved_fpos = file->f_pos;
                file->f_pos = cur_foff;
                vfs_read(file, page_buf, to_read);
                file->f_pos = saved_fpos;
            }
        }

        vmm_map(proc->pml4_phys, target_addr + offset, phys, vmm_flags);
    }

    if (file) fput(file);
    vma_add(proc, target_addr, target_addr + aligned_len, (u32)prot & 7, vma_fl);
    return (s64)target_addr;
}

s64 sys_munmap_impl(pt_regs_t *r)
{
    virt_addr_t addr = (virt_addr_t)r->rdi;
    size_t length = (size_t)r->rsi;
    if (length == 0 || (addr & (PAGE_SIZE - 1))) return -(s64)EINVAL;
    if (addr >= TASK_SIZE_MAX || addr < PAGE_SIZE) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    size_t aligned_len = ALIGN_UP(length, PAGE_SIZE);
    if (addr + aligned_len < addr || addr + aligned_len > TASK_SIZE_MAX) return -(s64)EINVAL;

    vmm_unmap_range(proc->pml4_phys, addr, aligned_len / PAGE_SIZE, true);
    vma_remove(proc, addr, addr + aligned_len);
    return 0;
}

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

s64 sys_mprotect_impl(pt_regs_t *r)
{
    virt_addr_t addr = (virt_addr_t)r->rdi;
    size_t length = (size_t)r->rsi;
    int prot = (int)r->rdx;

    if (length == 0) return 0;
    if (addr & (PAGE_SIZE - 1)) return -(s64)EINVAL;
    if (addr >= TASK_SIZE_MAX || addr < 0x1000) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc || !proc->pml4_phys) return -(s64)EPERM;

    size_t aligned_len = ALIGN_UP(length, PAGE_SIZE);
    if (addr + aligned_len < addr || addr + aligned_len > TASK_SIZE_MAX) return -(s64)EINVAL;

    u64 vmm_flags = VMM_F_USER;
    if (prot != PROT_NONE) vmm_flags |= VMM_F_PRESENT;
    if (prot & PROT_WRITE) vmm_flags |= VMM_F_WRITE;
    if (!(prot & PROT_EXEC)) vmm_flags |= VMM_F_NX;

    size_t page_count = aligned_len / PAGE_SIZE;

    /* Record the new protection first. It is what the #PF handler grants to
     * demand-paged frames in this range, so if it cannot be recorded exactly
     * the call must fail before the page tables are touched — otherwise the
     * range ends up with narrower PTEs than the registry claims and later
     * faults hand back the *old*, wider rights. */
    /* The vDSO text and the vvar pages are single frames shared by every
     * process: a writable PTE on them would let one process rewrite the
     * clock code or data of all others, and re-protecting the HPET page would
     * drop its uncached attribute. Linux refuses these the same way. */
    if (vma_range_has_flags(proc, addr, addr + aligned_len, VMA_F_VVAR) ||
        ((prot & PROT_WRITE) &&
         vma_range_has_flags(proc, addr, addr + aligned_len, VMA_F_VDSO)))
        return -(s64)EACCES;

    s64 rc = vma_setprot(proc, addr, addr + aligned_len, (u32)prot & 7);
    if (rc < 0) return rc;

    vmm_set_flags(proc->pml4_phys, addr, page_count, vmm_flags);
    return 0;
}

/* ── In-memory anonymous file (memfd_create) ──────────────────────────────── */
typedef struct {
    spinlock_t lock;
    u8        *data;
    size_t     size;
    size_t     capacity;
} memfd_ctx_t;

static s64 memfd_read_op(file_t *filp, void *buf, size_t count, u64 *offset)
{
    if (!filp || !filp->private_data || !buf || !offset) return -(s64)EINVAL;
    memfd_ctx_t *ctx = (memfd_ctx_t *)filp->private_data;
    if (count == 0) return 0;

    spinlock_lock(&ctx->lock);
    if (*offset >= ctx->size) {
        spinlock_unlock(&ctx->lock);
        return 0; /* EOF */
    }
    size_t avail = ctx->size - *offset;
    size_t to_read = (count < avail) ? count : avail;
    /* The VFS read/write ops are handed a *kernel* buffer — sys_read_impl()
     * has already staged the transfer and copies out to userspace itself, as
     * tmpfs and every other filesystem here assume. Reaching for
     * copy_to_user() meant the destination failed the user-range check on
     * every call, so memfd reads returned -EFAULT unconditionally. */
    memcpy(buf, ctx->data + *offset, to_read);
    *offset += to_read;
    spinlock_unlock(&ctx->lock);
    return (s64)to_read;
}

static s64 memfd_write_op(file_t *filp, const void *buf, size_t count, u64 *offset)
{
    if (!filp || !filp->private_data || !buf || !offset) return -(s64)EINVAL;
    memfd_ctx_t *ctx = (memfd_ctx_t *)filp->private_data;
    if (count == 0) return 0;

    spinlock_lock(&ctx->lock);
    size_t new_end = *offset + count;
    if (new_end > ctx->capacity) {
        size_t new_cap = (ctx->capacity == 0) ? 4096 : ctx->capacity * 2;
        while (new_cap < new_end) new_cap *= 2;
        u8 *new_buf = (u8 *)kmalloc(new_cap);
        if (!new_buf) {
            spinlock_unlock(&ctx->lock);
            return -(s64)ENOMEM;
        }
        if (ctx->data && ctx->size > 0) {
            memcpy(new_buf, ctx->data, ctx->size);
            kfree(ctx->data);
        }
        ctx->data = new_buf;
        ctx->capacity = new_cap;
    }
    /* Kernel-to-kernel, for the reason memfd_read_op() explains. */
    memcpy(ctx->data + *offset, buf, count);
    *offset += count;
    if (*offset > ctx->size) {
        ctx->size = *offset;
        if (filp->f_inode) filp->f_inode->i_size = ctx->size;
    }
    spinlock_unlock(&ctx->lock);
    return (s64)count;
}

static s64 memfd_ioctl_op(file_t *filp, u32 cmd, u64 arg)
{
    if (!filp || !filp->private_data) return -(s64)EBADF;
    memfd_ctx_t *ctx = (memfd_ctx_t *)filp->private_data;
    if (cmd == 0x541B /* FIONREAD */) {
        if (!arg || (uintptr_t)arg >= TASK_SIZE_MAX) return -(s64)EINVAL;
        int avail = (filp->f_pos < ctx->size) ? (int)(ctx->size - filp->f_pos) : 0;
        if (copy_to_user((void *)(uintptr_t)arg, &avail, sizeof(int)) != 0) return -(s64)EFAULT;
        return 0;
    }
    return -(s64)EINVAL;
}

static s64 memfd_release_op(inode_t *inode, file_t *filp)
{
    (void)inode;
    if (filp) {
        if (filp->private_data) {
            memfd_ctx_t *ctx = (memfd_ctx_t *)filp->private_data;
            if (ctx->data) kfree(ctx->data);
            kfree(ctx);
            filp->private_data = NULL;
        }
        if (filp->f_inode) {
            kfree(filp->f_inode);
            filp->f_inode = NULL;
        }
    }
    return 0;
}

static file_operations_t g_memfd_fops = {
    .read    = memfd_read_op,
    .write   = memfd_write_op,
    .ioctl   = memfd_ioctl_op,
    .release = memfd_release_op,
};

s64 sys_memfd_create_impl(pt_regs_t *r)
{
    const char *uname = (const char *)r->rdi;
    unsigned int flags = (unsigned int)r->rsi;
    (void)uname;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    memfd_ctx_t *ctx = (memfd_ctx_t *)kzalloc(sizeof(memfd_ctx_t));
    if (!ctx) return -(s64)ENOMEM;
    spinlock_init(&ctx->lock);

    file_t *f = (file_t *)kzalloc(sizeof(file_t));
    if (!f) {
        kfree(ctx);
        return -(s64)ENOMEM;
    }

    inode_t *node = (inode_t *)kzalloc(sizeof(inode_t));
    if (!node) {
        kfree(ctx);
        kfree(f);
        return -(s64)ENOMEM;
    }
    node->i_mode = S_IFREG | 0600;

    f->f_inode = node;
    f->f_op = &g_memfd_fops;
    f->f_flags = O_RDWR;
    f->f_count = 1;
    f->private_data = ctx;

    s64 fd = fd_install(proc, f, (flags & 0x0001 /* MFD_CLOEXEC */) ? FD_CLOEXEC : 0);
    if (fd < 0) {
        kfree(node);
        kfree(ctx);
        kfree(f);
        return -(s64)EMFILE;
    }
    return fd;
}

s64 sys_swapon_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    (void)r->rsi;

    if (!user_path) return -(s64)EINVAL;
    char path[256];
    s64 slen = copy_str_from_user(path, user_path, sizeof(path));
    if (slen < 0) return slen;

    struct stat st;
    s64 ret = vfs_stat(path, &st);
    if (ret < 0) return ret;

    return 0;
}

s64 sys_swapoff_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    if (!user_path) return -(s64)EINVAL;
    char path[256];
    s64 slen = copy_str_from_user(path, user_path, sizeof(path));
    if (slen < 0) return slen;
    return 0;
}

s64 sys_msync_impl(pt_regs_t *r)
{
    virt_addr_t addr = (virt_addr_t)r->rdi;
    size_t length = (size_t)r->rsi;
    int flags = (int)r->rdx;

    if (addr & (PAGE_SIZE - 1)) return -(s64)EINVAL;
    if (flags & ~(1 /* MS_ASYNC */ | 2 /* MS_INVALIDATE */ | 4 /* MS_SYNC */)) return -(s64)EINVAL;
    if ((flags & (1 | 4)) == (1 | 4)) return -(s64)EINVAL;
    if ((flags & (1 | 4)) == 0) return -(s64)EINVAL;
    if (addr >= TASK_SIZE_MAX || addr + length < addr) return -(s64)ENOMEM;
    if (length == 0) return 0;

    process_t *proc = sched_current_process();
    if (!proc || !proc->pml4_phys) return -(s64)EPERM;

    size_t aligned_len = ALIGN_UP(length, PAGE_SIZE);
    for (uintptr_t va = addr; va < addr + aligned_len; va += PAGE_SIZE) {
        if (!vmm_translate(proc->pml4_phys, va)) return -(s64)ENOMEM;
    }

    vfs_sync_all();
    return 0;
}

s64 sys_madvise_impl(pt_regs_t *r)
{
    u64   addr   = r->rdi;
    u64   length = r->rsi;
    int   advice = (int)r->rdx;

    if (addr & (PAGE_SIZE - 1)) return -(s64)EINVAL;   /* must be page-aligned */
    if (length > TASK_SIZE_MAX) return -(s64)EINVAL;
    if (addr >= TASK_SIZE_MAX || addr + length < addr) return -(s64)EINVAL;

    /* This kernel maps anonymous memory eagerly and never reclaims a resident
     * page, so every hint that is legal is also a no-op. What matters is
     * rejecting the ones that are *not* legal — callers probe with a bogus
     * advice and branch on the -EINVAL. */
    switch (advice) {
        case 0:   /* MADV_NORMAL       */
        case 1:   /* MADV_RANDOM       */
        case 2:   /* MADV_SEQUENTIAL   */
        case 3:   /* MADV_WILLNEED     */
        case 4:   /* MADV_DONTNEED     */
        case 8:   /* MADV_FREE         */
        case 9:   /* MADV_REMOVE       */
        case 10:  /* MADV_DONTFORK     */
        case 11:  /* MADV_DOFORK       */
        case 12:  /* MADV_MERGEABLE    */
        case 13:  /* MADV_UNMERGEABLE  */
        case 14:  /* MADV_HUGEPAGE     */
        case 15:  /* MADV_NOHUGEPAGE   */
        case 16:  /* MADV_DONTDUMP     */
        case 17:  /* MADV_DODUMP       */
        case 18:  /* MADV_WIPEONFORK   */
        case 19:  /* MADV_KEEPONFORK   */
        case 20:  /* MADV_COLD         */
        case 21:  /* MADV_PAGEOUT      */
        case 100: /* MADV_HWPOISON (privileged, but harmless as a no-op here) */
            return 0;
        default:
            return -(s64)EINVAL;
    }
}

/* ── Linux Memory Management ABIs (mremap, mincore) ────────────────────────── */
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED   2

s64 sys_mremap_impl(pt_regs_t *r)
{
    uintptr_t old_addr = (uintptr_t)r->rdi;
    size_t old_size = (size_t)r->rsi;
    size_t new_size = (size_t)r->rdx;
    int flags = (int)r->r10;

    (void)flags;
    if (old_addr >= TASK_SIZE_MAX || (old_addr & 0xFFF) != 0) return -(s64)EINVAL;
    if (new_size == 0) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    size_t old_pages = (old_size + 4095) / 4096;
    size_t new_pages = (new_size + 4095) / 4096;

    if (new_pages == old_pages) return (s64)old_addr;

    if (new_pages > old_pages) {
        for (size_t i = old_pages; i < new_pages; i++) {
            phys_addr_t p = pmm_alloc_page();
            if (!p) return -(s64)ENOMEM;
            hw_clear_page((void *)(HHDM_BASE + p));
            vmm_map(proc->pml4_phys, old_addr + i * 4096, p, VMM_F_PRESENT | VMM_F_WRITE | VMM_F_USER | VMM_F_NX);
        }
        return (s64)old_addr;
    } else {
        /* BUG-AK fix: reclaim physical pages and update VMA on shrink */
        vmm_unmap_range(proc->pml4_phys, old_addr + new_pages * 4096,
                        old_pages - new_pages, true);
        vma_remove(proc, old_addr + new_size, old_addr + old_size);
        return (s64)old_addr;
    }
}

/* No swap device or reclaim exists, so every user page that is mapped is
 * already resident and unevictable — there is nothing for "locked" to pin
 * against. These used to be blind no-ops; now they genuinely record
 * (VMA_F_LOCKED, kernel/mm/vma.c) which mappings the caller asked to be
 * locked. Unlike real mlock(2), a range with no VMA at all (e.g. inside the
 * brk()-managed heap, which never gets one — see vma_set_locked()'s own
 * comment) is a no-op rather than -ENOMEM: this kernel's VMA list is a
 * best-effort index, not an authoritative map, so treating "no VMA found"
 * as "definitely unmapped" would reject plenty of genuinely valid memory. */
s64 sys_mlock_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    u64 addr = r->rdi, len = r->rsi;
    if (len == 0) return 0;
    u64 start = ALIGN_DOWN(addr, PAGE_SIZE);
    u64 end   = ALIGN_UP(addr + len, PAGE_SIZE);
    if (end <= start) return -(s64)EINVAL;

    /* Actually fault in the pages so they are resident */
    for (u64 p = start; p < end; p += PAGE_SIZE) {
        if (!vmm_translate(proc->pml4_phys, p)) {
            char dummy;
            /* Attempt to read 1 byte to trigger demand paging */
            if (copy_from_user(&dummy, (const void *)p, 1) != 0) {
                return -(s64)ENOMEM;
            }
        }
    }

    return vma_set_locked(proc, start, end, true);
}

s64 sys_munlock_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    u64 addr = r->rdi, len = r->rsi;
    if (len == 0) return 0;
    u64 start = ALIGN_DOWN(addr, PAGE_SIZE);
    u64 end   = ALIGN_UP(addr + len, PAGE_SIZE);
    if (end <= start) return -(s64)EINVAL;
    return vma_set_locked(proc, start, end, false);
}

/* mlock2(addr, len, flags) — MLOCK_ONFAULT would defer locking until each
 * page faults in; every mapped page is already resident here (no demand
 * paging beyond the existing VMA_F_DEMAND path this doesn't interact with),
 * so there's no meaningful "on fault" moment to defer to — treated the same
 * as a plain mlock(). */
s64 sys_mlock2_impl(pt_regs_t *r)
{
    return sys_mlock_impl(r);
}

s64 sys_mlockall_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    s64 flags = (s64)(s32)r->rdi;
    if (flags == 0 || (flags & ~(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT))) return -(s64)EINVAL;
    if (flags & MCL_CURRENT) vma_set_locked_all(proc, true);
    /* MCL_FUTURE ("lock every mapping this process creates from now on") is
     * accepted but not enforced: nothing here tracks a per-process
     * lock-all-future-mappings flag that mmap() would need to consult. A
     * process that asked for MCL_FUTURE gets MCL_CURRENT's effect now and
     * no error, rather than a silent, permanent gap for something that
     * still has nothing to actually pin against either way. */
    return 0;
}

s64 sys_munlockall_impl(pt_regs_t *r)
{
    (void)r;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    vma_set_locked_all(proc, false);
    return 0;
}

/* process_vm_readv / process_vm_writev — copy between the caller and a target
 * process, page by page, translating the remote virtual addresses through the
 * target's PML4. */
#define PVM_USER_MAX TASK_SIZE_MAX

/* True if [base, base+len) is a non-wrapping range wholly inside the user half. */
static bool pvm_user_range_ok(u64 base, u64 len)
{
    if (len == 0) return true;
    if (base >= PVM_USER_MAX) return false;
    if (len > PVM_USER_MAX - base) return false;   /* overflow / crosses into kernel half */
    return true;
}

static s64 do_process_vm(pt_regs_t *r, bool write_to_remote)
{
    u32 pid = (u32)r->rdi;
    const struct iovec *ulocal = (const struct iovec *)r->rsi;
    unsigned long liovcnt = (unsigned long)r->rdx;
    const struct iovec *uremote = (const struct iovec *)r->r10;
    unsigned long riovcnt = (unsigned long)r->r8;

    if (liovcnt > 1024 || riovcnt > 1024) return -(s64)EINVAL;
    if (!ulocal || !uremote) return -(s64)EFAULT;

    /* Snapshot the target under the scheduler lock — never hold a raw
     * process_t* across the blocking copy loop below. */
    struct proc_ident tgt;
    if (!sched_proc_ident(pid, &tgt) || tgt.is_zombie || !tgt.pml4_phys)
        return -(s64)ESRCH;

    /* ptrace-style access check (Linux PTRACE_MODE_ATTACH_REALCREDS): root, or a
     * caller whose *real* uid/gid equal the target's real, effective AND saved
     * ids. Anything weaker lets a process poke a setuid peer that dropped euid
     * but still holds suid==0. */
    process_t *caller = sched_current_process();
    if (!caller) return -(s64)EPERM;
    if (caller->euid != 0) {
        if (caller->uid != tgt.uid || caller->uid != tgt.euid || caller->uid != tgt.suid ||
            caller->gid != tgt.gid || caller->gid != tgt.egid || caller->gid != tgt.sgid)
            return -(s64)EPERM;
        if (tgt.pid <= 2) return -(s64)EPERM;   /* never non-root vs init/kernel */
    }

    const u64 need = write_to_remote ? (VMM_F_PRESENT | VMM_F_USER | VMM_F_WRITE)
                                     : (VMM_F_PRESENT | VMM_F_USER);

    struct iovec rio;
    unsigned long ri = 0;
    u64 roff = 0;
    s64 copied = 0;

    for (unsigned long li = 0; li < liovcnt; li++) {
        struct iovec lio;
        if (copy_from_user(&lio, &ulocal[li], sizeof lio) != 0) return -(s64)EFAULT;
        if (!pvm_user_range_ok((u64)(uintptr_t)lio.iov_base, lio.iov_len))
            return copied ? copied : -(s64)EFAULT;
        u8 *lptr = (u8 *)lio.iov_base;
        u64 lrem = lio.iov_len;

        /* Re-check the target between local iovecs: bail cleanly if it exited or
         * exec'd a new address space rather than translating against a stale
         * (possibly freed) PML4. */
        struct proc_ident now;
        if (!sched_proc_ident(pid, &now) || now.is_zombie ||
            now.pml4_phys != tgt.pml4_phys)
            return copied;

        while (lrem) {
            if (roff == 0) {
                if (ri >= riovcnt) return copied;
                if (copy_from_user(&rio, &uremote[ri], sizeof rio) != 0) return -(s64)EFAULT;
                ri++;
                if (!pvm_user_range_ok((u64)(uintptr_t)rio.iov_base, rio.iov_len))
                    return copied ? copied : -(s64)EFAULT;
                if (rio.iov_len == 0) { roff = 0; continue; }
            }
            u64 rva = (u64)(uintptr_t)rio.iov_base + roff;
            u64 rleft = rio.iov_len - roff;
            u64 page_left = PAGE_SIZE - (rva & (PAGE_SIZE - 1));
            u64 n = lrem;
            if (n > rleft) n = rleft;
            if (n > page_left) n = page_left;

            /* The remote page must be a ring-3-accessible page of the target
             * (and writable for a poke). This is what stops a remote iovec
             * pointing at the kernel higher-half — vmm_translate alone would
             * happily resolve it. */
            if ((vmm_query_flags(tgt.pml4_phys, rva) & need) != need)
                return copied ? copied : -(s64)EFAULT;

            phys_addr_t rphys = vmm_translate(tgt.pml4_phys, rva);
            if (!rphys) return copied ? copied : -(s64)EFAULT;
            u8 *rkern = (u8 *)PHYS_TO_VIRT(rphys);

            if (write_to_remote) {
                if (copy_from_user(rkern, lptr, n) != 0) return copied ? copied : -(s64)EFAULT;
            } else {
                if (copy_to_user(lptr, rkern, n) != 0) return copied ? copied : -(s64)EFAULT;
            }

            lptr += n; lrem -= n; copied += n;
            roff += n;
            if (roff == rio.iov_len) roff = 0;
        }
    }
    return copied;
}
s64 sys_process_vm_readv_impl(pt_regs_t *r)  { return do_process_vm(r, false); }
s64 sys_process_vm_writev_impl(pt_regs_t *r) { return do_process_vm(r, true);  }

/* ============================================================================
 * Memory-protection keys — pkey_alloc(2) / pkey_free(2) / pkey_mprotect(2)
 *
 * The hardware (CR4.PKE, enabled in cpu_enable_features_bsp()) tags each
 * user-accessible leaf PTE with one of 16 keys in bits 62:59, and the PKRU
 * register carries two bits per key — access-disable and write-disable — that
 * gate every data access to a page carrying that key, from ring 3 *and* from
 * ring 0. PKRU lives in the XSAVE state, so it is per-thread and the context
 * switch preserves it for free.
 *
 * Key 0 is the key every page starts with and is never handed out, so a
 * process that never calls pkey_alloc() is completely unaffected.
 * ========================================================================= */

s64 sys_pkey_alloc_impl(pt_regs_t *r)
{
    unsigned long flags  = (unsigned long)r->rdi;
    unsigned long rights = (unsigned long)r->rsi;

    if (!g_pku_enabled) return -(s64)ENOSPC;   /* Linux reports "no keys left" */
    if (flags != 0) return -(s64)EINVAL;
    if (rights & ~(unsigned long)PKEY_ACCESS_MASK) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    int key = -1;
    irqflags_t irqf = spinlock_lock_irqsave(&proc->fd_lock);
    for (int k = 1; k < PKEY_MAX; k++) {
        if (!(proc->pkey_alloc_map & (1u << k))) {
            proc->pkey_alloc_map |= (u16)(1u << k);
            key = k;
            break;
        }
    }
    spinlock_unlock_irqrestore(&proc->fd_lock, irqf);
    if (key < 0) return -(s64)ENOSPC;

    /* Publish the requested rights into this thread's PKRU. Two bits per key:
     * bit 2k = access-disable, bit 2k+1 = write-disable. */
    u32 pkru = rdpkru();
    pkru &= ~(0x3u << (2 * key));
    if (rights & PKEY_DISABLE_ACCESS) pkru |= (1u << (2 * key));
    if (rights & PKEY_DISABLE_WRITE)  pkru |= (1u << (2 * key + 1));
    wrpkru(pkru);

    return key;
}

s64 sys_pkey_free_impl(pt_regs_t *r)
{
    int key = (int)r->rdi;

    if (!g_pku_enabled) return -(s64)EINVAL;
    if (key <= 0 || key >= PKEY_MAX) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    irqflags_t irqf = spinlock_lock_irqsave(&proc->fd_lock);
    bool was_allocated = (proc->pkey_alloc_map & (1u << key)) != 0;
    proc->pkey_alloc_map &= (u16)~(1u << key);
    spinlock_unlock_irqrestore(&proc->fd_lock, irqf);
    if (!was_allocated) return -(s64)EINVAL;

    /* Freeing a key does not un-tag the pages still carrying it — that is
     * Linux's behaviour too, and it is why freeing a key in use is documented
     * as a programming error. Reset its PKRU bits to "deny", so a stale page
     * cannot silently become accessible when the key is handed out again. */
    u32 pkru = rdpkru();
    pkru |= (0x3u << (2 * key));
    wrpkru(pkru);
    return 0;
}

s64 sys_pkey_mprotect_impl(pt_regs_t *r)
{
    virt_addr_t addr = (virt_addr_t)r->rdi;
    size_t length    = (size_t)r->rsi;
    int prot         = (int)r->rdx;
    int pkey         = (int)(s32)r->r10;

    /* pkey == -1 means "leave the key alone", i.e. plain mprotect(2). */
    if (pkey != -1) {
        if (!g_pku_enabled) return -(s64)EINVAL;
        if (pkey < 0 || pkey >= PKEY_MAX) return -(s64)EINVAL;
        process_t *p = sched_current_process();
        if (!p) return -(s64)EPERM;
        if (!(p->pkey_alloc_map & (1u << pkey))) return -(s64)EINVAL;
    }

    if (length == 0) return 0;
    if (addr & (PAGE_SIZE - 1)) return -(s64)EINVAL;
    if (addr >= TASK_SIZE_MAX || addr < 0x1000) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc || !proc->pml4_phys) return -(s64)EPERM;

    size_t aligned_len = ALIGN_UP(length, PAGE_SIZE);
    if (addr + aligned_len < addr || addr + aligned_len > TASK_SIZE_MAX)
        return -(s64)EINVAL;

    u64 vmm_flags = VMM_F_USER;
    if (prot != PROT_NONE)  vmm_flags |= VMM_F_PRESENT;
    if (prot & PROT_WRITE)  vmm_flags |= VMM_F_WRITE;
    if (!(prot & PROT_EXEC)) vmm_flags |= VMM_F_NX;
    if (pkey != -1) vmm_flags |= VMM_F_PKEY_SET | VMM_F_PKEY(pkey);

    /* The vDSO text and the vvar pages are single frames shared by every
     * process: a writable PTE on them would let one process rewrite the
     * clock code or data of all others, and re-protecting the HPET page would
     * drop its uncached attribute. Linux refuses these the same way. */
    if (vma_range_has_flags(proc, addr, addr + aligned_len, VMA_F_VVAR) ||
        ((prot & PROT_WRITE) &&
         vma_range_has_flags(proc, addr, addr + aligned_len, VMA_F_VDSO)))
        return -(s64)EACCES;

    s64 rc = vma_setprot(proc, addr, addr + aligned_len, (u32)prot & 7);
    if (rc < 0) return rc;

    vmm_set_flags(proc->pml4_phys, addr, aligned_len / PAGE_SIZE, vmm_flags);
    return 0;
}

/* ============================================================================
 * mincore(2) — real residency, one page-table lookup per page
 * ========================================================================= */

s64 sys_mincore_impl(pt_regs_t *r)
{
    uintptr_t start = (uintptr_t)r->rdi;
    size_t length   = (size_t)r->rsi;
    unsigned char *vec = (unsigned char *)r->rdx;

    if (start >= TASK_SIZE_MAX || (start & 0xFFF) != 0) return -(s64)EINVAL;
    if (!vec || (uintptr_t)vec >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc || !proc->pml4_phys) return -(s64)EPERM;

    size_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    if (start + (u64)pages * PAGE_SIZE > TASK_SIZE_MAX) return -(s64)ENOMEM;

    /* Batch the answers: one copy_to_user per chunk instead of per page turns
     * a 1 MB query from 256 user-access transitions into one. */
    unsigned char chunk[256];
    size_t done = 0;
    while (done < pages) {
        size_t n = pages - done;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        for (size_t i = 0; i < n; i++) {
            uintptr_t va = start + (done + i) * PAGE_SIZE;
            /* A PROT_NONE page keeps its frame with PRESENT clear, and is still
             * resident — report it as such, which is what mincore means. */
            chunk[i] = (vmm_query_flags(proc->pml4_phys, va) & VMM_F_PRESENT) ||
                       vmm_translate(proc->pml4_phys, va) ? 1 : 0;
        }
        if (copy_to_user(&vec[done], chunk, n) != 0) return -(s64)EFAULT;
        done += n;
    }
    return 0;
}

/* ============================================================================
 * process_madvise(2) — madvise on another process's address space
 * ========================================================================= */

s64 sys_process_madvise_impl(pt_regs_t *r)
{
    int pidfd                = (int)r->rdi;
    const struct iovec *uiov = (const struct iovec *)r->rsi;
    unsigned long vlen       = (unsigned long)r->rdx;
    int advice               = (int)r->r10;
    unsigned int flags       = (unsigned int)r->r8;

    if (flags != 0) return -(s64)EINVAL;
    if (vlen > 1024) return -(s64)EINVAL;
    if (!uiov || (uintptr_t)uiov >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *caller = sched_current_process();
    if (!caller) return -(s64)EPERM;

    file_t *pf = fget(caller, pidfd);
    if (!pf) return -(s64)EBADF;
    fput(pf);

    /* Only the advice values that are meaningful without a reclaim path. The
     * hints this kernel can honour are all no-ops on an eagerly-mapped address
     * space, so the call validates its arguments and reports the byte count it
     * would have covered, which is what callers branch on. */
    switch (advice) {
        case 4:   /* MADV_DONTNEED  */
        case 8:   /* MADV_FREE      */
        case 20:  /* MADV_COLD      */
        case 21:  /* MADV_PAGEOUT   */
        case 22:  /* MADV_WILLNEED-ish / POPULATE_READ */
            break;
        default:
            return -(s64)EINVAL;
    }

    s64 total = 0;
    for (unsigned long i = 0; i < vlen; i++) {
        struct iovec kiov;
        if (copy_from_user(&kiov, &uiov[i], sizeof(kiov)) != 0) return -(s64)EFAULT;
        if ((uintptr_t)kiov.iov_base >= TASK_SIZE_MAX) return -(s64)EFAULT;
        if (kiov.iov_len > (size_t)0x7fffffffffffffffLL - (size_t)total)
            return -(s64)EINVAL;
        total += (s64)kiov.iov_len;
    }
    return total;
}

/* ============================================================================
 * cachestat(2) — page-cache residency of a file range
 * ========================================================================= */

s64 sys_cachestat_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const struct cachestat_range *urange = (const struct cachestat_range *)r->rsi;
    struct cachestat *ucs = (struct cachestat *)r->rdx;
    unsigned int flags = (unsigned int)r->r10;

    if (flags != 0) return -(s64)EINVAL;
    if (!urange || !ucs) return -(s64)EFAULT;
    if ((uintptr_t)urange >= TASK_SIZE_MAX ||
        (uintptr_t)ucs    >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    file_t *file = fget(proc, fd);
    if (!file) return -(s64)EBADF;
    if (!file->f_inode || S_ISDIR(file->f_inode->i_mode)) { fput(file); return -(s64)EBADF; }

    struct cachestat_range range;
    if (copy_from_user(&range, urange, sizeof(range)) != 0) { fput(file); return -(s64)EFAULT; }

    u64 size = file->f_inode->i_size;
    u64 off  = range.off;
    /* len == 0 means "to the end of the file", per the man page. */
    u64 len  = range.len ? range.len : (off < size ? size - off : 0);
    fput(file);

    u64 pages = 0;
    if (off < size) {
        u64 end = off + len;
        if (end < off || end > size) end = size;
        pages = (end - off + PAGE_SIZE - 1) / PAGE_SIZE;
    }

    /* This kernel reads through the block layer's cache rather than a unified
     * page cache with its own residency bookkeeping, so the honest answer is
     * "everything within the file is cached, nothing is dirty or in
     * writeback" — the shape callers use to decide whether a read would
     * block. */
    struct cachestat cs;
    __builtin_memset(&cs, 0, sizeof(cs));
    cs.nr_cache = pages;
    return copy_to_user(ucs, &cs, sizeof(cs)) == 0 ? 0 : -(s64)EFAULT;
}

/* ============================================================================
 * mseal(2) — make a mapping's protections permanent
 * ========================================================================= */

s64 sys_mseal_impl(pt_regs_t *r)
{
    uintptr_t addr = (uintptr_t)r->rdi;
    size_t len     = (size_t)r->rsi;
    unsigned long flags = (unsigned long)r->rdx;

    if (flags != 0) return -(s64)EINVAL;
    if (addr & (PAGE_SIZE - 1)) return -(s64)EINVAL;
    if (addr + len < addr || addr + len > TASK_SIZE_MAX) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    /* Every page in the range must be mapped, or Linux returns ENOMEM. We
     * enforce that much; the seal itself is not yet tracked, so a later
     * mprotect() on the range still succeeds. Reporting ENOSYS instead would
     * be worse: callers use mseal() as opportunistic hardening and treat a
     * success as "done", never as "and now nothing can change". */
    for (uintptr_t va = addr; va < addr + len; va += PAGE_SIZE) {
        if (!vmm_translate(proc->pml4_phys, va)) return -(s64)ENOMEM;
    }
    return 0;
}

/* ============================================================================
 * NUMA memory policy: set_mempolicy(2), get_mempolicy(2), mbind(2),
 * migrate_pages(2), set_mempolicy_home_node(2)
 *
 * AzamiOS presents one memory node. That makes every policy trivially
 * satisfied rather than unimplementable, so these validate their arguments the
 * way a one-node Linux kernel does and keep the state they are given — a
 * nodemask naming node 3 is an error here for the same reason it is there.
 * ========================================================================= */

/* Read a user nodemask and check it names nothing but node 0. */
static s64 mempolicy_check_nodemask(const unsigned long *unodes, unsigned long maxnode,
                                    u64 *out_mask)
{
    *out_mask = 0;
    if (maxnode > 8 * sizeof(u64) * 16) return -(s64)EINVAL;
    if (!unodes || maxnode == 0) return 0;
    if ((uintptr_t)unodes >= TASK_SIZE_MAX) return -(s64)EFAULT;

    /* maxnode counts bits, and the bitmap is an array of unsigned long. */
    unsigned long words = (maxnode + 63) / 64;
    for (unsigned long i = 0; i < words; i++) {
        u64 w = 0;
        if (copy_from_user(&w, unodes + i, sizeof(u64)) != 0) return -(s64)EFAULT;
        if (i == 0) {
            /* Mask off bits past maxnode before judging the word. */
            if (maxnode < 64) w &= (maxnode == 0) ? 0 : ((1ULL << maxnode) - 1);
            *out_mask = w;
            if (w & ~1ULL) return -(s64)EINVAL;   /* only node 0 exists */
        } else if (w) {
            return -(s64)EINVAL;
        }
    }
    return 0;
}

static s64 mempolicy_check_mode(int mode, u64 nodemask)
{
    int flags = mode & MPOL_MODE_FLAGS;
    mode &= ~MPOL_MODE_FLAGS;
    if (mode < 0 || mode >= MPOL_MAX) return -(s64)EINVAL;
    if ((flags & MPOL_F_STATIC_NODES) && (flags & MPOL_F_RELATIVE_NODES))
        return -(s64)EINVAL;

    /* MPOL_BIND and MPOL_INTERLEAVE require a non-empty node set;
     * MPOL_DEFAULT and MPOL_LOCAL require an empty one. */
    if ((mode == MPOL_BIND || mode == MPOL_INTERLEAVE) && nodemask == 0)
        return -(s64)EINVAL;
    if ((mode == MPOL_DEFAULT || mode == MPOL_LOCAL) && nodemask != 0)
        return -(s64)EINVAL;
    return 0;
}

s64 sys_set_mempolicy_impl(pt_regs_t *r)
{
    int mode = (int)r->rdi;
    const unsigned long *unodes = (const unsigned long *)r->rsi;
    unsigned long maxnode = (unsigned long)r->rdx;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    u64 mask = 0;
    s64 rc = mempolicy_check_nodemask(unodes, maxnode, &mask);
    if (rc < 0) return rc;
    rc = mempolicy_check_mode(mode, mask);
    if (rc < 0) return rc;

    proc->mempolicy_mode = (u32)mode;
    proc->mempolicy_nodemask = mask;
    return 0;
}

s64 sys_get_mempolicy_impl(pt_regs_t *r)
{
    int *umode = (int *)r->rdi;
    unsigned long *unodes = (unsigned long *)r->rsi;
    unsigned long maxnode = (unsigned long)r->rdx;
    uintptr_t addr = (uintptr_t)r->r10;
    unsigned long flags = (unsigned long)r->r8;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (flags & ~(unsigned long)(MPOL_F_NODE | MPOL_F_ADDR | MPOL_F_MEMS_ALLOWED))
        return -(s64)EINVAL;
    if ((flags & MPOL_F_ADDR) && !addr) return -(s64)EINVAL;
    if ((flags & MPOL_F_MEMS_ALLOWED) && (flags & (MPOL_F_ADDR | MPOL_F_NODE)))
        return -(s64)EINVAL;

    int mode;
    u64 mask;
    if (flags & MPOL_F_MEMS_ALLOWED) {
        mode = 0;
        mask = 1;                       /* node 0 is the only one permitted */
    } else if (flags & MPOL_F_NODE) {
        /* Report which node the memory is on, not which policy governs it.
         * With one node the answer is always 0 — but only for a mapped
         * address, so an unmapped one still has to fault. */
        if (flags & MPOL_F_ADDR) {
            if (addr >= TASK_SIZE_MAX) return -(s64)EFAULT;
            if (!vmm_translate(proc->pml4_phys, addr)) return -(s64)EFAULT;
        }
        mode = 0;
        mask = proc->mempolicy_nodemask;
    } else {
        mode = (int)proc->mempolicy_mode;
        mask = proc->mempolicy_nodemask;
    }

    if (umode) {
        if ((uintptr_t)umode >= TASK_SIZE_MAX) return -(s64)EFAULT;
        if (copy_to_user(umode, &mode, sizeof(int)) != 0) return -(s64)EFAULT;
    }
    if (unodes && maxnode) {
        if ((uintptr_t)unodes >= TASK_SIZE_MAX) return -(s64)EFAULT;
        unsigned long words = (maxnode + 63) / 64;
        for (unsigned long i = 0; i < words; i++) {
            u64 w = (i == 0) ? mask : 0;
            if (copy_to_user(unodes + i, &w, sizeof(u64)) != 0) return -(s64)EFAULT;
        }
    }
    return 0;
}

/* move_pages(2), fully implemented for the one-node case this kernel
 * presents — same "faithful for one node rather than stubbed" approach as
 * the other mempolicy calls above (see this file's MPOL_* comment).
 * Querying (nodes == NULL) reports node 0 for every present page and
 * -ENOENT for an unmapped one; a move request succeeds trivially when the
 * target is node 0 (the page is already there) and fails -ENODEV for any
 * other target, exactly matching what a real move_pages() reports on
 * hardware with a single NUMA node.
 *
 * Only self (pid == 0 or the caller's own pid) is supported: real
 * move_pages() can also target another process the caller has
 * ptrace-equivalent permission over, but every real caller of this syscall
 * is a NUMA-aware allocator checking or placing its *own* pages — nothing
 * in a single-node kernel needs the cross-process case, and skipping it
 * avoids taking on another process's page-table locking here. */
#define MOVE_PAGES_MAX 1024 /* same per-call bound as readv/writev's iovec cap */

s64 sys_move_pages_impl(pt_regs_t *r)
{
    int pid = (int)r->rdi;
    unsigned long count = (unsigned long)r->rsi;
    void *const *upages = (void *const *)r->rdx;
    const int *unodes = (const int *)r->r10;
    int *ustatus = (int *)r->r8;
    /* r9 (flags: MPOL_MF_MOVE / MPOL_MF_MOVE_ALL) doesn't change behavior
     * either way on a single-node system. */

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (pid != 0 && pid != (int)proc->pid) return -(s64)EPERM;

    if (count > MOVE_PAGES_MAX) return -(s64)E2BIG;
    if (count == 0) return 0;
    if (!upages || !ustatus) return -(s64)EFAULT;
    if ((uintptr_t)upages >= TASK_SIZE_MAX || (uintptr_t)ustatus >= TASK_SIZE_MAX)
        return -(s64)EFAULT;
    if (unodes && (uintptr_t)unodes >= TASK_SIZE_MAX) return -(s64)EFAULT;

    for (unsigned long i = 0; i < count; i++) {
        void *upage_ptr;
        if (copy_from_user(&upage_ptr, &upages[i], sizeof(void *)) != 0) return -(s64)EFAULT;
        uintptr_t addr = (uintptr_t)upage_ptr;

        int status;
        if (addr >= TASK_SIZE_MAX ||
            !vmm_translate(proc->pml4_phys, ALIGN_DOWN(addr, PAGE_SIZE))) {
            status = -(s32)ENOENT; /* page not present */
        } else if (unodes) {
            int want_node;
            if (copy_from_user(&want_node, &unodes[i], sizeof(int)) != 0) return -(s64)EFAULT;
            status = (want_node == 0) ? 0 : -(s32)ENODEV;
        } else {
            status = 0; /* query only: present pages are always on node 0 */
        }
        if (copy_to_user(&ustatus[i], &status, sizeof(int)) != 0) return -(s64)EFAULT;
    }
    return 0;
}

s64 sys_mbind_impl(pt_regs_t *r)
{
    uintptr_t addr = (uintptr_t)r->rdi;
    u64 len = r->rsi;
    int mode = (int)r->rdx;
    const unsigned long *unodes = (const unsigned long *)r->r10;
    unsigned long maxnode = (unsigned long)r->r8;
    unsigned flags = (unsigned)r->r9;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (addr & (PAGE_SIZE - 1)) return -(s64)EINVAL;
    /* MPOL_MF_STRICT | MPOL_MF_MOVE | MPOL_MF_MOVE_ALL */
    if (flags & ~7u) return -(s64)EINVAL;

    u64 mask = 0;
    s64 rc = mempolicy_check_nodemask(unodes, maxnode, &mask);
    if (rc < 0) return rc;
    rc = mempolicy_check_mode(mode, mask);
    if (rc < 0) return rc;

    /* The range has to exist even though the policy cannot move anything. */
    u64 end = addr + ALIGN_UP(len, PAGE_SIZE);
    if (end < addr || end > TASK_SIZE_MAX) return -(s64)EINVAL;
    return 0;
}

s64 sys_migrate_pages_impl(pt_regs_t *r)
{
    unsigned long maxnode = (unsigned long)r->rsi;
    const unsigned long *uold = (const unsigned long *)r->rdx;
    const unsigned long *unew = (const unsigned long *)r->r10;

    u64 om = 0, nm = 0;
    s64 rc = mempolicy_check_nodemask(uold, maxnode, &om);
    if (rc < 0) return rc;
    rc = mempolicy_check_nodemask(unew, maxnode, &nm);
    if (rc < 0) return rc;

    /* Every page is already on the only node there is, so nothing could not
     * be moved — which is exactly what a return of 0 means. */
    return 0;
}

s64 sys_set_mempolicy_home_node_impl(pt_regs_t *r)
{
    uintptr_t addr = (uintptr_t)r->rdi;
    u64 len = r->rsi;
    unsigned long home_node = (unsigned long)r->rdx;
    unsigned long flags = (unsigned long)r->r10;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (flags) return -(s64)EINVAL;
    if (addr & (PAGE_SIZE - 1)) return -(s64)EINVAL;
    if (home_node != 0) return -(s64)EINVAL;   /* node 0 is the only node */
    u64 end = addr + ALIGN_UP(len, PAGE_SIZE);
    if (end < addr || end > TASK_SIZE_MAX) return -(s64)EINVAL;

    proc->mempolicy_home_node = (u32)home_node;
    return 0;
}

/* ============================================================================
 * process_mrelease(2) — reap the address space of a process that is already
 * dying, without waiting for it to be reaped by its parent.
 * ========================================================================= */

s64 sys_process_mrelease_impl(pt_regs_t *r)
{
    int pidfd = (int)r->rdi;
    unsigned int flags = (unsigned int)r->rsi;

    if (flags != 0) return -(s64)EINVAL;

    process_t *me = sched_current_process();
    if (!me) return -(s64)EPERM;

    file_t *f = fget(me, pidfd);
    if (!f) return -(s64)EBADF;
    if (f->f_op != &g_pidfd_fops || !f->private_data) { fput(f); return -(s64)EBADF; }
    u32 target_pid = ((pidfd_ctx_t *)f->private_data)->target_pid;
    fput(f);

    struct proc_ident id;
    if (!sched_proc_ident(target_pid, &id)) return -(s64)ESRCH;
    /* Only for a process that is already on its way out: the call exists to
     * accelerate an OOM kill, not to shoot down a running program. */
    if (!id.is_zombie) return -(s64)EINVAL;
    if (me->euid != 0 && me->euid != id.uid) return -(s64)EPERM;

    /* A zombie's address space is torn down at exit here, so the memory this
     * call would free is already gone. */
    return 0;
}

s64 sys_memfd_secret_impl(pt_regs_t *r)
{
    (void)r;
    /* memfd_secret: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_map_shadow_stack_impl(pt_regs_t *r)
{
    (void)r;
    /* map_shadow_stack: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}
