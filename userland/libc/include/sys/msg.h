/* ============================================================================
 * AzamiOS libc — <sys/msg.h>: System V message queues (POSIX XSI)
 * ============================================================================ */
#ifndef _SYS_MSG_H
#define _SYS_MSG_H

#include <sys/ipc.h>
#include <sys/types.h>

/* msgrcv()/msgsnd() flags */
#define MSG_NOERROR 010000   /* truncate instead of failing with E2BIG */
#define MSG_EXCEPT  020000   /* take the first message *not* of msgtyp */

/* msgctl() commands beyond the IPC_* set */
#define MSG_STAT    11
#define MSG_INFO    12

/*
 * msgctl(0, IPC_INFO, &msginfo) reports the system limits; MSG_INFO reuses the
 * same structure for current usage, putting the number of queues in msgpool,
 * the number of queued messages in msgmap and the queued bytes in msgtql.
 * Both return the highest slot index in use, for MSG_STAT to walk.
 */
struct msginfo {
    int msgpool;   /* limit: buffer pool size / usage: queues in use    */
    int msgmap;    /* limit: map entries      / usage: messages queued  */
    int msgmax;    /* max bytes in one message                          */
    int msgmnb;    /* max bytes in one queue                            */
    int msgmni;    /* max number of queues                              */
    int msgssz;    /* message segment size                              */
    int msgtql;    /* limit: max queued bytes / usage: bytes queued     */
    unsigned short msgseg;
};

struct msqid_ds {
    struct ipc_perm msg_perm;
    long            msg_stime;    /* last msgsnd() time            */
    unsigned long   __unused1;
    long            msg_rtime;    /* last msgrcv() time            */
    unsigned long   __unused2;
    long            msg_ctime;    /* last change time              */
    unsigned long   __unused3;
    unsigned long   msg_cbytes;   /* bytes currently queued        */
    unsigned long   msg_qnum;     /* messages currently queued     */
    unsigned long   msg_qbytes;   /* maximum bytes allowed         */
    int             msg_lspid;    /* pid of the last msgsnd()      */
    int             msg_lrpid;    /* pid of the last msgrcv()      */
    unsigned long   __unused4;
    unsigned long   __unused5;
};

/* The shape every message must start with; mtext is whatever follows. */
struct msgbuf {
    long mtype;
    char mtext[1];
};

/** msgget(key, msgflg) → message queue identifier, or -1. */
int     msgget(key_t key, int msgflg);

/** msgsnd(msqid, msgp, msgsz, msgflg) — append a message. */
int     msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg);

/**
 * msgrcv(msqid, msgp, msgsz, msgtyp, msgflg) → bytes received, or -1.
 *
 * @msgtyp of 0 takes the first message, a positive value takes the first of
 * exactly that type, and a negative value takes the lowest type ≤ |msgtyp|.
 */
ssize_t msgrcv(int msqid, void *msgp, size_t msgsz, long msgtyp, int msgflg);

/** msgctl(msqid, cmd, buf) — IPC_STAT / IPC_SET / IPC_RMID. */
int     msgctl(int msqid, int cmd, struct msqid_ds *buf);

#endif /* _SYS_MSG_H */
