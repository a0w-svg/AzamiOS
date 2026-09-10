/* ============================================================================
 * AzamiOS — CFS Scheduler & Process/Thread Management Header
 * File: kernel/sched/sched.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../arch/x86_64/cpu/cpu.h"
#include "../../arch/x86_64/cpu/idt.h" /* pt_regs_t */

/* Thread states */
typedef enum {
    THREAD_READY = 0,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_BLOCKED_PENDING,
    THREAD_SLEEPING,
    THREAD_SLEEPING_PENDING,
    THREAD_DYING,
    THREAD_ZOMBIE
} thread_state_t;

struct process;
struct az_object;
typedef struct az_object az_object_t;

/* Extended FPU/SIMD save area, sized by FPU_STATE_MAX_SIZE (arch/x86_64/cpu/
 * cpu.h). XSAVE for XCR0 = x87|SSE|AVX needs 832 bytes and the AVX-512 set
 * pushes that to ~2.7 KB; the constant covers both plus the 63 bytes the
 * runtime alignment bump below can consume. cpu_enable_features_bsp() refuses
 * to put a component in XCR0 whose save area would not fit here, so the buffer
 * is always large enough for whatever XSAVE variant the boot settled on.
 *
 * NOTE: deliberately NOT __attribute__((aligned(64))). kzalloc only guarantees
 * 16-byte alignment (16-byte block header), so the struct would be misaligned
 * at runtime anyway — and worse, telling the compiler it is 64-aligned lets it
 * fold away the runtime FPU_ALIGN() bump in sched.c, feeding xsave64 a
 * 16-mod-64 pointer → #GP. The runtime FPU_ALIGN() is the real guarantee. */
typedef struct {
    u8 buffer[FPU_STATE_MAX_SIZE];
} fpu_state_t;

/**
 * struct thread — Microkernel execution unit.
 */
typedef struct thread {
    u32             tid;             /* Thread ID */
    struct process *proc;            /* Owning process */
    u64             kernel_rsp;      /* Saved kernel stack pointer on context switch */
    u64             kernel_stack_top;/* Top of dedicated kernel stack for ring-0 transitions */
    u64             kernel_stack_base;/* Lowest mapped stack page VA (guard page sits just below) */
    thread_state_t  state;           /* Thread state */
    bool            unblock_pending; /* Signal from sched_unblock during context switch */
    bool            stopped;         /* Parked in a job-control / ptrace stop  */
    u64             vruntime;        /* Completely Fair Scheduler virtual runtime */
    u32             priority;        /* Thread priority (weight modifier for CFS) */
    u32             cpu_id;          /* Currently assigned logical CPU */
    pt_regs_t      *user_regs;       /* Saved user frame during syscalls/interrupts */
    u64             sleep_end_ticks; /* Ticks when sleeping should end */
    u64             fs_base;         /* Per-thread FS_BASE (TLS); 0 => use proc->fs_base */
    u64             clear_child_tid; /* CLONE_CHILD_CLEARTID / set_tid_address(2) futex */
    fpu_state_t     fpu_state;       /* XSAVE/FXSAVE area */
    struct thread  *next;            /* Ready queue / list pointer */
    struct thread  *proc_next;       /* Next thread in the same process */
    struct thread  *sem_next;        /* POSIX semaphore wait queue link */
} thread_t;

/* ── POSIX Signals ────────────────────────────────────────────────────────── */
#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1    10
#define SIGSEGV    11
#define SIGUSR2    12
#define SIGPIPE    13
#define SIGALRM    14
#define SIGTERM    15
#define SIGCHLD    17
#define SIGCONT    18
#define SIGSTOP    19
#define SIGTSTP    20
#define SIGWINCH   28
#define _NSIG      64

typedef u64 sigset_t;

typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_RESTORER  0x04000000
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

typedef struct sigaction {
    sighandler_t sa_handler;
    u64          sa_flags;
    void       (*sa_restorer)(void);
    sigset_t     sa_mask;
} sigaction_t;

#define MAX_SHMEM_PER_PROC 64

/*
 * Size of every per-process descriptor table (VFS files, their flags, and
 * object-manager handles). This used to be a bare 64 repeated at the three
 * array declarations below and at well over a hundred bounds checks and
 * iteration limits spread across the syscall layer, the scheduler, procfs and
 * several drivers. Raising the limit meant finding all of them; missing one
 * turned a bounds check into an out-of-bounds access on the very table it was
 * meant to guard. One definition, used everywhere.
 */
#define PROC_MAX_FDS 64

