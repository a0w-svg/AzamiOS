/* ============================================================================
 * AzamiOS — CFS Scheduler & Process/Thread Management Header
 * File: kernel/sched/sched.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
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

typedef struct __attribute__((aligned(16))) {
    u8 buffer[512];
} fpu_state_t;

/**
 * struct thread — Microkernel execution unit.
 */
typedef struct thread {
    u32             tid;             /* Thread ID */
    struct process *proc;            /* Owning process */
    u64             kernel_rsp;      /* Saved kernel stack pointer on context switch */
    u64             kernel_stack_top;/* Top of dedicated kernel stack for ring-0 transitions */
    thread_state_t  state;           /* Thread state */
    bool            unblock_pending; /* Signal from sched_unblock during context switch */
    u64             vruntime;        /* Completely Fair Scheduler virtual runtime */
    u32             priority;        /* Thread priority (weight modifier for CFS) */
    u32             cpu_id;          /* Currently assigned logical CPU */
    pt_regs_t      *user_regs;       /* Saved user frame during syscalls/interrupts */
    u64             sleep_end_ticks; /* Ticks when sleeping should end */
    fpu_state_t     fpu_state;       /* 512-byte FXSAVE/FXRSTOR area */
    struct thread  *next;            /* Ready queue / list pointer */
    struct thread  *proc_next;       /* Next thread in the same process */
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
    void           *handle_table[64];      /* VFS file descriptor table */
    u8              fd_flags[64];          /* File descriptor flags (FD_CLOEXEC) */
    az_object_t    *obj_handle_table[64];  /* Object Manager handle table */
    proc_shmem_map_t shmem_maps[MAX_SHMEM_PER_PROC]; /* Shared memory mappings */
    virt_addr_t     heap_start;            /* Base of user heap for brk */
    virt_addr_t     heap_end;              /* Current break address for brk */
    virt_addr_t     mmap_current;          /* Current bump pointer for anonymous mmap */
    int             exit_code;             /* Exit status code */
    bool            is_zombie;             /* Process terminated, awaiting waitpid */
    struct thread  *wait_thread;          /* Parent thread waiting on child exit */
    char            cwd[256];              /* Current working directory */
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
    u64             fs_base;               /* x86_64 FS_BASE (User TLS) */
    u64             gs_base;               /* x86_64 GS_BASE (User TLS) */
} process_t;

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

/** sched_exit_thread() — Terminate current thread and reschedule (never returns). */
__noreturn void sched_exit_thread(void);

/** sched_kill_process(pid, sig) — Send signal to process or terminate it. */
s64 sched_kill_process(u32 pid, int sig);

/** sched_get_process_list() — Return head of global process list. */
process_t *sched_get_process_list(void);

/** Scheduler lock helpers */
void sched_lock(void);
void sched_unlock(void);

/** Telemetry */
u32 sched_get_process_count(void);
u64 sched_get_idle_ticks(u32 cpu_id);
u64 sched_get_active_ticks(u32 cpu_id);
u64 sched_get_ticks(void);
