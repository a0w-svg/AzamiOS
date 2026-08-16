/* ============================================================================
 * AzamiOS Userspace — Copy Files (cp.elf)
 * File: userland/apps/cp/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/sys/stat.h"
#include "../../libc/include/unistd.h"

static int copy_file(const char *src, const char *dst)
{
    int src_fd = open(src, O_RDONLY, 0);
    if (src_fd < 0) {
        printf("cp: cannot open '%s' for reading\n", src);
        return 1;
    }

    struct stat st;
    mode_t mode = 0644;
    if (fstat(src_fd, &st) == 0) {
        mode = st.st_mode & 0777;
    }

    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (dst_fd < 0) {
        printf("cp: cannot open '%s' for writing\n", dst);
        close(src_fd);
        return 1;
    }

    char buf[4096];
    ssize_t n;
    int err = 0;
    while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(dst_fd, buf + written, n - written);
            if (w <= 0) {
                printf("cp: write error\n");
                err = 1;
                break;
            }
            written += w;
        }
        if (err) break;
    }

    close(src_fd);
    close(dst_fd);
    return err;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: cp source destination\n");
        return 1;
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    struct stat st;
    if (stat(dst, &st) == 0 && S_ISDIR(st.st_mode)) {
        const char *base = strrchr(src, '/');
        base = base ? base + 1 : src;

        char final_dst[512];
        snprintf(final_dst, sizeof(final_dst), "%s/%s", dst, base);
        return copy_file(src, final_dst);
    }

    return copy_file(src, dst);
}