typedef struct {
    u32 shmem_id;
    virt_addr_t virt_addr;
    void *shmem_ptr;
} proc_shmem_map_t;

/**
 * struct process — Microkernel address space and resource container.
 */
typedef struct process {
    u32             pid;             /* Process ID */
    phys_addr_t     pml4_phys;       /* Physical address of level-4 page table (CR3) */
    char            name[32];        /* Process name */
    thread_t       *threads;         /* Head of threads list in this process */
    struct process *parent;          /* Parent process */
    struct process *next;            /* Global process list pointer */
    void           *handle_table[PROC_MAX_FDS];      /* VFS file descriptor table */
    u8              fd_flags[PROC_MAX_FDS];          /* File descriptor flags (FD_CLOEXEC) */
    az_object_t    *obj_handle_table[PROC_MAX_FDS];  /* Object Manager handle table */
    proc_shmem_map_t shmem_maps[MAX_SHMEM_PER_PROC]; /* Shared memory mappings */
    virt_addr_t     heap_start;            /* Base of user heap for brk */
    virt_addr_t     heap_end;              /* Current break address for brk */
    virt_addr_t     mmap_current;          /* Current bump pointer for anonymous mmap */
    void           *vma_list;              /* Sorted vm_area_t list (kernel/mm/vma.c) */
    int             exit_code;             /* exit() status (0..255), valid when term_signal == 0 */
    int             term_signal;           /* Signal that terminated the process, or 0 for normal exit */
    u32             pgid;                  /* POSIX process group ID (job control) */
    u32             sid;                   /* POSIX session ID */
    bool            is_zombie;             /* Process terminated, awaiting waitpid */
    bool            child_subreaper;       /* PR_SET_CHILD_SUBREAPER: reaps orphan descendents */
    struct thread  *wait_thread;          /* Parent thread waiting on child exit */
    /* Filesystem namespace.
     *
     * `root` is the process's filesystem root as a REAL kernel path — "/" for
     * everything that has not called chroot(2). `cwd` is the working directory
     * as the process SEES it, i.e. relative to `root`; the two are identical
     * whenever root is "/", which is the only case that existed before chroot
     * was made to confine anything.
     *
     * The syscall layer maps between them: a user path is normalised against
     * cwd (where ".." can never climb above "/"), then `root` is prepended to
     * reach the real path the VFS is asked for. getcwd(2) reports cwd, so a
     * confined process cannot even learn where its jail sits. */
    char            root[256];             /* Real path of the filesystem root */
    char            cwd[256];              /* Working directory, relative to root */
    u32             umask;                 /* POSIX-02: per-process file creation mask (default 022) */
    u32             uid;                   /* Real User ID */
    u32             gid;                   /* Real Group ID */
    u32             euid;                  /* Effective User ID */
    u32             egid;                  /* Effective Group ID */
    u32             suid;                  /* Saved User ID */
    u32             sgid;                  /* Saved Group ID */
    u32             groups[32];            /* Supplementary groups */
    u32             ngroups;               /* Number of supplementary groups */
    int             pdeath_sig;            /* Signal to receive on parent death */
    sigaction_t     sigactions[_NSIG];     /* Signal handlers */
    sigset_t        sig_pending;           /* Pending signals bitmask */
    sigset_t        sig_blocked;           /* Blocked signals bitmask */

    /* ── Job-control stop (SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU) and ptrace ────
     * stop_state is the authority: non-zero means every thread of this
     * process must park on its next return to ring 3, and PROC_STOP_* says
     * which of the two mechanisms asked for it. The requesting side also ORs
     * the stop signal into sig_pending, so a thread already asleep inside a
     * blocking syscall breaks out through the same `sig_pending & ~blocked`
     * test the rest of the kernel already uses for -EINTR — without that a
     * process blocked in read(2) could not be stopped until the read
     * finished. See kernel/ptrace.c for the whole state machine. */
    u32             stop_state;            /* PROC_STOP_* (0 = running)      */
    int             stop_signal;           /* Signal that caused the stop    */
    bool            stop_notified;         /* Parent already saw this stop   */
    bool            cont_pending;          /* SIGCONT awaiting a WCONTINUED  */

    u32             tracer_pid;            /* ptrace(2) tracer, 0 if untraced*/
    u64             ptrace_opts;           /* PTRACE_O_* set by SETOPTIONS   */
    u64             ptrace_msg;            /* PTRACE_GETEVENTMSG value       */
    u64             ptrace_orig_rax;       /* Syscall number of the call the
                                            * tracee is inside; what strace
                                            * reads as orig_rax at the exit
                                            * stop, where RAX already holds
                                            * the return value              */
    u32             ptrace_flags;          /* PT_* — see kernel/ptrace.h     */
    u32             ptrace_event;          /* PTRACE_EVENT_* of current stop */
    int             ptrace_stop_sig;       /* Signal reported at this stop   */
    int             ptrace_inject_sig;     /* Signal the tracer asked us to
                                            * deliver on resume (0 = none)   */
    u32             hold_count;            /* >0 while a syscall on another CPU
                                            * holds a counted reference to this
                                            * process (proc_get_by_pid / the
                                            * ptrace pin). The reaper, wait4()
                                            * and proc_destroy() must not free
                                            * it — its threads or its address
                                            * space — until it drops to 0.
                                            * Raised under g_sched_lock (the
                                            * lock every teardown path also
                                            * holds to decide a free); dropped
                                            * with a bare atomic decrement. */
    u64             fs_base;               /* x86_64 FS_BASE (User TLS) */
    u64             gs_base;               /* x86_64 GS_BASE (User TLS) */

    /* ── POSIX.1e capability sets (see kernel/security/security.h) ────────
     * A bit per CAP_* constant. Previously the CAP_* checks scattered through
     * the syscall layer collapsed to "is euid 0", so a privilege could never
     * be dropped or delegated. These make the checks mean something: root is
     * granted the full set at credential-change time, and any process can
     * narrow its own set irreversibly via capset()/the bounding set. */
    u64             cap_permitted;         /* Caps the process may enable      */
    u64             cap_effective;         /* Caps currently in force          */
    u64             cap_inheritable;       /* Caps preserved across execve()   */
    u64             cap_bounding;          /* Ceiling; only ever cleared       */

    /* PR_SET_NO_NEW_PRIVS. Once set it is inherited and can never be cleared,
     * so a sandboxed child cannot regain privilege through a setuid execve(). */
    bool            no_new_privs;

    /* seccomp(2) mode: 0 = disabled, 1 = SECCOMP_MODE_STRICT. Inherited across
     * fork/exec and, like no_new_privs, one-way. */
    u32             seccomp_mode;

    /* personality(2) word. Only ADDR_NO_RANDOMIZE (0x0040000) is acted on: it
     * disables ASLR for the next execve(), which is what `setarch -R` and most
     * debuggers rely on to get reproducible addresses. */
    u32             personality;

    /* POSIX process CPU accounting, in scheduler ticks. Charged by
     * sched_tick() to whichever process was running, split by the privilege
     * level it was interrupted in, and rolled into the parent's c* fields
     * when the process is reaped. Backs times(2), getrusage(2) and the
     * ITIMER_VIRTUAL / ITIMER_PROF interval timers. */
    u64             utime_ticks;           /* ticks spent in ring 3          */
    u64             stime_ticks;           /* ticks spent in the kernel      */
    u64             cutime_ticks;          /* reaped children's user time    */
    u64             cstime_ticks;          /* reaped children's system time  */

    /* Coarse per-process event counts. Each is one increment on a path that
     * is already slow (a fault, a context switch), which is what lets
     * perf_event_open()'s software events be a subtraction of two reads rather
     * than a hook that has to run inside the event itself. */
    u64             nr_minor_faults;       /* demand-paged / COW faults      */
    u64             nr_major_faults;       /* faults needing I/O (none yet)  */
    u64             nr_ctx_switches;       /* times a thread was resumed     */
    u64             nr_migrations;         /* resumed on a different CPU     */
    u32             last_cpu;              /* CPU it last ran on; (u32)-1
                                            * until it has run at all       */

    /* Stack extent chosen by the ELF loader for this image. Randomised per
     * exec, so the stack VMA can no longer be a hard-coded constant. */
    virt_addr_t     stack_low;
    virt_addr_t     stack_high;

    /* Alternate signal stack (sigaltstack) */
    void           *sas_ss_sp;
    size_t          sas_ss_size;
    int             sas_ss_flags;

    /* set_robust_list(2)/get_robust_list(2). The kernel stores the pointer and
     * hands it back; it does not walk the list on exit, so a process that dies
     * holding a robust mutex leaves it locked. Keeping the value round-trips
     * correctly for the many libraries that set it and read it back. */
    void           *robust_list;
    size_t          robust_list_len;

    /* pkey_alloc(2) bookkeeping: bit k set means key k is allocated. Key 0 is
     * the default key every page carries and is never handed out or freed. */
    u16             pkey_alloc_map;

    /* ioprio_set(2)/ioprio_get(2). Class and level packed the way Linux packs
     * them (see IOPRIO_PRIO_VALUE). 0 means "unset", which ioprio_get() maps
     * back to a class derived from the CPU nice level, exactly as Linux does. */
    u32             ioprio;

    /* NUMA memory policy (set_mempolicy(2) / mbind(2) / get_mempolicy(2)).
     * This machine has one memory node, so the policy is recorded and reported
     * faithfully but cannot change where a page comes from. Storing it is what
     * makes a round-trip through get_mempolicy() honest, and lets a nodemask
     * naming a node that does not exist be rejected rather than ignored. */
    u32             mempolicy_mode;
    u64             mempolicy_nodemask;
    u32             mempolicy_home_node;
} process_t;

