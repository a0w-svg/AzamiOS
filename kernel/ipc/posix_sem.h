/* ============================================================================
 * AzamiOS — POSIX Named Semaphores (sem_open / sem_post / sem_wait / ...)
 * File: kernel/ipc/posix_sem.h
 *
 * Named semaphores are identified by a NUL-terminated path string (like
 * "/mysem") and persist across fork() / exec() via the kernel table until
 * the last open handle is sem_close()'d and the name has been sem_unlink()'d.
 *
 * The implementation is fd-based: sem_open() returns a file descriptor that
 * userland passes back to sem_post(), sem_wait(), sem_timedwait(),
 * sem_trywait(), sem_getvalue(), and sem_close().  sem_unlink() takes the
 * name directly.
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../sched/sched.h"
#include "../../arch/x86_64/cpu/spinlock.h"

#define POSIX_SEM_NAME_MAX  64
#define POSIX_SEM_TABLE_MAX 128
#define SEM_VALUE_MAX       32767

#define SEM_O_CREAT   0x040
#define SEM_O_EXCL    0x080

typedef struct posix_sem {
    char        name[POSIX_SEM_NAME_MAX];
    u32         refcount;
    bool        unlinked;
    int         value;
    spinlock_t  lock;
    thread_t   *wait_head;
    thread_t   *wait_tail;
} posix_sem_t;

void          posix_sem_init(void);
posix_sem_t  *posix_sem_open_kern(const char *name, int oflag,
                                  unsigned int mode, unsigned int value);
void          posix_sem_put(posix_sem_t *sem);
int           posix_sem_unlink(const char *name);
int           posix_sem_post(posix_sem_t *sem);
int           posix_sem_wait(posix_sem_t *sem);
int           posix_sem_trywait(posix_sem_t *sem);
int           posix_sem_timedwait(posix_sem_t *sem, u64 abs_timeout_ns);
int           posix_sem_getvalue(posix_sem_t *sem, int *sval);
