/* ============================================================================
 * AzamiOS Desktop & System — Persistent Configuration Storage Header
 * File: userland/apps/shared/sys_config.h
 *
 * Implements persistent configuration storage prioritizing SATA hard drive
 * (/hdd/etc, /hdd/config) with transparent fallback and synchronization to /etc.
 * ============================================================================ */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "../../libc/include/unistd.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/sys/stat.h"

static inline void az_config_init_storage(void)
{
    mkdir("/etc", 0755);
    const char *home = getenv("HOME");
    if (home && home[0]) {
        char dir[128];
        snprintf(dir, sizeof(dir), "%s/.config", home);
        mkdir(dir, 0755);
    }
}

static inline bool az_config_has_hdd(void)
{
    struct stat st;
    return (stat("/etc", &st) == 0 && S_ISDIR(st.st_mode));
}

static inline int az_config_read(const char *name, char *out_buf, size_t max_len)
{
    if (!name || !out_buf || max_len == 0) return -1;
    out_buf[0] = '\0';

    char path[256];
    int fd = -1;

    /* 1. Check user configuration: $HOME/.config/<name> */
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(path, sizeof(path), "%s/.config/%s", home, name);
        fd = open(path, O_RDONLY);
    }

    /* 2. Check system configuration: /etc/<name> */
    if (fd < 0) {
        snprintf(path, sizeof(path), "/etc/%s", name);
        fd = open(path, O_RDONLY);
    }

    /* 3. Fallback to /usr/share/azami/<name> or legacy /hdd/etc/<name> */
    if (fd < 0) {
        snprintf(path, sizeof(path), "/usr/share/azami/%s", name);
        fd = open(path, O_RDONLY);
    }
    if (fd < 0) {
        snprintf(path, sizeof(path), "/hdd/etc/%s", name);
        fd = open(path, O_RDONLY);
    }

    if (fd >= 0) {
        ssize_t n = read(fd, out_buf, max_len - 1);
        close(fd);
        if (n > 0) {
            out_buf[n] = '\0';
            while (n > 0 && (out_buf[n - 1] == '\r' || out_buf[n - 1] == '\n' || out_buf[n - 1] == ' ')) {
                out_buf[--n] = '\0';
            }
            return (int)n;
        }
    }
    return -1;
}

static inline int az_config_write(const char *name, const char *data, size_t len)
{
    if (!name || !data) return -1;
    if (len == 0) len = strlen(data);

    az_config_init_storage();

    char path[256];

    /* 1. Write to standard system-wide /etc/<name> */
    snprintf(path, sizeof(path), "/etc/%s", name);
    int fd_etc = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_etc >= 0) {
        write(fd_etc, data, len);
        if (data[len - 1] != '\n') write(fd_etc, "\n", 1);
        close(fd_etc);
    }

    /* 2. If user home is set, also update user $HOME/.config/<name> */
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(path, sizeof(path), "%s/.config/%s", home, name);
        int fd_usr = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_usr >= 0) {
            write(fd_usr, data, len);
            if (data[len - 1] != '\n') write(fd_usr, "\n", 1);
            close(fd_usr);
        }
    }

    return 0;
}
