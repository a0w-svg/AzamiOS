/* ============================================================================
 * AzamiOS — System V IPC (XSI) implementation
 * File: kernel/ipc/sysvipc.c
 *
 * See sysvipc.h for the shape of the interface.  Three fixed tables back the
 * three mechanisms; a slot is free when its `used` flag is clear, and every
 * reuse of a slot bumps its sequence number so stale identifiers fail with
 * EIDRM rather than silently addressing a different object.
 *
 * Blocking operations — semop() waiting on a condition, msgsnd() on a full
 * queue, msgrcv() on an empty one — re-test their condition once per
 * scheduler tick rather than parking on a wait queue.  That trades a little
 * latency for not having to make every IPC object own a wait queue, and it
 * keeps the interruption path simple: a signal that becomes deliverable
 * while sleeping ends the wait with EINTR, as POSIX requires.
 * ============================================================================ */

#define DEBUG 0
#include <azami/debug.h>
#include "sysvipc.h"
#include "../sched/sched.h"
#include "../mm/kmalloc.h"
#include "../mm/pmm.h"
#include "../mm/vma.h"
#include "../lib/string.h"
#include "../uaccess.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../arch/x86_64/cpu/hwaccel.h"

/* Wall-clock seconds, shared with the syscall layer's cached RTC read. */
extern u64 get_cached_unix_time(void);

/* ── Shared state ────────────────────────────────────────────────────────── */

typedef struct shm_segment {
    bool        used;
    u16         seq;
    struct ipc64_perm perm;
    size_t      size;            /* requested size, unrounded  */
    size_t      npages;
    phys_addr_t *pages;
    s64         atime, dtime, ctime;
    s32         cpid, lpid;
    u64         nattch;
    bool        pending_rmid;    /* IPC_RMID seen; free at last detach */
} shm_segment_t;

typedef struct shm_attach {
    bool        used;
    u32         pid;
    virt_addr_t addr;
    u32         idx;             /* slot in g_shm */
    size_t      npages;
} shm_attach_t;

typedef struct sem_set {
    bool        used;
    u16         seq;
    struct ipc64_perm perm;
    u32         nsems;
    s32        *vals;
    s32        *pids;            /* pid of the last operation per semaphore */
    u32        *ncnt;            /* processes waiting for the value to rise */
    u32        *zcnt;            /* processes waiting for the value to hit 0 */
    s64         otime, ctime;
} sem_set_t;

/* One process's outstanding SEM_UNDO adjustment for one semaphore. */
typedef struct sem_undo {
    bool used;
    u32  pid;
    u32  idx;                    /* slot in g_sem */
    u16  seq;
    u32  semnum;
    s32  adjust;
} sem_undo_t;

typedef struct msg_node {
    s64    mtype;
    size_t msgsz;
    struct msg_node *next;
    /* payload follows the node in the same allocation */
} msg_node_t;

typedef struct msg_queue {
    bool        used;
    u16         seq;
    struct ipc64_perm perm;
    msg_node_t *head, *tail;
    u64         cbytes, qnum, qbytes;
    s64         stime, rtime, ctime;
    s32         lspid, lrpid;
} msg_queue_t;

static shm_segment_t g_shm[SHM_MAX_SEGS];
static shm_attach_t  g_shm_attach[SHM_MAX_ATTACH];
static sem_set_t     g_sem[SEM_MAX_SETS];
static sem_undo_t    g_sem_undo[SEM_MAX_UNDO];
static msg_queue_t   g_msg[MSG_MAX_QUEUES];
static spinlock_t    g_ipc_lock = SPINLOCK_INIT;

void sysvipc_init(void)
{
    memset(g_shm, 0, sizeof(g_shm));
    memset(g_shm_attach, 0, sizeof(g_shm_attach));
    memset(g_sem, 0, sizeof(g_sem));
    memset(g_sem_undo, 0, sizeof(g_sem_undo));
    memset(g_msg, 0, sizeof(g_msg));
    pr_debug("[SYSVIPC] XSI shared memory, semaphores and message queues ready\n");
}

/* ── Identifiers and permissions ─────────────────────────────────────────── */

static inline int ipc_build_id(u32 idx, u16 seq)
{
    return (int)((u32)seq * IPCMNI + idx);
}

static inline u32 ipc_id_index(int id) { return (u32)id % IPCMNI; }
static inline u16 ipc_id_seq(int id)   { return (u16)((u32)id / IPCMNI); }

/*
 * Fold a get()/ctl() flag word into the access it is asking for, the way
 * Linux's ipcperms() does: the caller passes a full 0777-style mode, and the
 * owner, group and other triplets are OR'd together into a single 0007 mask.
 * A request of 0660 therefore asks for read+write, and 0444 for read only.
 */
static inline u32 ipc_want_from_flag(int flag)
{
    u32 f = (u32)flag;
    return ((f >> 6) | (f >> 3) | f) & 0007;
}

/*
 * Access check with the classic owner/group/other split.  `want` is 4 for
 * read and 2 for write, matching the low bits of the permission mode.
 */
static bool ipc_permitted(const struct ipc64_perm *p, process_t *proc, u32 want)
{
    if (!proc) return false;
    if (proc->euid == 0) return true;

    if (proc->euid == p->uid || proc->euid == p->cuid) return ((p->mode >> 6) & want) == want;
    if (proc->egid == p->gid || proc->egid == p->cgid) return ((p->mode >> 3) & want) == want;
    return (p->mode & want) == want;
}

/* Only the owner, the creator or a privileged process may destroy or modify. */
static bool ipc_owner(const struct ipc64_perm *p, process_t *proc)
{
    return proc && (proc->euid == 0 || proc->euid == p->uid || proc->euid == p->cuid);
}

static void ipc_perm_init(struct ipc64_perm *p, s32 key, u32 mode, process_t *proc, u16 seq)
{
    memset(p, 0, sizeof(*p));
    p->key  = key;
    p->uid  = p->cuid = proc ? proc->euid : 0;
    p->gid  = p->cgid = proc ? proc->egid : 0;
    p->mode = mode & 0777;
    p->seq  = seq;
}

/* Sleep one tick; true if a signal became deliverable and the wait must end. */
static bool ipc_wait_tick(process_t *proc)
{
    sched_sleep(1);
    return (proc->sig_pending & ~proc->sig_blocked) != 0;
}

static bool user_ptr_ok(const void *p)
{
    return p != NULL && (uintptr_t)p < 0x8000000000000000ULL;
}

/* ============================================================================
 * Shared memory
 * ========================================================================= */

static shm_segment_t *shm_lookup(int shmid, u32 *out_idx)
{
    u32 idx = ipc_id_index(shmid);
    if (shmid < 0 || idx >= SHM_MAX_SEGS) return NULL;

    shm_segment_t *s = &g_shm[idx];
    if (!s->used || s->seq != ipc_id_seq(shmid)) return NULL;
    if (out_idx) *out_idx = idx;
    return s;
}

static void shm_free_slot(shm_segment_t *s)
{
    if (s->pages) {
        for (size_t i = 0; i < s->npages; i++) {
            if (s->pages[i]) pmm_free_page(s->pages[i]);
        }
        kfree(s->pages);
    }
    s->pages  = NULL;
    s->npages = 0;
    s->used   = false;
    s->pending_rmid = false;
    s->seq++;              /* invalidate every id that referred to this slot */
}

