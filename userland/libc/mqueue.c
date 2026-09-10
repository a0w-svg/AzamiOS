/* ============================================================================
 * AzamiOS Userspace — POSIX Message Queues
 * File: userland/libc/mqueue.c
 *
 * Thin wrappers over the six mq_* system calls. The shape of the split is the
 * one POSIX chose and every libc repeats: the kernel provides the timed and
 * the get/set-in-one-call forms, and the library derives mq_send/mq_receive/
 * mq_getattr/mq_setattr from them so there is one blocking implementation
 * rather than four.
 * ============================================================================ */

#include "include/mqueue.h"
#include "include/sys/syscall.h"
#include "include/unistd.h"
#include "include/errno.h"
#include <stdarg.h>

static inline long __mq_ret(long r)
{
    if (r < 0) { errno = (int)-r; return -1; }
    return r;
}

mqd_t mq_open(const char *name, int oflag, ...)
{
    unsigned int mode = 0;
    struct mq_attr *attr = 0;

    /* mode and attr are only present — and only meaningful — with O_CREAT.
     * Reading them unconditionally would walk off the end of the call frame
     * for the two-argument form. */
    if (oflag & O_CREAT) {
        va_list ap;
        va_start(ap, oflag);
        mode = va_arg(ap, unsigned int);
        attr = va_arg(ap, struct mq_attr *);
        va_end(ap);
    }

    return (mqd_t)__mq_ret(syscall4(SYS_mq_open, (long)name, oflag,
                                    (long)mode, (long)attr));
}

int mq_close(mqd_t mqdes)
{
    /* An mqd_t is a file descriptor, so this is close(2) — and must be, or a
     * queue descriptor would leak past the point the program thinks it closed
     * it. There is no separate mq_close system call for exactly this reason. */
    return close((int)mqdes);
}

int mq_unlink(const char *name)
{
    return (int)__mq_ret(syscall1(SYS_mq_unlink, (long)name));
}

int mq_getsetattr(mqd_t mqdes, const struct mq_attr *newattr,
                  struct mq_attr *oldattr)
{
    return (int)__mq_ret(syscall3(SYS_mq_getsetattr, mqdes,
                                  (long)newattr, (long)oldattr));
}

int mq_getattr(mqd_t mqdes, struct mq_attr *attr)
{
    return mq_getsetattr(mqdes, 0, attr);
}

int mq_setattr(mqd_t mqdes, const struct mq_attr *newattr,
               struct mq_attr *oldattr)
{
    return mq_getsetattr(mqdes, newattr, oldattr);
}

int mq_timedsend(mqd_t mqdes, const char *msg_ptr, size_t msg_len,
                 unsigned int msg_prio, const struct timespec *abs_timeout)
{
    return (int)__mq_ret(syscall5(SYS_mq_timedsend, mqdes, (long)msg_ptr,
                                  (long)msg_len, (long)msg_prio,
                                  (long)abs_timeout));
}

int mq_send(mqd_t mqdes, const char *msg_ptr, size_t msg_len,
            unsigned int msg_prio)
{
    return mq_timedsend(mqdes, msg_ptr, msg_len, msg_prio, 0);
}

ssize_t mq_timedreceive(mqd_t mqdes, char *msg_ptr, size_t msg_len,
                        unsigned int *msg_prio,
                        const struct timespec *abs_timeout)
{
    return (ssize_t)__mq_ret(syscall5(SYS_mq_timedreceive, mqdes,
                                      (long)msg_ptr, (long)msg_len,
                                      (long)msg_prio, (long)abs_timeout));
}

ssize_t mq_receive(mqd_t mqdes, char *msg_ptr, size_t msg_len,
                   unsigned int *msg_prio)
{
    return mq_timedreceive(mqdes, msg_ptr, msg_len, msg_prio, 0);
}

int mq_notify(mqd_t mqdes, const struct sigevent *sevp)
{
    return (int)__mq_ret(syscall2(SYS_mq_notify, mqdes, (long)sevp));
}
