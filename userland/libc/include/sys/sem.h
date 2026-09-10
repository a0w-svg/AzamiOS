/* ============================================================================
 * AzamiOS libc — <sys/sem.h>: System V semaphores (POSIX XSI)
 * ============================================================================ */
#ifndef _SYS_SEM_H
#define _SYS_SEM_H

#include <sys/ipc.h>
#include <time.h>

/* semop() flags */
#define SEM_UNDO    0x1000   /* undo this adjustment if the process dies */

/* semctl() commands beyond the IPC_* set */
#define GETPID      11
#define GETVAL      12
#define GETALL      13
#define GETNCNT     14
#define GETZCNT     15
#define SETVAL      16
#define SETALL      17
#define SEM_STAT    18
#define SEM_INFO    19

struct semid_ds {
    struct ipc_perm sem_perm;
    long            sem_otime;    /* last semop() time            */
    unsigned long   __unused1;
    long            sem_ctime;    /* last change time             */
    unsigned long   __unused2;
    unsigned long   sem_nsems;    /* semaphores in the set        */
    unsigned long   __unused3;
    unsigned long   __unused4;
};

/* One element of the array passed to semop(). */
struct sembuf {
    unsigned short sem_num;   /* index within the set                    */
    short          sem_op;    /* <0 wait-and-take, >0 give, 0 wait-for-0 */
    short          sem_flg;   /* IPC_NOWAIT and/or SEM_UNDO              */
};

/* Largest value a semaphore may hold (SEMVMX). */
#define SEMVMX      32767

/*
 * semctl(0, 0, IPC_INFO, &seminfo) reports the system limits; SEM_INFO reuses
 * the same structure to report current usage, putting the number of sets in
 * semusz and the total number of semaphores in semaem.  Both return the
 * highest slot index in use, which SEM_STAT can then walk.
 */
struct seminfo {
    int semmap;   /* obsolete, retained for layout compatibility */
    int semmni;   /* max semaphore sets                          */
    int semmns;   /* max semaphores system-wide                  */
    int semmnu;   /* max undo structures                         */
    int semmsl;   /* max semaphores per set                      */
    int semopm;   /* max operations per semop() call             */
    int semume;   /* max undo entries per process                */
    int semusz;   /* size of the undo structure / sets in use    */
    int semvmx;   /* max semaphore value                         */
    int semaem;   /* max adjust-on-exit value / semaphores in use */
};

/* The fourth argument to semctl(), passed by value. */
union semun {
    int              val;
    struct semid_ds *buf;
    unsigned short  *array;
    void            *__pad;
};

/** semget(key, nsems, semflg) → semaphore set identifier, or -1. */
int semget(key_t key, int nsems, int semflg);

/**
 * semop(semid, sops, nsops) — apply an array of operations atomically.
 *
 * Either every operation in @sops can proceed, or the call blocks (or fails
 * with EAGAIN under IPC_NOWAIT) with none of them applied.
 */
int semop(int semid, struct sembuf *sops, size_t nsops);

/**
 * semtimedop(semid, sops, nsops, timeout) — semop() with a bound on the wait.
 *
 * Identical to semop(), except that if the operations cannot proceed within
 * @timeout the call fails with EAGAIN instead of blocking indefinitely.  A
 * NULL @timeout means no bound, i.e. exactly semop().
 */
int semtimedop(int semid, struct sembuf *sops, size_t nsops,
               const struct timespec *timeout);

/** semctl(semid, semnum, cmd, ...) — query or modify a set. */
int semctl(int semid, int semnum, int cmd, ...);

#endif /* _SYS_SEM_H */