s64 sysv_shmget(s32 key, size_t size, int shmflg)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    size_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (key != IPC_PRIVATE && size == 0) npages = 0;   /* lookup-only is fine */
    if (npages > SHM_MAX_PAGES) return -(s64)EINVAL;

    spinlock_lock(&g_ipc_lock);

    if (key != IPC_PRIVATE) {
        for (u32 i = 0; i < SHM_MAX_SEGS; i++) {
            if (!g_shm[i].used || g_shm[i].perm.key != key) continue;

            shm_segment_t *s = &g_shm[i];
            if ((shmflg & IPC_CREAT) && (shmflg & IPC_EXCL)) {
                spinlock_unlock(&g_ipc_lock);
                return -(s64)EEXIST;
            }
            if (size && s->size < size) {
                spinlock_unlock(&g_ipc_lock);
                return -(s64)EINVAL;
            }
            if (!ipc_permitted(&s->perm, proc, ipc_want_from_flag(shmflg))) {
                spinlock_unlock(&g_ipc_lock);
                return -(s64)EACCES;
            }
            int id = ipc_build_id(i, s->seq);
            spinlock_unlock(&g_ipc_lock);
            return id;
        }
        if (!(shmflg & IPC_CREAT)) {
            spinlock_unlock(&g_ipc_lock);
            return -(s64)ENOENT;
        }
    }

    if (size == 0) {
        spinlock_unlock(&g_ipc_lock);
        return -(s64)EINVAL;
    }

    for (u32 i = 0; i < SHM_MAX_SEGS; i++) {
        if (g_shm[i].used) continue;
        shm_segment_t *s = &g_shm[i];

        s->pages = (phys_addr_t *)kzalloc(npages * sizeof(phys_addr_t));
        if (!s->pages) {
            spinlock_unlock(&g_ipc_lock);
            return -(s64)ENOMEM;
        }

        /* Segments are populated up front: a shared mapping has no owning
         * process to fault it in on demand. */
        for (size_t p = 0; p < npages; p++) {
            phys_addr_t page = pmm_alloc_page();
            if (!page) {
                for (size_t k = 0; k < p; k++) pmm_free_page(s->pages[k]);
                kfree(s->pages);
                s->pages = NULL;
                spinlock_unlock(&g_ipc_lock);
                return -(s64)ENOMEM;
            }
            hw_clear_page(PHYS_TO_VIRT(page));
            s->pages[p] = page;
        }

        ipc_perm_init(&s->perm, key, (u32)shmflg, proc, s->seq);
        s->used    = true;
        s->size    = size;
        s->npages  = npages;
        s->cpid    = (s32)proc->pid;
        s->lpid    = 0;
        s->nattch  = 0;
        s->atime   = s->dtime = 0;
        s->ctime   = (s64)get_cached_unix_time();

        int id = ipc_build_id(i, s->seq);
        spinlock_unlock(&g_ipc_lock);
        return id;
    }

    spinlock_unlock(&g_ipc_lock);
    return -(s64)ENOSPC;
}

s64 sysv_shmat(int shmid, virt_addr_t shmaddr, int shmflg)
{
    process_t *proc = sched_current_process();
    if (!proc || !proc->pml4_phys) return -(s64)EPERM;

    spinlock_lock(&g_ipc_lock);

    u32 idx;
    shm_segment_t *s = shm_lookup(shmid, &idx);
    if (!s) { spinlock_unlock(&g_ipc_lock); return -(s64)EINVAL; }

    bool readonly = (shmflg & SHM_RDONLY) != 0;
    if (!ipc_permitted(&s->perm, proc, readonly ? 4 : 6)) {
        spinlock_unlock(&g_ipc_lock);
        return -(s64)EACCES;
    }

    size_t len = s->npages * PAGE_SIZE;
    virt_addr_t va;

    if (shmaddr) {
        va = shmaddr;
        if (shmflg & SHM_RND) va = ALIGN_DOWN(va, PAGE_SIZE);
        if (va & (PAGE_SIZE - 1)) { spinlock_unlock(&g_ipc_lock); return -(s64)EINVAL; }
        if (va < 0x1000 || va + len >= 0x0000800000000000ULL) {
            spinlock_unlock(&g_ipc_lock);
            return -(s64)EINVAL;
        }
        for (size_t off = 0; off < len; off += PAGE_SIZE) {
            if (vmm_translate(proc->pml4_phys, va + off)) {
                spinlock_unlock(&g_ipc_lock);
                return -(s64)EINVAL;      /* would clobber an existing mapping */
            }
        }
    } else {
        /* Share the process's mmap arena so an attachment can never land on
         * an address mmap() would hand out later. */
        if (!proc->mmap_current || proc->mmap_current < 0x0000600000000000ULL ||
            proc->mmap_current >= 0x00007f0000000000ULL) {
            proc->mmap_current = 0x0000700000000000ULL;
        }
        if (len > 0x00007f0000000000ULL - proc->mmap_current) {
            spinlock_unlock(&g_ipc_lock);
            return -(s64)ENOMEM;
        }
        va = proc->mmap_current;
        proc->mmap_current += ALIGN_UP(len, PAGE_SIZE);
    }

    int slot = -1;
    for (u32 i = 0; i < SHM_MAX_ATTACH; i++) {
        if (!g_shm_attach[i].used) { slot = (int)i; break; }
    }
    if (slot < 0) { spinlock_unlock(&g_ipc_lock); return -(s64)ENOMEM; }

    /* VMM_F_SHARED marks the frames as owned by the segment, so unmapping
     * them — here or through munmap/exit — never returns them to the PMM. */
    u64 flags = (readonly ? VMM_USER_RO : VMM_USER_RW) | VMM_F_SHARED | VMM_F_NX;
    for (size_t i = 0; i < s->npages; i++) {
        vmm_map(proc->pml4_phys, va + i * PAGE_SIZE, s->pages[i], flags);
    }
    vma_add(proc, va, va + len,
            readonly ? VMA_PROT_READ : (VMA_PROT_READ | VMA_PROT_WRITE),
            VMA_F_SHARED | VMA_F_ANON);

    g_shm_attach[slot].used   = true;
    g_shm_attach[slot].pid    = proc->pid;
    g_shm_attach[slot].addr   = va;
    g_shm_attach[slot].idx    = idx;
    g_shm_attach[slot].npages = s->npages;

    s->nattch++;
    s->lpid  = (s32)proc->pid;
    s->atime = (s64)get_cached_unix_time();

    spinlock_unlock(&g_ipc_lock);
    return (s64)va;
}

/* Tear one attachment down.  Caller holds g_ipc_lock. */
static void shm_detach_locked(shm_attach_t *a, process_t *proc)
{
    if (proc && proc->pml4_phys) {
        /* The segment owns the frames; only the mappings go away here. */
        vmm_unmap_range(proc->pml4_phys, a->addr, a->npages, false);
        vma_remove(proc, a->addr, a->addr + a->npages * PAGE_SIZE);
    }

    shm_segment_t *s = &g_shm[a->idx];
    if (s->used) {
        if (s->nattch) s->nattch--;
        s->lpid  = proc ? (s32)proc->pid : s->lpid;
        s->dtime = (s64)get_cached_unix_time();
        /* A segment marked for removal disappears once nobody holds it. */
        if (s->pending_rmid && s->nattch == 0) shm_free_slot(s);
    }
    a->used = false;
}

