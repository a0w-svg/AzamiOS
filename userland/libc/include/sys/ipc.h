/* ============================================================================
 * AzamiOS libc — <sys/ipc.h>: common System V IPC definitions (POSIX XSI)
 * ============================================================================ */
#ifndef _SYS_IPC_H
#define _SYS_IPC_H

#include <sys/types.h>

typedef int key_t;

/* Permission and ownership shared by all three IPC mechanisms.  This is the
 * 64-bit layout the kernel uses; libc always requests it via IPC_64. */
struct ipc_perm {
    key_t          __key;
    unsigned int   uid;
    unsigned int   gid;
    unsigned int   cuid;
    unsigned int   cgid;
    unsigned int   mode;
    unsigned short __seq;
    unsigned short __pad2;
    unsigned int   __pad3;
    unsigned long  __unused1;
    unsigned long  __unused2;
};

/* Mode bits for the *get() calls; the low nine bits are the permissions. */
#define IPC_CREAT   01000
#define IPC_EXCL    02000
#define IPC_NOWAIT  04000

/* Reserved key: always creates a new, unnamed object. */
#define IPC_PRIVATE ((key_t)0)

/* Control commands. */
#define IPC_RMID    0
#define IPC_SET     1
#define IPC_STAT    2
#define IPC_INFO    3

/* Requests the 64-bit structure layout; libc ORs this into every command. */
#define IPC_64      0x0100

/**
 * ftok(pathname, proj_id) → a key derived from a file's identity.
 *
 * Returns -1 if the path cannot be stat()ed, so unrelated programs agreeing
 * on a filename agree on a key.
 */
key_t ftok(const char *pathname, int proj_id);

#endif /* _SYS_IPC_H */