/* wait4(2) option bits this kernel honours. */
#define WNOHANG      0x00000001
#define WUNTRACED    0x00000002
#define WCONTINUED   0x00000008

/* process_t::stop_state — why every thread of the process must park. */
#define PROC_STOP_NONE     0   /* running normally                           */
#define PROC_STOP_JOB      1   /* job-control stop; only SIGCONT/SIGKILL end it */
#define PROC_STOP_PTRACE   2   /* ptrace-stop; the tracer decides when it ends  */

/** sched_init() — Initialize CFS scheduler and per-CPU idle threads. */
void sched_init(void);

/** sched_start() — Start scheduling on current CPU (does not return). */
void sched_start(void);

/** sched_tick(regs) — Handle LAPIC timer tick, update vruntime, and preempt if necessary. */
void sched_tick(pt_regs_t *regs);

/** sched_yield() — Voluntarily yield CPU to the next ready thread. */
void sched_yield(void);

/** sched_check_reschedule() — Check and perform deferred reschedule (call from IRQ return path). */
void sched_check_reschedule(void);

/** sched_block(new_state) — Block current thread until unblocked. */
void sched_block(thread_state_t new_state);

/** sched_sleep(ticks) — Put current thread to sleep for specified tick count. */
void sched_sleep(u64 ticks);

