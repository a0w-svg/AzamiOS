#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <gui.h>

void _start(void) {
    char modname[64] = {0};
    int arg_fd = open("cmd_args", 0);
    if (arg_fd >= 0) {
        int n = read(arg_fd, modname, sizeof(modname) - 1);
        if (n > 0) modname[n] = '\0';
        close(arg_fd);
    }

    char *p = modname;
    while (*p == ' ') p++;

    int out_fd = open("cmd_out", O_WRONLY | O_CREAT, 0);
    if (out_fd < 0) exit(1);

    if (*p == '\0') {
        const char *msg = "reload: usage: reload <module_name>\n";
        write(out_fd, msg, strlen(msg));
        close(out_fd);
        exit(0);
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "Reloading kernel module [%s] live without reboot...\n", p);
    write(out_fd, msg, strlen(msg));

    int res = sys_modreload(p);
    if (res == 0) {
        const char *succ = "SUCCESS: Module reloaded dynamically.\n";
        write(out_fd, succ, strlen(succ));
    } else {
        const char *err = "ERROR: Module reload failed or not found.\n";
        write(out_fd, err, strlen(err));
    }
    close(out_fd);
    exit(0);
}