s64 sysv_shmdt(virt_addr_t shmaddr)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    spinlock_lock(&g_ipc_lock);
    for (u32 i = 0; i < SHM_MAX_ATTACH; i++) {
        shm_attach_t *a = &g_shm_attach[i];
        if (!a->used || a->pid != proc->pid || a->addr != shmaddr) continue;
        shm_detach_locked(a, proc);
        spinlock_unlock(&g_ipc_lock);
        return 0;
    }
    spinlock_unlock(&g_ipc_lock);
    return -(s64)EINVAL;
}

s64 sysv_shmctl(int shmid, int cmd, void *buf)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    cmd &= ~IPC_64;   /* the 64-bit layout is the only one implemented */

    /* IPC_INFO and SHM_INFO describe the subsystem, not one segment, so they
     * are answered before any id lookup.  Both return the highest slot index
     * in use, which is what lets `ipcs` drive SHM_STAT across the table. */
    if (cmd == IPC_INFO || cmd == SHM_INFO) {
        s64 highest = -1;
        struct shminfo64 lim;
        struct shm_info  use;
        memset(&lim, 0, sizeof(lim));
        memset(&use, 0, sizeof(use));

        lim.shmmax = (u64)SHM_MAX_PAGES * PAGE_SIZE;
        lim.shmmin = 1;
        lim.shmmni = SHM_MAX_SEGS;
        lim.shmseg = SHM_MAX_ATTACH;
        lim.shmall = (u64)SHM_MAX_SEGS * SHM_MAX_PAGES;

        spinlock_lock(&g_ipc_lock);
        for (u32 i = 0; i < SHM_MAX_SEGS; i++) {
            if (!g_shm[i].used) continue;
            highest = (s64)i;
            use.used_ids++;
            use.shm_tot += g_shm[i].npages;
        }
        use.shm_rss = use.shm_tot;   /* nothing is ever paged out */
        spinlock_unlock(&g_ipc_lock);

        if (!user_ptr_ok(buf)) return -(s64)EFAULT;
        if (cmd == IPC_INFO) {
            if (copy_to_user(buf, &lim, sizeof(lim)) != 0) return -(s64)EFAULT;
        } else {
            if (copy_to_user(buf, &use, sizeof(use)) != 0) return -(s64)EFAULT;
        }
        return highest < 0 ? 0 : highest;
    }

    spinlock_lock(&g_ipc_lock);

    /* SHM_STAT indexes the table directly instead of naming an id. */
    u32 idx;
    shm_segment_t *s;
    if (cmd == SHM_STAT) {
        if (shmid < 0 || (u32)shmid >= SHM_MAX_SEGS || !g_shm[shmid].used) {
            spinlock_unlock(&g_ipc_lock);
            return -(s64)EINVAL;
        }
        idx = (u32)shmid;
        s   = &g_shm[idx];
    } else {
        s = shm_lookup(shmid, &idx);
        if (!s) { spinlock_unlock(&g_ipc_lock); return -(s64)EINVAL; }
    }

    switch (cmd) {
    case IPC_STAT:
    case SHM_STAT: {
        if (!ipc_permitted(&s->perm, proc, 4)) {
            spinlock_unlock(&g_ipc_lock);
            return -(s64)EACCES;
        }
        struct shmid64_ds out;
        memset(&out, 0, sizeof(out));
        out.shm_perm   = s->perm;
        out.shm_segsz  = s->size;
        out.shm_atime  = s->atime;
        out.shm_dtime  = s->dtime;
        out.shm_ctime  = s->ctime;
        out.shm_cpid   = s->cpid;
        out.shm_lpid   = s->lpid;
        out.shm_nattch = s->nattch;
        int id = ipc_build_id(idx, s->seq);
        spinlock_unlock(&g_ipc_lock);

        if (!user_ptr_ok(buf)) return -(s64)EFAULT;
        if (copy_to_user(buf, &out, sizeof(out)) != 0) return -(s64)EFAULT;
        return (cmd == SHM_STAT) ? id : 0;
    }

    case IPC_SET: {
        if (!ipc_owner(&s->perm, proc)) {
            spinlock_unlock(&g_ipc_lock);
            return -(s64)EPERM;
        }
        spinlock_unlock(&g_ipc_lock);

        if (!user_ptr_ok(buf)) return -(s64)EFAULT;
        struct shmid64_ds in;
        if (copy_from_user(&in, buf, sizeof(in)) != 0) return -(s64)EFAULT;

        /* The lock was dropped across copy_from_user(), so the slot may have
         * been freed and handed to a completely different segment meanwhile.
         * Re-resolve the id — that revalidates the sequence number — instead
         * of trusting the stale pointer's `used` flag. */
        spinlock_lock(&g_ipc_lock);
        s = shm_lookup(shmid, NULL);
        if (!s) { spinlock_unlock(&g_ipc_lock); return -(s64)EIDRM; }
        s->perm.uid  = in.shm_perm.uid;
        s->perm.gid  = in.shm_perm.gid;
        s->perm.mode = (s->perm.mode & ~0777u) | (in.shm_perm.mode & 0777u);
        s->ctime     = (s64)get_cached_unix_time();
        spinlock_unlock(&g_ipc_lock);
        return 0;
    }

    case IPC_RMID: {
        if (!ipc_owner(&s->perm, proc)) {
            spinlock_unlock(&g_ipc_lock);
            return -(s64)EPERM;
        }
        /* Removal is deferred while the segment is still attached, which is
         * what lets the shmget/shmat/IPC_RMID idiom work. */
        if (s->nattch == 0) shm_free_slot(s);
        else                s->pending_rmid = true;
        spinlock_unlock(&g_ipc_lock);
        return 0;
    }

    case SHM_LOCK:
    case SHM_UNLOCK:
        /* Every segment is already resident and never paged out. */
        spinlock_unlock(&g_ipc_lock);
        return 0;

    default:
        spinlock_unlock(&g_ipc_lock);
        return -(s64)EINVAL;
    }
}

/* ============================================================================
 * Semaphores
 * ========================================================================= */

static sem_set_t *sem_lookup(int semid, u32 *out_idx)
{
    u32 idx = ipc_id_index(semid);
    if (semid < 0 || idx >= SEM_MAX_SETS) return NULL;

    sem_set_t *s = &g_sem[idx];
    if (!s->used || s->seq != ipc_id_seq(semid)) return NULL;
    if (out_idx) *out_idx = idx;
    return s;
}

static void sem_free_slot(sem_set_t *s)
{
    if (s->vals) kfree(s->vals);
    if (s->pids) kfree(s->pids);
    if (s->ncnt) kfree(s->ncnt);
    if (s->zcnt) kfree(s->zcnt);
    s->vals = NULL; s->pids = NULL; s->ncnt = NULL; s->zcnt = NULL;
    s->nsems = 0;
    s->used  = false;
    s->seq++;
}

static void sem_undo_record(u32 idx, u16 seq, u32 pid, u32 semnum, s32 adjust)
{
    for (u32 i = 0; i < SEM_MAX_UNDO; i++) {
        sem_undo_t *u = &g_sem_undo[i];
        if (u->used && u->pid == pid && u->idx == idx && u->seq == seq && u->semnum == semnum) {
            u->adjust += adjust;
            if (u->adjust == 0) u->used = false;
            return;
        }
    }
    if (adjust == 0) return;
    for (u32 i = 0; i < SEM_MAX_UNDO; i++) {
        sem_undo_t *u = &g_sem_undo[i];
        if (u->used) continue;
        u->used = true; u->pid = pid; u->idx = idx; u->seq = seq;
        u->semnum = semnum; u->adjust = adjust;
        return;
    }
    /* Undo table full: the adjustment is simply not recorded, which is the
     * documented failure mode rather than failing the operation itself. */
}

