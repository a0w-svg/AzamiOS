/* ============================================================================
 * AzamiOS — System V IPC (XSI): shared memory, semaphores, message queues
 * File: kernel/ipc/sysvipc.h
 *
 * The three XSI interprocess-communication mechanisms required by POSIX.1
 * (the XSI option), with the same identifier semantics Linux uses:
 *
 *   shmget/shmat/shmdt/shmctl   shared memory segments
 *   semget/semop/semctl         counting semaphore sets
 *   msgget/msgsnd/msgrcv/msgctl message queues
 *
 * Identifiers are Linux-compatible: an id encodes a slot index and a sequence
 * number (id = seq * IPCMNI + index), so an id from a destroyed object is
 * never mistaken for its successor in the same slot.  A key of IPC_PRIVATE
 * always creates a fresh object; any other key is looked up first, and
 * IPC_CREAT / IPC_EXCL decide what happens when it is or is not found.
 *
 * The 64-bit control structures (shmid64_ds, semid64_ds, msqid64_ds) match
 * the x86_64 Linux ABI byte for byte, so a libc that ORs IPC_64 into the
 * command — as glibc always does — gets exactly the layout it expects.
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../include/azami/defs.h"

struct process;

/* ── get() flags (low 9 bits are the permission mode) ────────────────────── */
#define IPC_CREAT   01000
#define IPC_EXCL    02000
#define IPC_NOWAIT  04000

#define IPC_PRIVATE 0

/* ── ctl() commands ──────────────────────────────────────────────────────── */
#define IPC_RMID    0
#define IPC_SET     1
#define IPC_STAT    2
#define IPC_INFO    3
#define IPC_64      0x0100   /* request the 64-bit structure layout */

/* shmctl-specific */
#define SHM_LOCK    11
#define SHM_UNLOCK  12
#define SHM_STAT    13
#define SHM_INFO    14
#define SHM_RDONLY  010000
#define SHM_RND     020000
#define SHM_REMAP   040000

/* semctl-specific */
#define GETPID      11
#define GETVAL      12
#define GETALL      13
#define GETNCNT     14
#define GETZCNT     15
#define SETVAL      16
#define SETALL      17
#define SEM_STAT    18
#define SEM_INFO    19
#define SEM_UNDO    0x1000

/* msgrcv/msgsnd-specific */
#define MSG_NOERROR 010000
#define MSG_EXCEPT  020000
#define MSG_STAT    11
#define MSG_INFO    12

/* ── Implementation limits ───────────────────────────────────────────────── */
#define IPCMNI          32768   /* slots addressable by one id (Linux value) */
#define SHM_MAX_SEGS    64
#define SHM_MAX_PAGES   4096    /* 16 MiB per segment                        */
#define SHM_MAX_ATTACH  256
#define SEM_MAX_SETS    64
#define SEM_MAX_NSEMS   256
#define SEM_MAX_OPS     64
#define SEM_MAX_UNDO    256
#define MSG_MAX_QUEUES  32
#define MSG_MAX_SIZE    8192    /* MSGMAX  */
#define MSG_MAX_BYTES   16384   /* MSGMNB  */

/* ── Linux x86_64 ABI structures ─────────────────────────────────────────── */

struct ipc64_perm {
    s32 key;
    u32 uid, gid, cuid, cgid;
    u32 mode;
    u16 seq;
    u16 __pad2;
    u32 __pad3;
    u64 __unused1, __unused2;
};

struct shmid64_ds {
    struct ipc64_perm shm_perm;
    u64 shm_segsz;
    s64 shm_atime;
    s64 shm_dtime;
    s64 shm_ctime;
    s32 shm_cpid;
    s32 shm_lpid;
    u64 shm_nattch;
    u64 __unused4, __unused5;
};

struct semid64_ds {
    struct ipc64_perm sem_perm;
    s64 sem_otime;
    u64 __unused1;
    s64 sem_ctime;
    u64 __unused2;
    u64 sem_nsems;
    u64 __unused3, __unused4;
};

struct msqid64_ds {
    struct ipc64_perm msg_perm;
    s64 msg_stime;
    u64 __unused1;
    s64 msg_rtime;
    u64 __unused2;
    s64 msg_ctime;
    u64 __unused3;
    u64 msg_cbytes;
    u64 msg_qnum;
    u64 msg_qbytes;
    s32 msg_lspid;
    s32 msg_lrpid;
    u64 __unused4, __unused5;
};

