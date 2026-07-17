/**
 * exec_server.h — AzamiOS Windows NT-Style Executive Subsystem Servers
 *
 * Defines ALPC Fast-Path Executive Servers (VFS, Proc, GUI) listening on LPC ports
 * to service UNIX ABI syscall requests with client-server isolation.
 */

#ifndef EXEC_SERVER_H
#define EXEC_SERVER_H

#include "lpc.h"

extern lpc_port_t *g_vfs_port;
extern lpc_port_t *g_proc_port;
extern lpc_port_t *g_gui_port;
extern lpc_port_t *g_compat32_port;

void exec_server_init(void);
int vfs_server_dispatch(lpc_msg_t *req, lpc_msg_t *reply);
int proc_server_dispatch(lpc_msg_t *req, lpc_msg_t *reply);
int gui_server_dispatch(lpc_msg_t *req, lpc_msg_t *reply);
int compat32_server_dispatch(lpc_msg_t *req, lpc_msg_t *reply);

#endif