s64 sysv_semget(s32 key, int nsems, int semflg)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (nsems < 0 || nsems > SEM_MAX_NSEMS) return -(s64)EINVAL;

    spinlock_lock(&g_ipc_lock);

    if (key != IPC_PRIVATE) {
        for (u32 i = 0; i < SEM_MAX_SETS; i++) {
            if (!g_sem[i].used || g_sem[i].perm.key != key) continue;

            sem_set_t *s = &g_sem[i];
            if ((semflg & IPC_CREAT) && (semflg & IPC_EXCL)) {
                spinlock_unlock(&g_ipc_lock);
                return -(s64)EEXIST;
            }
            if (nsems && (u32)nsems > s->nsems) {
                spinlock_unlock(&g_ipc_lock);
                return -(s64)EINVAL;
            }
            if (!ipc_permitted(&s->perm, proc, ipc_want_from_flag(semflg))) {
                spinlock_unlock(&g_ipc_lock);
                return -(s64)EACCES;
            }
            int id = ipc_build_id(i, s->seq);
            spinlock_unlock(&g_ipc_lock);
            return id;
        }
        if (!(semflg & IPC_CREAT)) {
            spinlock_unlock(&g_ipc_lock);
            return -(s64)ENOENT;
        }
    }

    if (nsems == 0) { spinlock_unlock(&g_ipc_lock); return -(s64)EINVAL; }

    for (u32 i = 0; i < SEM_MAX_SETS; i++) {
        if (g_sem[i].used) continue;
        sem_set_t *s = &g_sem[i];

        s->vals = (s32 *)kzalloc((size_t)nsems * sizeof(s32));
        s->pids = (s32 *)kzalloc((size_t)nsems * sizeof(s32));
        s->ncnt = (u32 *)kzalloc((size_t)nsems * sizeof(u32));
        s->zcnt = (u32 *)kzalloc((size_t)nsems * sizeof(u32));
        if (!s->vals || !s->pids || !s->ncnt || !s->zcnt) {
            sem_free_slot(s);
            s->seq--;    /* the slot was never handed out */
            spinlock_unlock(&g_ipc_lock);
            return -(s64)ENOMEM;
        }

        ipc_perm_init(&s->perm, key, (u32)semflg, proc, s->seq);
        s->used  = true;
        s->nsems = (u32)nsems;
        s->otime = 0;
        s->ctime = (s64)get_cached_unix_time();

        int id = ipc_build_id(i, s->seq);
        spinlock_unlock(&g_ipc_lock);
        return id;
    }

    spinlock_unlock(&g_ipc_lock);
    return -(s64)ENOSPC;
}

s64 sysv_semtimedop(int semid, const void *tsops, size_t nsops,
                    u64 timeout_ticks, bool has_timeout)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (nsops == 0 || nsops > SEM_MAX_OPS) return -(s64)EINVAL;
    if (!user_ptr_ok(tsops)) return -(s64)EFAULT;

    struct sembuf ops[SEM_MAX_OPS];
    if (copy_from_user(ops, tsops, nsops * sizeof(struct sembuf)) != 0) return -(s64)EFAULT;

    const u64 deadline = sched_get_ticks() + timeout_ticks;

    for (;;) {
        spinlock_lock(&g_ipc_lock);

        u32 idx;
        sem_set_t *s = sem_lookup(semid, &idx);
        if (!s) { spinlock_unlock(&g_ipc_lock); return -(s64)EIDRM; }

        bool need_write = false;
        for (size_t i = 0; i < nsops; i++) {
            if (ops[i].sem_num >= s->nsems) {
                spinlock_unlock(&g_ipc_lock);
                return -(s64)EFBIG;
            }
            if (ops[i].sem_op != 0) need_write = true;
        }
        if (!ipc_permitted(&s->perm, proc, need_write ? 2 : 4)) {
            spinlock_unlock(&g_ipc_lock);
            return -(s64)EACCES;
        }

        /* All-or-nothing: the whole array must be satisfiable before any of
         * it is applied, which is what makes semop() atomic. */
        bool blocked = false;
        s16  blocked_flg = 0;
        for (size_t i = 0; i < nsops && !blocked; i++) {
            s32 cur = s->vals[ops[i].sem_num];
            if (ops[i].sem_op < 0) {
                if (cur + ops[i].sem_op < 0) { blocked = true; blocked_flg = ops[i].sem_flg; }
            } else if (ops[i].sem_op == 0) {
                if (cur != 0) { blocked = true; blocked_flg = ops[i].sem_flg; }
            } else if (cur + ops[i].sem_op > SEM_VALUE_MAX_XSI) {
                /* An increment past SEMVMX is an error, never a wait. */
                spinlock_unlock(&g_ipc_lock);
                return -(s64)ERANGE;
            }
        }

        if (!blocked) {
            for (size_t i = 0; i < nsops; i++) {
                u32 n = ops[i].sem_num;
                s->vals[n] += ops[i].sem_op;
                s->pids[n]  = (s32)proc->pid;
                if (ops[i].sem_flg & SEM_UNDO) {
                    sem_undo_record(idx, s->seq, proc->pid, n, -ops[i].sem_op);
                }
            }
            s->otime = (s64)get_cached_unix_time();
            spinlock_unlock(&g_ipc_lock);
            return 0;
        }

        /* IPC_NOWAIT is a property of the individual operation that cannot
         * proceed, not of the array as a whole. */
        if (blocked_flg & IPC_NOWAIT) { spinlock_unlock(&g_ipc_lock); return -(s64)EAGAIN; }

        /* A timeout that has already run out ends the call before we sleep
         * again, so semtimedop() with a short timeout cannot overshoot it. */
        if (has_timeout && (s64)(sched_get_ticks() - deadline) >= 0) {
            spinlock_unlock(&g_ipc_lock);
            return -(s64)EAGAIN;
        }

        /* Publish the wait so semctl(GETNCNT/GETZCNT) can report it. */
        for (size_t i = 0; i < nsops; i++) {
            if (ops[i].sem_op < 0)      s->ncnt[ops[i].sem_num]++;
            else if (ops[i].sem_op == 0) s->zcnt[ops[i].sem_num]++;
        }
        spinlock_unlock(&g_ipc_lock);

        bool interrupted = ipc_wait_tick(proc);

        spinlock_lock(&g_ipc_lock);
        s = sem_lookup(semid, &idx);
        if (s) {
            for (size_t i = 0; i < nsops; i++) {
                if (ops[i].sem_op < 0) {
                    if (s->ncnt[ops[i].sem_num]) s->ncnt[ops[i].sem_num]--;
                } else if (ops[i].sem_op == 0) {
                    if (s->zcnt[ops[i].sem_num]) s->zcnt[ops[i].sem_num]--;
                }
            }
        }
        spinlock_unlock(&g_ipc_lock);

        if (!s) return -(s64)EIDRM;
        if (interrupted) return -(s64)EINTR;
        if (has_timeout && (s64)(sched_get_ticks() - deadline) >= 0) return -(s64)EAGAIN;
    }
}

s64 sysv_semop(int semid, const void *tsops, size_t nsops)
{
    return sysv_semtimedop(semid, tsops, nsops, 0, false);
}