/* ── Structures returned by the IPC_INFO / *_INFO control commands ───────── */

/* shmctl(IPC_INFO) — the system's shared-memory limits. */
struct shminfo64 {
    u64 shmmax;      /* largest segment, bytes            */
    u64 shmmin;      /* smallest segment, bytes           */
    u64 shmmni;      /* max number of segments            */
    u64 shmseg;      /* max segments attached per process */
    u64 shmall;      /* max total shared pages            */
    u64 __unused1, __unused2, __unused3, __unused4;
};

/* shmctl(SHM_INFO) — what is actually in use right now. */
struct shm_info {
    s32 used_ids;
    u64 shm_tot;     /* total pages across all segments   */
    u64 shm_rss;     /* resident pages                    */
    u64 shm_swp;     /* swapped pages (always 0 here)     */
    u64 swap_attempts;
    u64 swap_successes;
};

/* semctl(IPC_INFO) and semctl(SEM_INFO) share one structure; SEM_INFO
 * repurposes semusz/semaem to report usage rather than limits, exactly as
 * Linux does. */
struct seminfo {
    s32 semmap, semmni, semmns, semmnu, semmsl;
    s32 semopm, semume, semusz, semvmx, semaem;
};

/* msgctl(IPC_INFO) and msgctl(MSG_INFO). */
struct msginfo {
    s32 msgpool, msgmap, msgmax, msgmnb, msgmni, msgssz, msgtql;
    u16 msgseg;
};

/* Largest value a semaphore may hold — Linux's SEMVMX. */
#define SEM_VALUE_MAX_XSI  32767

/* One element of the array handed to semop(). */
struct sembuf {
    u16 sem_num;
    s16 sem_op;
    s16 sem_flg;
};

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/** sysvipc_init() — reset every table.  Call once during boot. */
void sysvipc_init(void);

/**
 * sysvipc_process_fork(child, parent) — the child inherits the parent's
 * shared-memory attachments, and each segment's shm_nattch counts it.
 * Returns 0, or -ENOMEM if the attachment table cannot hold the copies (in
 * which case nothing has been changed and the fork should fail).
 */
int sysvipc_process_fork(struct process *child, struct process *parent);

/**
 * sysvipc_process_exec(proc) — drop the attachments across execve(), which
 * POSIX requires the new process image not to inherit.
 */
void sysvipc_process_exec(struct process *proc);

/**
 * sysvipc_process_exit(proc) — release a dying process's IPC state.
 *
 * Detaches its shared-memory attachments, applies its SEM_UNDO adjustments,
 * and drops any segment whose last attach just went away after IPC_RMID.
 */
void sysvipc_process_exit(struct process *proc);

/* ── Syscall entry points ────────────────────────────────────────────────── */
s64 sysv_shmget(s32 key, size_t size, int shmflg);
s64 sysv_shmat(int shmid, virt_addr_t shmaddr, int shmflg);
s64 sysv_shmdt(virt_addr_t shmaddr);
s64 sysv_shmctl(int shmid, int cmd, void *buf);

s64 sysv_semget(s32 key, int nsems, int semflg);
s64 sysv_semop(int semid, const void *tsops, size_t nsops);
/* semtimedop(2): as semop(), but give up with EAGAIN after `timeout_ticks`.
 * `has_timeout` false means block indefinitely, i.e. plain semop() semantics. */
s64 sysv_semtimedop(int semid, const void *tsops, size_t nsops,
                    u64 timeout_ticks, bool has_timeout);
s64 sysv_semctl(int semid, int semnum, int cmd, u64 arg);

s64 sysv_msgget(s32 key, int msgflg);
s64 sysv_msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg);
s64 sysv_msgrcv(int msqid, void *msgp, size_t msgsz, s64 msgtyp, int msgflg);
s64 sysv_msgctl(int msqid, int cmd, void *buf);

/* ── /proc/sysvipc rendering ─────────────────────────────────────────────── */
int sysvipc_proc_shm(char *buf, size_t len);
int sysvipc_proc_sem(char *buf, size_t len);
int sysvipc_proc_msg(char *buf, size_t len);
