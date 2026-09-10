/* ============================================================================
 * AzamiOS libc — <sys/shm.h>: System V shared memory (POSIX XSI)
 * ============================================================================ */
#ifndef _SYS_SHM_H
#define _SYS_SHM_H

#include <sys/ipc.h>
#include <sys/types.h>

/* shmat() flags */
#define SHM_RDONLY  010000   /* attach read-only                        */
#define SHM_RND     020000   /* round the address down to SHMLBA        */
#define SHM_REMAP   040000   /* replace any existing mapping            */

/* shmctl() commands beyond the IPC_* set */
#define SHM_LOCK    11
#define SHM_UNLOCK  12
#define SHM_STAT    13
#define SHM_INFO    14

/*
 * shmctl(0, IPC_INFO, &shminfo) reports the system limits; SHM_INFO fills a
 * struct shm_info with what is in use instead.  Both return the highest slot
 * index in use, which SHM_STAT can then walk.
 */
struct shminfo {
    unsigned long shmmax;   /* largest segment, bytes            */
    unsigned long shmmin;   /* smallest segment, bytes           */
    unsigned long shmmni;   /* max number of segments            */
    unsigned long shmseg;   /* max segments attached per process */
    unsigned long shmall;   /* max total shared pages            */
    unsigned long __unused1, __unused2, __unused3, __unused4;
};

struct shm_info {
    int           used_ids;        /* segments in existence       */
    unsigned long shm_tot;         /* total pages across segments */
    unsigned long shm_rss;         /* resident pages              */
    unsigned long shm_swp;         /* swapped pages (always 0)    */
    unsigned long swap_attempts;
    unsigned long swap_successes;
};

#define SHMLBA      4096     /* attach addresses are page-aligned       */

struct shmid_ds {
    struct ipc_perm shm_perm;
    size_t          shm_segsz;    /* size of the segment in bytes   */
    long            shm_atime;    /* last shmat() time              */
    long            shm_dtime;    /* last shmdt() time              */
    long            shm_ctime;    /* last change time               */
    int             shm_cpid;     /* pid of the creator             */
    int             shm_lpid;     /* pid of the last operator       */
    unsigned long   shm_nattch;   /* current number of attaches     */
    unsigned long   __unused4;
    unsigned long   __unused5;
};

/** shmget(key, size, shmflg) → segment identifier, or -1. */
int    shmget(key_t key, size_t size, int shmflg);

/** shmat(shmid, shmaddr, shmflg) → attach address, or (void *)-1. */
void  *shmat(int shmid, const void *shmaddr, int shmflg);

/** shmdt(shmaddr) — detach a segment from this process. */
int    shmdt(const void *shmaddr);

/** shmctl(shmid, cmd, buf) — IPC_STAT / IPC_SET / IPC_RMID and friends. */
int    shmctl(int shmid, int cmd, struct shmid_ds *buf);

#endif /* _SYS_SHM_H */