s64 sysv_semctl(int semid, int semnum, int cmd, u64 arg)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    cmd &= ~IPC_64;

    /* Subsystem-wide queries: no id is involved.  IPC_INFO reports the limits,
     * SEM_INFO reuses the same structure to report current usage. */
    if (cmd == IPC_INFO || cmd == SEM_INFO) {
        s64 highest = -1;
        struct seminfo info;
        memset(&info, 0, sizeof(info));

        info.semmni = SEM_MAX_SETS;
        info.semmsl = SEM_MAX_NSEMS;
        info.semmns = SEM_MAX_SETS * SEM_MAX_NSEMS;
        info.semopm = SEM_MAX_OPS;
        info.semmnu = SEM_MAX_UNDO;
        info.semume = SEM_MAX_UNDO;
        info.semvmx = SEM_VALUE_MAX_XSI;
        info.semaem = SEM_VALUE_MAX_XSI;
        info.semmap = SEM_MAX_SETS * SEM_MAX_NSEMS;

        spinlock_lock(&g_ipc_lock);
        s32 used_sets = 0, used_sems = 0;
        for (u32 i = 0; i < SEM_MAX_SETS; i++) {
            if (!g_sem[i].used) continue;
            highest = (s64)i;
            used_sets++;
            used_sems += (s32)g_sem[i].nsems;
        }
        spinlock_unlock(&g_ipc_lock);

        if (cmd == SEM_INFO) {
            info.semusz = used_sets;    /* sets in use    */
            info.semaem = used_sems;    /* semaphores in use */
        }

        void *ubuf = (void *)(uintptr_t)arg;
        if (!user_ptr_ok(ubuf)) return -(s64)EFAULT;
        if (copy_to_user(ubuf, &info, sizeof(info)) != 0) return -(s64)EFAULT;
        return highest < 0 ? 0 : highest;
    }

    spinlock_lock(&g_ipc_lock);

    u32 idx;
    sem_set_t *s;
    if (cmd == SEM_STAT) {
        if (semid < 0 || (u32)semid >= SEM_MAX_SETS || !g_sem[semid].used) {
            spinlock_unlock(&g_ipc_lock);
            return -(s64)EINVAL;
        }
        idx = (u32)semid;
        s   = &g_sem[idx];
    } else {
        s = sem_lookup(semid, &idx);
        if (!s) { spinlock_unlock(&g_ipc_lock); return -(s64)EINVAL; }
    }

    switch (cmd) {
    case IPC_STAT:
    case SEM_STAT: {
        if (!ipc_permitted(&s->perm, proc, 4)) {
            spinlock_unlock(&g_ipc_lock); return -(s64)EACCES;
        }
        struct semid64_ds out;
        memset(&out, 0, sizeof(out));
        out.sem_perm  = s->perm;
        out.sem_otime = s->otime;
        out.sem_ctime = s->ctime;
        out.sem_nsems = s->nsems;
        int id = ipc_build_id(idx, s->seq);
        spinlock_unlock(&g_ipc_lock);

        void *ubuf = (void *)(uintptr_t)arg;
        if (!user_ptr_ok(ubuf)) return -(s64)EFAULT;
        if (copy_to_user(ubuf, &out, sizeof(out)) != 0) return -(s64)EFAULT;
        return (cmd == SEM_STAT) ? id : 0;
    }

    case IPC_SET: {
        if (!ipc_owner(&s->perm, proc)) { spinlock_unlock(&g_ipc_lock); return -(s64)EPERM; }
        spinlock_unlock(&g_ipc_lock);

        void *ubuf = (void *)(uintptr_t)arg;
        if (!user_ptr_ok(ubuf)) return -(s64)EFAULT;
        struct semid64_ds in;
        if (copy_from_user(&in, ubuf, sizeof(in)) != 0) return -(s64)EFAULT;

        spinlock_lock(&g_ipc_lock);
        s = sem_lookup(semid, NULL);          /* revalidate across the gap */
        if (!s) { spinlock_unlock(&g_ipc_lock); return -(s64)EIDRM; }
        s->perm.uid  = in.sem_perm.uid;
        s->perm.gid  = in.sem_perm.gid;
        s->perm.mode = (s->perm.mode & ~0777u) | (in.sem_perm.mode & 0777u);
        s->ctime     = (s64)get_cached_unix_time();
        spinlock_unlock(&g_ipc_lock);
        return 0;
    }

    case IPC_RMID:
        if (!ipc_owner(&s->perm, proc)) { spinlock_unlock(&g_ipc_lock); return -(s64)EPERM; }
        /* Drop undo records first; the slot's sequence number is about to
         * change and they key off it. */
        for (u32 i = 0; i < SEM_MAX_UNDO; i++) {
            if (g_sem_undo[i].used && g_sem_undo[i].idx == idx) g_sem_undo[i].used = false;
        }
        sem_free_slot(s);
        spinlock_unlock(&g_ipc_lock);
        return 0;

    case GETVAL:
        if (semnum < 0 || (u32)semnum >= s->nsems) { spinlock_unlock(&g_ipc_lock); return -(s64)EINVAL; }
        if (!ipc_permitted(&s->perm, proc, 4))     { spinlock_unlock(&g_ipc_lock); return -(s64)EACCES; }
        { s64 v = s->vals[semnum]; spinlock_unlock(&g_ipc_lock); return v; }

    case GETPID:
        if (semnum < 0 || (u32)semnum >= s->nsems) { spinlock_unlock(&g_ipc_lock); return -(s64)EINVAL; }
        { s64 v = s->pids[semnum]; spinlock_unlock(&g_ipc_lock); return v; }

    case GETNCNT:
        if (semnum < 0 || (u32)semnum >= s->nsems) { spinlock_unlock(&g_ipc_lock); return -(s64)EINVAL; }
        { s64 v = s->ncnt[semnum]; spinlock_unlock(&g_ipc_lock); return v; }

    case GETZCNT:
        if (semnum < 0 || (u32)semnum >= s->nsems) { spinlock_unlock(&g_ipc_lock); return -(s64)EINVAL; }
        { s64 v = s->zcnt[semnum]; spinlock_unlock(&g_ipc_lock); return v; }

    case SETVAL: {
        if (semnum < 0 || (u32)semnum >= s->nsems) { spinlock_unlock(&g_ipc_lock); return -(s64)EINVAL; }
        if (!ipc_permitted(&s->perm, proc, 2))     { spinlock_unlock(&g_ipc_lock); return -(s64)EACCES; }
        s32 val = (s32)(u32)arg;
        if (val < 0 || val > SEM_VALUE_MAX_XSI) {
            spinlock_unlock(&g_ipc_lock); return -(s64)ERANGE;
        }
        s->vals[semnum] = val;
        s->pids[semnum] = (s32)proc->pid;
        s->ctime = (s64)get_cached_unix_time();
        /* POSIX: setting a value clears the undo entries for that semaphore. */
        for (u32 i = 0; i < SEM_MAX_UNDO; i++) {
            if (g_sem_undo[i].used && g_sem_undo[i].idx == idx &&
                g_sem_undo[i].semnum == (u32)semnum) {
                g_sem_undo[i].used = false;
            }
        }
        spinlock_unlock(&g_ipc_lock);
        return 0;
    }

    case GETALL: {
        if (!ipc_permitted(&s->perm, proc, 4)) { spinlock_unlock(&g_ipc_lock); return -(s64)EACCES; }
        u16 tmp[SEM_MAX_NSEMS];
        u32 n = s->nsems;
        for (u32 i = 0; i < n; i++) tmp[i] = (u16)s->vals[i];
        spinlock_unlock(&g_ipc_lock);

        void *ubuf = (void *)(uintptr_t)arg;
        if (!user_ptr_ok(ubuf)) return -(s64)EFAULT;
        return copy_to_user(ubuf, tmp, n * sizeof(u16)) == 0 ? 0 : -(s64)EFAULT;
    }

    case SETALL: {
        if (!ipc_permitted(&s->perm, proc, 2)) { spinlock_unlock(&g_ipc_lock); return -(s64)EACCES; }
        u32 n = s->nsems;
        spinlock_unlock(&g_ipc_lock);

        void *ubuf = (void *)(uintptr_t)arg;
        if (!user_ptr_ok(ubuf)) return -(s64)EFAULT;
        u16 tmp[SEM_MAX_NSEMS];
        if (copy_from_user(tmp, ubuf, n * sizeof(u16)) != 0) return -(s64)EFAULT;
        /* Reject the whole array before applying any of it, so a bad element
         * cannot leave the set half-updated. */
        for (u32 i = 0; i < n; i++)
            if (tmp[i] > SEM_VALUE_MAX_XSI) return -(s64)ERANGE;

        spinlock_lock(&g_ipc_lock);
        s = sem_lookup(semid, &idx);          /* revalidate across the gap */
        if (!s || s->nsems != n) { spinlock_unlock(&g_ipc_lock); return -(s64)EIDRM; }
        for (u32 i = 0; i < n; i++) {
            s->vals[i] = (s32)tmp[i];
            s->pids[i] = (s32)proc->pid;
        }
        s->ctime = (s64)get_cached_unix_time();
        for (u32 i = 0; i < SEM_MAX_UNDO; i++) {
            if (g_sem_undo[i].used && g_sem_undo[i].idx == idx) g_sem_undo[i].used = false;
        }
        spinlock_unlock(&g_ipc_lock);
        return 0;
    }

    default:
        spinlock_unlock(&g_ipc_lock);
        return -(s64)EINVAL;
    }
}

