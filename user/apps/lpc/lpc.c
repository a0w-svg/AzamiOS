/**
 * lpc.c — AzamiOS ALPC Executive Subsystem Utility
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <lpc_client.h>

void _start(void) {
    char stat_buf[512];
    char out[1024];
    int n = lpc_stat(stat_buf, sizeof(stat_buf) - 1);
    if (n > 0) stat_buf[n] = '\0';
    else strcpy(stat_buf, "No active ALPC ports found.\n");

    int vfs_id  = lpc_connect(LPC_PORT_VFS);
    int proc_id = lpc_connect(LPC_PORT_PROC);
    int gui_id  = lpc_connect(LPC_PORT_GUI);

    snprintf(out, sizeof(out),
             "AzamiOS Windows NT Executive ALPC Subsystem Report:\n\n"
             "Connected Port Handles (O(1) Indexing):\n"
             "  [VfsServer]  Port ID: %d (Zero-Copy Section Capable)\n"
             "  [ProcServer] Port ID: %d (Zero-Copy Section Capable)\n"
             "  [GuiServer]  Port ID: %d (Zero-Copy Section Capable)\n\n"
             "Active Executive Ports Table:\n%s\n"
             "UNIX ABI Syscalls: Synchronously routed via ALPC Fast-Path!\n",
             vfs_id, proc_id, gui_id, stat_buf);

    int fd = open("cmd_out", O_WRONLY | O_CREAT, 0);
    if (fd >= 0) {
        write(fd, out, strlen(out));
        close(fd);
    }
    exit(0);
}
