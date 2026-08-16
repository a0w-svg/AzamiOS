/* ============================================================================
 * AzamiOS Userspace — Move / Rename Files (mv.elf)
 * File: userland/apps/mv/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/sys/stat.h"
#include "../../libc/include/unistd.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: mv source destination\n");
        return 1;
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    char target[512];
    struct stat st;
    if (stat(dst, &st) == 0 && S_ISDIR(st.st_mode)) {
        const char *base = strrchr(src, '/');
        base = base ? base + 1 : src;
        snprintf(target, sizeof(target), "%s/%s", dst, base);
    } else {
        snprintf(target, sizeof(target), "%s", dst);
    }

    if (rename(src, target) == 0) {
        return 0;
    }

    /* Fallback: copy and unlink */
    int sfd = open(src, O_RDONLY, 0);
    if (sfd < 0) {
        printf("mv: cannot open '%s'\n", src);
        return 1;
    }

    int dfd = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dfd < 0) {
        printf("mv: cannot create '%s'\n", target);
        close(sfd);
        return 1;
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(sfd, buf, sizeof(buf))) > 0) {
        write(dfd, buf, n);
    }
    close(sfd);
    close(dfd);

    unlink(src);
    return 0;
}