/* ============================================================================
 * Message queues
 * ========================================================================= */

static msg_queue_t *msg_lookup(int msqid, u32 *out_idx)
{
    u32 idx = ipc_id_index(msqid);
    if (msqid < 0 || idx >= MSG_MAX_QUEUES) return NULL;

    msg_queue_t *q = &g_msg[idx];
    if (!q->used || q->seq != ipc_id_seq(msqid)) return NULL;
    if (out_idx) *out_idx = idx;
    return q;
}

static void msg_free_slot(msg_queue_t *q)
{
    msg_node_t *n = q->head;
    while (n) {
        msg_node_t *next = n->next;
        kfree(n);
        n = next;
    }
    q->head = q->tail = NULL;
    q->cbytes = q->qnum = 0;
    q->used = false;
    q->seq++;
}

s64 sysv_msgget(s32 key, int msgflg)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    spinlock_lock(&g_ipc_lock);

    if (key != IPC_PRIVATE) {
        for (u32 i = 0; i < MSG_MAX_QUEUES; i++) {
            if (!g_msg[i].used || g_msg[i].perm.key != key) continue;

            msg_queue_t *q = &g_msg[i];
            if ((msgflg & IPC_CREAT) && (msgflg & IPC_EXCL)) {
                spinlock_unlock(&g_ipc_lock);
                return -(s64)EEXIST;
            }
            if (!ipc_permitted(&q->perm, proc, ipc_want_from_flag(msgflg))) {
                spinlock_unlock(&g_ipc_lock);
                return -(s64)EACCES;
            }
            int id = ipc_build_id(i, q->seq);
            spinlock_unlock(&g_ipc_lock);
            return id;
        }
        if (!(msgflg & IPC_CREAT)) {
            spinlock_unlock(&g_ipc_lock);
            return -(s64)ENOENT;
        }
    }

    for (u32 i = 0; i < MSG_MAX_QUEUES; i++) {
        if (g_msg[i].used) continue;
        msg_queue_t *q = &g_msg[i];

        ipc_perm_init(&q->perm, key, (u32)msgflg, proc, q->seq);
        q->used   = true;
        q->head   = q->tail = NULL;
        q->cbytes = q->qnum = 0;
        q->qbytes = MSG_MAX_BYTES;
        q->stime  = q->rtime = 0;
        q->ctime  = (s64)get_cached_unix_time();
        q->lspid  = q->lrpid = 0;

        int id = ipc_build_id(i, q->seq);
        spinlock_unlock(&g_ipc_lock);
        return id;
    }

    spinlock_unlock(&g_ipc_lock);
    return -(s64)ENOSPC;
}

s64 sysv_msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (msgsz > MSG_MAX_SIZE) return -(s64)EINVAL;
    if (!user_ptr_ok(msgp)) return -(s64)EFAULT;

    /* The user buffer is `struct msgbuf { long mtype; char mtext[]; }`. */
    s64 mtype;
    if (copy_from_user(&mtype, msgp, sizeof(mtype)) != 0) return -(s64)EFAULT;
    if (mtype < 1) return -(s64)EINVAL;

    msg_node_t *node = (msg_node_t *)kzalloc(sizeof(msg_node_t) + msgsz);
    if (!node) return -(s64)ENOMEM;
    node->mtype = mtype;
    node->msgsz = msgsz;
    if (msgsz && copy_from_user((u8 *)(node + 1), (const u8 *)msgp + sizeof(s64), msgsz) != 0) {
        kfree(node);
        return -(s64)EFAULT;
    }

    for (;;) {
        spinlock_lock(&g_ipc_lock);

        msg_queue_t *q = msg_lookup(msqid, NULL);
        if (!q) { spinlock_unlock(&g_ipc_lock); kfree(node); return -(s64)EIDRM; }
        if (!ipc_permitted(&q->perm, proc, 2)) {
            spinlock_unlock(&g_ipc_lock); kfree(node); return -(s64)EACCES;
        }

        if (q->cbytes + msgsz <= q->qbytes) {
            node->next = NULL;
            if (q->tail) q->tail->next = node;
            else         q->head = node;
            q->tail    = node;
            q->cbytes += msgsz;
            q->qnum++;
            q->lspid   = (s32)proc->pid;
            q->stime   = (s64)get_cached_unix_time();
            spinlock_unlock(&g_ipc_lock);
            return 0;
        }

        if (msgflg & IPC_NOWAIT) {
            spinlock_unlock(&g_ipc_lock);
            kfree(node);
            return -(s64)EAGAIN;
        }
        spinlock_unlock(&g_ipc_lock);

        if (ipc_wait_tick(proc)) { kfree(node); return -(s64)EINTR; }
    }
}

