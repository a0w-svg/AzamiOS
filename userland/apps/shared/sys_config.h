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
#include "../../libc/include/fcntl.h"
#include "../../libc/include/sys/stat.h"

static inline void az_config_init_storage(void)
{
    struct stat st;
    if (stat("/hdd", &st) == 0 && S_ISDIR(st.st_mode)) {
        mkdir("/hdd/etc", 0755);
        mkdir("/hdd/config", 0755);
    }
}

static inline bool az_config_has_hdd(void)
{
    struct stat st;
    return (stat("/hdd/etc", &st) == 0 && S_ISDIR(st.st_mode));
}

static inline int az_config_read(const char *name, char *out_buf, size_t max_len)
{
    if (!name || !out_buf || max_len == 0) return -1;
    out_buf[0] = '\0';

    char path[256];

    /* 1. Check /hdd/config/ */
    snprintf(path, sizeof(path), "/hdd/config/%s", name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        /* 2. Check /hdd/etc/ */
        snprintf(path, sizeof(path), "/hdd/etc/%s", name);
        fd = open(path, O_RDONLY);
    }
    if (fd < 0) {
        /* 3. Fallback to /etc/ */
        snprintf(path, sizeof(path), "/etc/%s", name);
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

    /* Write to /hdd/etc/<name> or /hdd/config/<name> */
    if (az_config_has_hdd()) {
        snprintf(path, sizeof(path), "/hdd/etc/%s", name);
        int fd_hdd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_hdd >= 0) {
            write(fd_hdd, data, len);
            if (data[len - 1] != '\n') write(fd_hdd, "\n", 1);
            close(fd_hdd);
        }
    }

    /* Synchronize to /etc/<name> */
    snprintf(path, sizeof(path), "/etc/%s", name);
    int fd_etc = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_etc >= 0) {
        write(fd_etc, data, len);
        if (data[len - 1] != '\n') write(fd_etc, "\n", 1);
        close(fd_etc);
    }

    return 0;
}