/** sched_waitpid(target_pid, status, options) — Wait for child process termination. */
s64 sched_waitpid(s32 target_pid, int *status, int options);

/** sched_unblock(t) — Mark a blocked/sleeping thread as READY. */
void sched_unblock(thread_t *t);

/** sched_current_thread() — Return pointer to current thread on calling CPU. */
thread_t *sched_current_thread(void);

/** sched_current_process() — Return pointer to current process on calling CPU. */
process_t *sched_current_process(void);

/** proc_create(name, pml4_phys) — Create a new process container. */
process_t *proc_create(const char *name, phys_addr_t pml4_phys);

/** proc_destroy(proc) — Unlink and free process container. */
void proc_destroy(process_t *proc);

/** thread_create(proc, entry, arg, is_kernel) — Create a new execution thread and enqueue. */
thread_t *thread_create(process_t *proc, uintptr_t entry, uintptr_t arg, bool is_kernel);

/** thread_create_ex(proc, entry, arg, is_kernel, enqueue) — Create a new execution thread with optional enqueue. */
thread_t *thread_create_ex(process_t *proc, uintptr_t entry, uintptr_t arg, bool is_kernel, bool enqueue);

/** sched_enqueue_thread(t) — Add a prepared thread to the CFS ready queue. */
void sched_enqueue_thread(thread_t *t);

/** thread_clear_child_tid() — Honour CLONE_CHILD_CLEARTID for a dying thread:
 *  zero the registered user word and wake one futex waiter on it. Must run in
 *  the exiting thread's own context, while its address space is still live.
 *  Implemented in kernel/syscall/syscall.c, next to the futex tables. */
void thread_clear_child_tid(struct thread *t);

/** sched_exit_thread() — Terminate current thread and reschedule (never returns). */
__noreturn void sched_exit_thread(void);

/** sched_exit_group_mark() — for exit_group(2)/execve(2): wind down every
 *  *other* thread of the current process. Caller then does its own
 *  sched_exit_thread() (exit_group) or continues (execve). */
void sched_exit_group_mark(void);

/** sched_dethread_wait() — spin until every other thread of the current process
 *  has reached THREAD_ZOMBIE. Call after sched_exit_group_mark() when the caller
 *  is about to free shared resources (execve freeing the old address space). */
void sched_dethread_wait(void);

/** sched_dethread_reap() — free all zombie sibling threads and their kernel stacks
 *  after dethread_wait(). Leaves only the calling thread in proc->threads. */
