#pragma once
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* /proc/mounts fields use octal escapes for spaces, tabs and backslashes.
 * Refuse overlong fields so callers never query a truncated mount path. */
typedef struct {
    char source[128];
    char path[256];
    char type[32];
    char options[128];
} az_mount_entry_t;

static inline int az_mount_field(const char **cursor, char *out, size_t capacity)
{
    const char *p = *cursor;
    while (*p == ' ' || *p == '\t') p++;
    size_t used = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            if (p[0] < '0' || p[0] > '7' || !p[1] || p[1] < '0' || p[1] > '7' ||
                !p[2] || p[2] < '0' || p[2] > '7') return -1;
            unsigned int value = (unsigned int)(p[0] - '0') * 64 +
                                 (unsigned int)(p[1] - '0') * 8 + (unsigned int)(p[2] - '0');
            if (value == 0 || value > 255) return -1;
            c = (unsigned char)value; p += 3;
        }
        if (used + 1 >= capacity) return -1;
        out[used++] = (char)c;
    }
    if (!used) return -1;
    out[used] = '\0'; *cursor = p;
    return 0;
}

static inline int az_mount_parse(const char *line, az_mount_entry_t *out)
{
    az_mount_entry_t entry;
    if (!line || !out || az_mount_field(&line, entry.source, sizeof(entry.source)) < 0 ||
        entry.source[0] == '#' || az_mount_field(&line, entry.path, sizeof(entry.path)) < 0 ||
        entry.path[0] != '/' || az_mount_field(&line, entry.type, sizeof(entry.type)) < 0 ||
        az_mount_field(&line, entry.options, sizeof(entry.options)) < 0) return -1;
    *out = entry;
    return 0;
}

static inline int az_mount_virtual(const az_mount_entry_t *entry)
{
    static const char *const types[] = {"proc", "procfs", "devfs", "devtmpfs", "sysfs",
        "devpts", "cgroup", "cgroup2", "sockfs", "pipefs", "anon_inodefs"};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        if (!strcmp(entry->type, types[i])) return 1;
    return 0;
}

static inline int az_mount_readonly(const az_mount_entry_t *entry)
{
    const char *p = entry->options;
    while (*p) {
        const char *end = strchr(p, ',');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        if (length == 2 && !strncmp(p, "ro", 2)) return 1;
        if (!end) break;
        p = end + 1;
    }
    return 0;
}

/* Stream the entire table, including short reads and a final unterminated
 * line. Virtual mounts are filtered; omitted counts malformed/oversized
 * records and records beyond capacity, rather than silently truncating them. */
static inline int az_mount_read_table(const char *path, az_mount_entry_t *entries,
                                       int capacity, int *omitted)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buffer[512], line[2048];
    size_t used = 0;
    int overflow = 0, count = 0, skipped = 0;
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { close(fd); return -1; }
        for (ssize_t i = 0; i <= n; i++) {
            if (i == n && n != 0) break;
            int end = n == 0 || buffer[i] == '\n';
            if (end) {
                line[used] = '\0';
                if (overflow) skipped++;
                else if (used && line[0] != '#') {
                    az_mount_entry_t entry;
                    if (az_mount_parse(line, &entry) < 0) skipped++;
                    else if (!az_mount_virtual(&entry)) {
                        if (count < capacity) entries[count++] = entry;
                        else skipped++;
                    }
                }
                used = 0; overflow = 0;
            } else if (used + 1 < sizeof(line)) line[used++] = buffer[i];
            else overflow = 1;
        }
        if (n == 0) break;
    }
    if (close(fd) < 0) return -1;
    if (omitted) *omitted = skipped;
    return count;
}