s64 sysv_msgrcv(int msqid, void *msgp, size_t msgsz, s64 msgtyp, int msgflg)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!user_ptr_ok(msgp)) return -(s64)EFAULT;

    for (;;) {
        spinlock_lock(&g_ipc_lock);

        msg_queue_t *q = msg_lookup(msqid, NULL);
        if (!q) { spinlock_unlock(&g_ipc_lock); return -(s64)EIDRM; }
        if (!ipc_permitted(&q->perm, proc, 4)) {
            spinlock_unlock(&g_ipc_lock); return -(s64)EACCES;
        }

        /*
         * Selection follows POSIX: 0 takes the first message, a positive type
         * takes the first of exactly that type, and a negative type takes the
         * lowest type that is at most |msgtyp|.
         */
        msg_node_t *prev = NULL, *found = NULL, *found_prev = NULL;
        for (msg_node_t *n = q->head; n; prev = n, n = n->next) {
            bool match;
            if (msgtyp == 0)      match = true;
            else if (msgtyp > 0)  match = (msgflg & MSG_EXCEPT) ? (n->mtype != msgtyp)
                                                               : (n->mtype == msgtyp);
            else                  match = (n->mtype <= -msgtyp);

            if (!match) continue;
            if (msgtyp < 0) {
                if (!found || n->mtype < found->mtype) { found = n; found_prev = prev; }
            } else {
                found = n; found_prev = prev;
                break;
            }
        }

        if (found) {
            if (found->msgsz > msgsz && !(msgflg & MSG_NOERROR)) {
                spinlock_unlock(&g_ipc_lock);
                return -(s64)E2BIG;
            }

            if (found_prev) found_prev->next = found->next;
            else            q->head = found->next;
            if (q->tail == found) q->tail = found_prev;

            q->cbytes -= found->msgsz;
            q->qnum--;
            q->lrpid = (s32)proc->pid;
            q->rtime = (s64)get_cached_unix_time();
            spinlock_unlock(&g_ipc_lock);

            size_t copied = found->msgsz < msgsz ? found->msgsz : msgsz;
            s64 mtype = found->mtype;
            int err = 0;
            if (copy_to_user(msgp, &mtype, sizeof(mtype)) != 0) err = 1;
            else if (copied && copy_to_user((u8 *)msgp + sizeof(s64),
                                            (u8 *)(found + 1), copied) != 0) err = 1;
            kfree(found);
            return err ? -(s64)EFAULT : (s64)copied;
        }

        if (msgflg & IPC_NOWAIT) {
            spinlock_unlock(&g_ipc_lock);
            return -(s64)ENOMSG;
        }
        spinlock_unlock(&g_ipc_lock);

        if (ipc_wait_tick(proc)) return -(s64)EINTR;
    }
}

s64 sysv_msgctl(int msqid, int cmd, void *buf)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    cmd &= ~IPC_64;

    if (cmd == IPC_INFO || cmd == MSG_INFO) {
        s64 highest = -1;
        struct msginfo info;
        memset(&info, 0, sizeof(info));

        info.msgmni = MSG_MAX_QUEUES;
        info.msgmax = MSG_MAX_SIZE;
        info.msgmnb = MSG_MAX_BYTES;
        info.msgssz = 16;
        info.msgseg = 0;

        spinlock_lock(&g_ipc_lock);
        s32 used = 0;
        u64 total_bytes = 0, total_msgs = 0;
        for (u32 i = 0; i < MSG_MAX_QUEUES; i++) {
            if (!g_msg[i].used) continue;
            highest = (s64)i;
            used++;
            total_bytes += g_msg[i].cbytes;
            total_msgs  += g_msg[i].qnum;
        }
        spinlock_unlock(&g_ipc_lock);

        if (cmd == MSG_INFO) {
            info.msgpool = used;                 /* queues in use   */
            info.msgmap  = (s32)total_msgs;      /* messages queued */
            info.msgtql  = (s32)total_bytes;     /* bytes queued    */
        } else {
            info.msgpool = MSG_MAX_QUEUES * MSG_MAX_BYTES / 1024;
            info.msgmap  = MSG_MAX_QUEUES;
            info.msgtql  = MSG_MAX_QUEUES * MSG_MAX_BYTES;
        }

        if (!user_ptr_ok(buf)) return -(s64)EFAULT;
        if (copy_to_user(buf, &info, sizeof(info)) != 0) return -(s64)EFAULT;
        return highest < 0 ? 0 : highest;
    }

    spinlock_lock(&g_ipc_lock);

    u32 idx;
    msg_queue_t *q;
    if (cmd == MSG_STAT) {
        if (msqid < 0 || (u32)msqid >= MSG_MAX_QUEUES || !g_msg[msqid].used) {
            spinlock_unlock(&g_ipc_lock);
            return -(s64)EINVAL;
        }
        idx = (u32)msqid;
        q   = &g_msg[idx];
    } else {
        q = msg_lookup(msqid, &idx);
        if (!q) { spinlock_unlock(&g_ipc_lock); return -(s64)EINVAL; }
    }

    switch (cmd) {
    case IPC_STAT:
    case MSG_STAT: {
        if (!ipc_permitted(&q->perm, proc, 4)) {
            spinlock_unlock(&g_ipc_lock); return -(s64)EACCES;
        }
        struct msqid64_ds out;
        memset(&out, 0, sizeof(out));
        out.msg_perm   = q->perm;
        out.msg_stime  = q->stime;
        out.msg_rtime  = q->rtime;
        out.msg_ctime  = q->ctime;
        out.msg_cbytes = q->cbytes;
        out.msg_qnum   = q->qnum;
        out.msg_qbytes = q->qbytes;
        out.msg_lspid  = q->lspid;
        out.msg_lrpid  = q->lrpid;
        int id = ipc_build_id(idx, q->seq);
        spinlock_unlock(&g_ipc_lock);

        if (!user_ptr_ok(buf)) return -(s64)EFAULT;
        if (copy_to_user(buf, &out, sizeof(out)) != 0) return -(s64)EFAULT;
        return (cmd == MSG_STAT) ? id : 0;
    }

    case IPC_SET: {
        if (!ipc_owner(&q->perm, proc)) { spinlock_unlock(&g_ipc_lock); return -(s64)EPERM; }
        spinlock_unlock(&g_ipc_lock);

        if (!user_ptr_ok(buf)) return -(s64)EFAULT;
        struct msqid64_ds in;
        if (copy_from_user(&in, buf, sizeof(in)) != 0) return -(s64)EFAULT;

        spinlock_lock(&g_ipc_lock);
        q = msg_lookup(msqid, NULL);          /* revalidate across the gap */
        if (!q) { spinlock_unlock(&g_ipc_lock); return -(s64)EIDRM; }
        q->perm.uid  = in.msg_perm.uid;
        q->perm.gid  = in.msg_perm.gid;
        q->perm.mode = (q->perm.mode & ~0777u) | (in.msg_perm.mode & 0777u);
        if (in.msg_qbytes && in.msg_qbytes <= MSG_MAX_BYTES) q->qbytes = in.msg_qbytes;
        q->ctime = (s64)get_cached_unix_time();
        spinlock_unlock(&g_ipc_lock);
        return 0;
    }

    case IPC_RMID:
        if (!ipc_owner(&q->perm, proc)) { spinlock_unlock(&g_ipc_lock); return -(s64)EPERM; }
        msg_free_slot(q);
        spinlock_unlock(&g_ipc_lock);
        return 0;

    default:
        spinlock_unlock(&g_ipc_lock);
        return -(s64)EINVAL;
    }
}

/* ============================================================================
 * Process teardown
 * ========================================================================= */

/*
 * Drop every attachment a pid holds, bookkeeping only.  Both callers below are
 * replacing or destroying the address space wholesale, so the mappings go away
 * with it and only nattch — and the deferred-removal it gates — matter here.
 * Caller holds g_ipc_lock.
 */
