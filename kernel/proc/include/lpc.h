/**
 * lpc.h — AzamiOS Windows NT-Style Local Procedure Call (LPC / ALPC) Subsystem
 *
 * Implements Executive Client-Server Port Architecture with ALPC Fast-Path
 * zero-copy synchronous handoff for high-performance microkernel IPC.
 */

#ifndef LPC_H
#define LPC_H

#include <stdint.h>
#include <stdbool.h>
#include "process.h"

/* ── Standard Executive Port Names ──────────────────────────────────────── */
#define LPC_PORT_VFS       "\\Port\\VfsServer"
#define LPC_PORT_PROC      "\\Port\\ProcServer"
#define LPC_PORT_GUI       "\\Port\\GuiServer"

/* ── LPC Message Types ──────────────────────────────────────────────────── */
#define LPC_REQ_VFS_OPEN    101
#define LPC_REQ_VFS_READ    102
#define LPC_REQ_VFS_WRITE   103
#define LPC_REQ_VFS_CLOSE   104
#define LPC_REQ_VFS_STAT    105
#define LPC_REQ_VFS_MOUNT   106
#define LPC_REQ_VFS_UNMOUNT 107
#define LPC_REQ_VFS_LSEEK   108

#define LPC_REQ_PROC_STAT   201
#define LPC_REQ_PROC_GETPID 202
#define LPC_REQ_PROC_EXEC   203
#define LPC_REQ_PROC_KILL   204
#define LPC_REQ_PROC_FORK   205
#define LPC_REQ_PROC_YIELD  206
#define LPC_REQ_PROC_EXIT   207

#define LPC_REQ_GUI_MAP     301
#define LPC_REQ_GUI_FLIP    302
#define LPC_REQ_GUI_RECT    303
#define LPC_REQ_GUI_TEXT    304
#define LPC_REQ_GUI_LINE    305
#define LPC_REQ_GUI_CIRCLE  306

#define LPC_MAX_MSGS  32
#define LPC_MAX_PORTS 16

/* ── LPC Message Structure (ALPC Zero-Copy Section Capable) ─────────────── */
typedef struct {
    uint32_t  msg_id;
    uint32_t  sender_pid;
    uint32_t  type;
    uint32_t  arg1;
    uint32_t  arg2;
    uint32_t  arg3;
    uint32_t  arg4;
    uintptr_t section_ptr;  /* ALPC Zero-Copy Shared Memory Section View pointer */
    uint32_t  section_size; /* ALPC Section View buffer size in bytes */
    int32_t   status;
    char      data[128];
} lpc_msg_t;

/* ── LPC Port Structure ─────────────────────────────────────────────────── */
typedef struct lpc_port {
    char     name[32];
    uint32_t port_id;
    uint32_t owner_pid;
    bool     active;
    volatile int lock;
    lpc_msg_t queue[LPC_MAX_MSGS];
    int      head;
    int      tail;
    int      count;
    /* ALPC Fast-Path handler for immediate zero-copy executive dispatch */
    int      (*fast_handler)(lpc_msg_t *req, lpc_msg_t *reply);
} lpc_port_t;

/* ── LPC Executive Subsystem API ────────────────────────────────────────── */
void lpc_init(void);
lpc_port_t* lpc_create_port(const char *name, uint32_t owner_pid, int (*handler)(lpc_msg_t*, lpc_msg_t*));
lpc_port_t* lpc_connect_port(const char *name);
lpc_port_t* lpc_get_port_by_id(uint32_t port_id);
int lpc_send_request(lpc_port_t *port, lpc_msg_t *req, lpc_msg_t *reply);
int lpc_receive(lpc_port_t *port, lpc_msg_t *req);
int lpc_reply(lpc_port_t *port, lpc_msg_t *reply);
int lpc_get_status_table(char *buf, int max_len);

#endif