void sched_dethread_reap(void);

/**
 * sched_kernel_process() — the process every kernel thread belongs to.
 *
 * Subsystems that need a background thread (the POSIX timer engine, the
 * zombie reaper) create it against this.
 */
process_t *sched_kernel_process(void);

/** sched_get_process_by_pid(pid) — Find process by PID.
 *
 * Returns a bare pointer with no lifetime guarantee: safe only while the caller
 * holds g_sched_lock, or for a target that cannot exit (e.g. the caller
 * itself). Any code that keeps the pointer past a lock drop, or dereferences a
 * process another CPU may be tearing down, must use proc_get_by_pid() instead.
 */
process_t *sched_get_process_by_pid(u32 pid);

/**
 * proc_get_by_pid(pid) — Look up a live process and take a counted reference.
 *
 * The reference (process_t::hold_count) is raised under g_sched_lock, the same
 * lock the reaper, wait4() and proc_destroy() hold while they decide whether a
 * process may be unlinked and freed — so a get can never interleave with a
 * free. While the reference is held the process_t, its thread list and its
 * address space are guaranteed to stay allocated. Release it with proc_put().
 *
 * Returns NULL for an unknown or already-zombie pid.
 */
process_t *proc_get_by_pid(u32 pid);

/**
 * proc_get_locked(p) — Take a counted reference to a process the caller already
 * holds a valid pointer to (typically mid-walk of the process list under
 * g_sched_lock). Caller must hold g_sched_lock.
 */
void proc_get_locked(process_t *p);

/** proc_put(p) — Drop a reference taken by proc_get_by_pid()/proc_get_locked(). */
void proc_put(process_t *p);

/** sched_kill_process(pid, sig) — Send signal to process or terminate it. */
s64 sched_kill_process(u32 pid, int sig);

/** sched_get_process_list() — Return head of global process list. */
process_t *sched_get_process_list(void);

/** Identity/credentials/address-space snapshot of a process, copied out
 * atomically under the scheduler lock. Lets a caller act on another process
 * without ever dereferencing a process_t another CPU may be freeing. */
struct proc_ident {
    u32         pid, ppid;
    u32         uid, gid, euid, egid, suid, sgid;
    phys_addr_t pml4_phys;
    u8          is_zombie;
};
/** sched_proc_ident(pid, out) — fill *out for the live process `pid`.
 * Returns false (leaving *out untouched) if there is no such live process. */
bool sched_proc_ident(u32 pid, struct proc_ident *out);

/** Scheduler lock helpers */
void sched_lock(void);
void sched_unlock(void);

/** Telemetry */
u32 sched_get_process_count(void);
u64 sched_get_idle_ticks(u32 cpu_id);
u64 sched_get_active_ticks(u32 cpu_id);
u64 sched_get_ticks(void);

/* ── Job-control stop / resume (kernel/ptrace.c drives the ptrace half) ──── */

/**
 * sched_stop_current() — park the calling thread until the process is resumed.
 *
 * Call only on a return path to ring 3, with proc->stop_state already non-zero.
 * The thread blocks in a loop, so a spurious wake (a signal delivered to a
 * stopped process, say) re-parks it rather than letting it escape the stop;
 * SIGKILL still gets out, because that marks the thread THREAD_DYING and the
 * loop tests for it.
 */
void sched_stop_current(void);

/**
 * sched_request_stop(p, sig, kind) — ask every thread of @p to park.
 *
 * Sets stop_state and pokes each thread so it reaches its next ring-3 exit:
 * blocked and sleeping threads are made runnable, threads mid-context-switch
 * get unblock_pending, and a thread running on another CPU is sent a
 * reschedule IPI. @kind is PROC_STOP_JOB or PROC_STOP_PTRACE.
 */
void sched_request_stop(process_t *p, int sig, u32 kind);

/**
 * sched_resume_process(p, cont_report) — undo a stop.
 *
 * Clears stop_state and wakes every parked thread. @cont_report asks for a
 * WCONTINUED report to the parent, which is what SIGCONT wants and what a
 * ptrace resume does not.  Returns true if @p was actually stopped.
 */
bool sched_resume_process(process_t *p, bool cont_report);

/**
 * sched_notify_parent(p, sig) — post @sig to @p's parent (and to its tracer,
 * when they differ) and wake anyone blocked in wait4(2) on it. Used to report
 * a stop, a resume and a ptrace-stop; the exit path has its own wakeup.
 */
void sched_notify_parent(process_t *p, int sig);

