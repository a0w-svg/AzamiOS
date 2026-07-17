/**
 * lpc_client.h — AzamiOS Userspace ALPC / LPC Client Library
 *
 * Provides Windows NT-style ALPC communication primitives for user applications
 * to interact with Executive Subsystem Servers (VFS, Proc, GUI) with zero-copy
 * data routing and minimal context switch overhead.
 */

#ifndef _LPC_CLIENT_H
#define _LPC_CLIENT_H

#include <stdint.h>
#include <stdbool.h>

#define LPC_PORT_VFS       "\\Port\\VfsServer"
#define LPC_PORT_PROC      "\\Port\\ProcServer"
#define LPC_PORT_GUI       "\\Port\\GuiServer"
#define LPC_PORT_COMPAT32  "\\Port\\Compat32Server"

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
#define LPC_REQ_PROC_EXEC32 208

#define LPC_REQ_GUI_MAP     301
#define LPC_REQ_GUI_FLIP    302
#define LPC_REQ_GUI_RECT    303
#define LPC_REQ_GUI_TEXT    304
#define LPC_REQ_GUI_LINE    305
#define LPC_REQ_GUI_CIRCLE  306

#define LPC_REQ_COMPAT32_SYSCALL 401
#define LPC_REQ_COMPAT32_EXEC    402
#define LPC_REQ_COMPAT32_STAT    403

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

int lpc_connect(const char *port_name);
int lpc_send(int port_id, lpc_msg_t *req, lpc_msg_t *reply);
int lpc_send_zerocopy(int port_id, uint32_t req_type, void *section_ptr, uint32_t section_size, uint32_t arg1, uint32_t arg2, lpc_msg_t *reply);
int lpc_stat(char *buf, int max_len);

#endif