static void shm_drop_attachments_locked(u32 pid)
{
    for (u32 i = 0; i < SHM_MAX_ATTACH; i++) {
        shm_attach_t *a = &g_shm_attach[i];
        if (!a->used || a->pid != pid) continue;

        shm_segment_t *s = &g_shm[a->idx];
        if (s->used) {
            if (s->nattch) s->nattch--;
            s->dtime = (s64)get_cached_unix_time();
            if (s->pending_rmid && s->nattch == 0) shm_free_slot(s);
        }
        a->used = false;
    }
}

/*
 * sysvipc_process_fork() — POSIX: the child of fork() inherits every attached
 * shared-memory segment, and each segment's shm_nattch counts it.
 *
 * Without this the child holds the mappings (the frames are VMM_F_SHARED, so
 * vmm_clone_space() shares rather than copies them) while the segment believes
 * only the parent is attached.  A parent that then detaches and calls
 * IPC_RMID drops nattch to zero, the segment's pages go back to the PMM, and
 * the child is left reading and writing freed physical memory.
 *
 * Slots are reserved before anything is published so the copy either takes
 * effect in full or not at all.
 */
int sysvipc_process_fork(struct process *child, struct process *parent)
{
    if (!child || !parent) return 0;
    u32 ppid = ((process_t *)parent)->pid;
    u32 cpid = ((process_t *)child)->pid;

    spinlock_lock(&g_ipc_lock);

    u32 need = 0, have = 0;
    for (u32 i = 0; i < SHM_MAX_ATTACH; i++) {
        if (g_shm_attach[i].used) { if (g_shm_attach[i].pid == ppid) need++; }
        else have++;
    }
    if (need == 0) { spinlock_unlock(&g_ipc_lock); return 0; }
    if (have < need) { spinlock_unlock(&g_ipc_lock); return -(int)ENOMEM; }

    /* Snapshot the parent's slots first: the copies below occupy free slots
     * that this same scan would otherwise walk into and duplicate. */
    u32 src[SHM_MAX_ATTACH];
    u32 n = 0;
    for (u32 i = 0; i < SHM_MAX_ATTACH; i++) {
        if (g_shm_attach[i].used && g_shm_attach[i].pid == ppid) src[n++] = i;
    }

    for (u32 k = 0; k < n; k++) {
        shm_attach_t *a = &g_shm_attach[src[k]];
        for (u32 i = 0; i < SHM_MAX_ATTACH; i++) {
            if (g_shm_attach[i].used) continue;
            g_shm_attach[i] = *a;
            g_shm_attach[i].pid = cpid;
            if (g_shm[a->idx].used) g_shm[a->idx].nattch++;
            break;
        }
    }

    spinlock_unlock(&g_ipc_lock);
    return 0;
}

/*
 * sysvipc_process_exec() — POSIX: "the shared memory segments attached to the
 * calling process shall not be attached to the new process image".  The old
 * address space is discarded by execve() itself; what has to go with it is the
 * attachment bookkeeping, or the segments stay pinned for the lifetime of the
 * new image and a stale recorded address can later be handed to shmdt().
 */
void sysvipc_process_exec(struct process *proc)
{
    if (!proc) return;
    spinlock_lock(&g_ipc_lock);
    shm_drop_attachments_locked(((process_t *)proc)->pid);
    spinlock_unlock(&g_ipc_lock);
}

void sysvipc_process_exit(struct process *proc)
{
    if (!proc) return;
    u32 pid = ((process_t *)proc)->pid;

    spinlock_lock(&g_ipc_lock);

    shm_drop_attachments_locked(pid);

    /* Apply SEM_UNDO: a process that dies holding semaphores must not leave
     * them held, or every other waiter blocks forever. */
    for (u32 i = 0; i < SEM_MAX_UNDO; i++) {
        sem_undo_t *u = &g_sem_undo[i];
        if (!u->used || u->pid != pid) continue;

        sem_set_t *s = &g_sem[u->idx];
        if (s->used && s->seq == u->seq && u->semnum < s->nsems) {
            s32 v = s->vals[u->semnum] + u->adjust;
            s->vals[u->semnum] = v < 0 ? 0 : v;
        }
        u->used = false;
    }

    spinlock_unlock(&g_ipc_lock);
}

/* ============================================================================
 * /proc/sysvipc rendering — same columns Linux uses, so ipcs can parse it
 * ========================================================================= */

int sysvipc_proc_shm(char *buf, size_t len)
{
    int n = scnprintf(buf, len,
        "       key      shmid perms       size  cpid  lpid nattch   uid   gid  cuid  cgid      atime      dtime      ctime\n");

    spinlock_lock(&g_ipc_lock);
    for (u32 i = 0; i < SHM_MAX_SEGS && (size_t)n < len; i++) {
        shm_segment_t *s = &g_shm[i];
        if (!s->used) continue;
        n += scnprintf(buf + n, len - (size_t)n,
                      "%10d %10d %5o %10llu %5d %5d %6llu %5u %5u %5u %5u %10lld %10lld %10lld\n",
                      s->perm.key, ipc_build_id(i, s->seq), s->perm.mode,
                      (unsigned long long)s->size, s->cpid, s->lpid,
                      (unsigned long long)s->nattch,
                      s->perm.uid, s->perm.gid, s->perm.cuid, s->perm.cgid,
                      (long long)s->atime, (long long)s->dtime, (long long)s->ctime);
    }
    spinlock_unlock(&g_ipc_lock);
    return n;
}

int sysvipc_proc_sem(char *buf, size_t len)
{
    int n = scnprintf(buf, len,
        "       key      semid perms      nsems   uid   gid  cuid  cgid      otime      ctime\n");

    spinlock_lock(&g_ipc_lock);
    for (u32 i = 0; i < SEM_MAX_SETS && (size_t)n < len; i++) {
        sem_set_t *s = &g_sem[i];
        if (!s->used) continue;
        n += scnprintf(buf + n, len - (size_t)n,
                      "%10d %10d %5o %10u %5u %5u %5u %5u %10lld %10lld\n",
                      s->perm.key, ipc_build_id(i, s->seq), s->perm.mode, s->nsems,
                      s->perm.uid, s->perm.gid, s->perm.cuid, s->perm.cgid,
                      (long long)s->otime, (long long)s->ctime);
    }
    spinlock_unlock(&g_ipc_lock);
    return n;
}

int sysvipc_proc_msg(char *buf, size_t len)
{
    int n = scnprintf(buf, len,
        "       key      msqid perms      cbytes       qnum lspid lrpid   uid   gid  cuid  cgid      stime      rtime      ctime\n");

    spinlock_lock(&g_ipc_lock);
    for (u32 i = 0; i < MSG_MAX_QUEUES && (size_t)n < len; i++) {
        msg_queue_t *q = &g_msg[i];
        if (!q->used) continue;
        n += scnprintf(buf + n, len - (size_t)n,
                      "%10d %10d %5o %11llu %10llu %5d %5d %5u %5u %5u %5u %10lld %10lld %10lld\n",
                      q->perm.key, ipc_build_id(i, q->seq), q->perm.mode,
                      (unsigned long long)q->cbytes, (unsigned long long)q->qnum,
                      q->lspid, q->lrpid,
                      q->perm.uid, q->perm.gid, q->perm.cuid, q->perm.cgid,
                      (long long)q->stime, (long long)q->rtime, (long long)q->ctime);
    }
    spinlock_unlock(&g_ipc_lock);
    return n;
}
