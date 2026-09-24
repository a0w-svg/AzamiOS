#ifndef _UAPI_FANOTIFY_H
#define _UAPI_FANOTIFY_H

#include "../types.h"

/* Fanotify init flags */
#define FAN_CLASS_NOTIF         0x00000000
#define FAN_CLASS_CONTENT       0x00000004
#define FAN_CLASS_PRE_CONTENT   0x00000008
#define FAN_CLOEXEC             0x00000001
#define FAN_NONBLOCK            0x00000002
#define FAN_UNLIMITED_QUEUE     0x00000010
#define FAN_UNLIMITED_MARKS     0x00000020
#define FAN_ENABLE_AUDIT        0x00000040
#define FAN_REPORT_TID          0x00000100
#define FAN_REPORT_FID          0x00000200
#define FAN_REPORT_DIR_FID      0x00000400
#define FAN_REPORT_NAME         0x00000800

/* Fanotify mark flags */
#define FAN_MARK_ADD            0x00000001
#define FAN_MARK_REMOVE         0x00000002
#define FAN_MARK_DONT_FOLLOW    0x00000004
#define FAN_MARK_ONLYDIR        0x00000008
#define FAN_MARK_IGNORED_MASK   0x00000020
#define FAN_MARK_IGNORED_SURV_MODIFY 0x00000040
#define FAN_MARK_FLUSH          0x00000080
#define FAN_MARK_INODE          0x00000000
#define FAN_MARK_MOUNT          0x00000010
#define FAN_MARK_FILESYSTEM     0x00000100

/* Fanotify events */
#define FAN_ACCESS              0x00000001
#define FAN_MODIFY              0x00000002
#define FAN_ATTRIB              0x00000004
#define FAN_CLOSE_WRITE         0x00000008
#define FAN_CLOSE_NOWRITE       0x00000010
#define FAN_OPEN                0x00000020
#define FAN_MOVED_FROM          0x00000040
#define FAN_MOVED_TO            0x00000080
#define FAN_CREATE              0x00000100
#define FAN_DELETE              0x00000200
#define FAN_DELETE_SELF         0x00000400
#define FAN_MOVE_SELF           0x00000800
#define FAN_OPEN_EXEC           0x00001000

#define FAN_Q_OVERFLOW          0x00004000
#define FAN_FS_ERROR            0x00008000

#define FAN_OPEN_PERM           0x00010000
#define FAN_ACCESS_PERM         0x00020000
#define FAN_OPEN_EXEC_PERM      0x00040000

#define FAN_EVENT_ON_CHILD      0x08000000
#define FAN_ONDIR               0x40000000

#define FAN_CLOSE               (FAN_CLOSE_WRITE | FAN_CLOSE_NOWRITE)

struct fanotify_event_metadata {
    u32 event_len;
    u8 vers;
    u8 reserved;
    u16 metadata_len;
    u64 mask;
    s32 fd;
    s32 pid;
};

struct fanotify_response {
    s32 fd;
    u32 response;
};

#define FAN_ALLOW       0x01
#define FAN_DENY        0x02
#define FAN_AUDIT       0x10

#define FAN_NOFD        -1

#endif /* _UAPI_FANOTIFY_H */
