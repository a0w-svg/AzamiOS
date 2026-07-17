/**
 * exec_server.c — AzamiOS Windows NT-Style Executive Subsystem Servers
 *
 * Implements ALPC Fast-Path Executive Servers (VFS, Proc, GUI) listening on LPC ports
 * to service UNIX ABI syscall requests with client-server isolation and zero-copy transfer.
 */

#include "include/exec_server.h"
#include "../filesystem/include/vfs.h"
#include "../drivers/include/gfx.h"
#include "../proc/include/process.h"
#include "../klibc/include/stdio.h"
#include "../klibc/include/string.h"
#include "../syscall/include/exec.h"

lpc_port_t *g_vfs_port = NULL;
lpc_port_t *g_proc_port = NULL;
lpc_port_t *g_gui_port = NULL;

void exec_server_init(void) {
    g_vfs_port  = lpc_create_port(LPC_PORT_VFS,  0, vfs_server_dispatch);
    g_proc_port = lpc_create_port(LPC_PORT_PROC, 0, proc_server_dispatch);
    g_gui_port  = lpc_create_port(LPC_PORT_GUI,  0, gui_server_dispatch);
    kprintf("exec_server: Windows NT-style Executive Subsystem Servers (VFS, Proc, GUI) active\n");
}

int vfs_server_dispatch(lpc_msg_t *req, lpc_msg_t *reply) {
    if (!req) return -1;
    int res = 0;
    switch (req->type) {
        case LPC_REQ_VFS_OPEN: {
            const char *path = req->section_ptr ? (const char*)req->section_ptr : req->data;
            vfs_file_t *f = vfs_open_file(path, req->arg1);
            if (reply) reply->arg1 = (uint32_t)(uintptr_t)f;
            res = f ? 0 : -1;
            break;
        }
        case LPC_REQ_VFS_READ: {
            vfs_file_t *f = (vfs_file_t*)(uintptr_t)req->arg1;
            uint8_t *buf = (uint8_t*)req->section_ptr;
            if (f && buf && req->section_size > 0) {
                res = vfs_file_read(f, req->section_size, buf);
                if (reply) reply->arg1 = (uint32_t)res;
            } else {
                res = -1;
                if (reply) reply->arg1 = 0;
            }
            break;
        }
        case LPC_REQ_VFS_WRITE: {
            vfs_file_t *f = (vfs_file_t*)(uintptr_t)req->arg1;
            uint8_t *buf = (uint8_t*)req->section_ptr;
            if (f && buf && req->section_size > 0) {
                res = vfs_file_write(f, req->section_size, buf);
                if (reply) reply->arg1 = (uint32_t)res;
            } else {
                res = -1;
                if (reply) reply->arg1 = 0;
            }
            break;
        }
        case LPC_REQ_VFS_CLOSE: {
            vfs_file_t *f = (vfs_file_t*)(uintptr_t)req->arg1;
            if (f) {
                vfs_file_close(f);
                res = 0;
            } else { res = -1; }
            break;
        }
        case LPC_REQ_VFS_MOUNT: {
            const char *dev = (const char*)req->section_ptr;
            const char *mnt = (const char*)(uintptr_t)req->arg1;
            const char *type = (const char*)(uintptr_t)req->arg2;
            res = vfs_mount(dev, mnt, type);
            break;
        }
        case LPC_REQ_VFS_UNMOUNT: {
            const char *mnt = (const char*)req->section_ptr;
            res = vfs_unmount(mnt);
            break;
        }
        default:
            res = -1;
    }
    return res;
}

int proc_server_dispatch(lpc_msg_t *req, lpc_msg_t *reply) {
    if (!req) return -1;
    int res = 0;
    switch (req->type) {
        case LPC_REQ_PROC_GETPID:
            if (reply) reply->arg1 = 1; /* Current PID */
            res = 1;
            break;
        case LPC_REQ_PROC_EXEC: {
            const char *filename = req->section_ptr ? (const char*)req->section_ptr : req->data;
            execute_program((char*)filename);
            res = 0;
            break;
        }
        case LPC_REQ_PROC_STAT:
            res = 0;
            break;
        default:
            res = -1;
    }
    return res;
}

int gui_server_dispatch(lpc_msg_t *req, lpc_msg_t *reply) {
    if (!req) return -1;
    int res = 0;
    switch (req->type) {
        case LPC_REQ_GUI_MAP:
            res = gfx_map_backbuffer();
            if (reply) reply->arg1 = (uint32_t)res;
            break;
        case LPC_REQ_GUI_FLIP:
            gfx_flip();
            res = 0;
            break;
        case LPC_REQ_GUI_RECT:
            gfx_draw_rect((int)req->arg1, (int)req->arg2, (int)req->arg3, (int)req->arg4, (uint32_t)req->section_ptr);
            res = 0;
            break;
        case LPC_REQ_GUI_TEXT:
            gfx_draw_text((int)req->arg1, (int)req->arg2, (const char*)req->section_ptr, (uint32_t)req->arg3, (uint32_t)req->arg4);
            res = 0;
            break;
        case LPC_REQ_GUI_LINE:
            gfx_draw_line((int)req->arg1, (int)req->arg2, (int)req->arg3, (int)req->arg4, (uint32_t)req->section_ptr);
            res = 0;
            break;
        case LPC_REQ_GUI_CIRCLE:
            gfx_draw_circle((int)req->arg1, (int)req->arg2, (int)req->arg3, (uint32_t)req->arg4);
            res = 0;
            break;
        default:
            res = -1;
    }
    return res;
}
