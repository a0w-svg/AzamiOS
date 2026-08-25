/* ============================================================================
 * AzamiOS Userland — POSIX file type classification utility
 * File: userland/apps/file/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>

static void classify_file(const char *path, int brief)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (!brief) printf("%s: ", path);
        printf("cannot open (No such file or directory)\n");
        return;
    }

    if (!brief) printf("%s: ", path);

    if (S_ISDIR(st.st_mode)) {
        printf("directory\n");
        return;
    }
    if (S_ISLNK(st.st_mode)) {
        char link_target[256];
        ssize_t len = readlink(path, link_target, sizeof(link_target) - 1);
        if (len > 0) {
            link_target[len] = '\0';
            printf("symbolic link to %s\n", link_target);
        } else {
            printf("symbolic link\n");
        }
        return;
    }
    if (S_ISCHR(st.st_mode)) {
        printf("character special (%u/%u)\n", (unsigned int)(st.st_rdev >> 8), (unsigned int)(st.st_rdev & 0xFF));
        return;
    }
    if (S_ISBLK(st.st_mode)) {
        printf("block special (%u/%u)\n", (unsigned int)(st.st_rdev >> 8), (unsigned int)(st.st_rdev & 0xFF));
        return;
    }
    if (S_ISFIFO(st.st_mode)) {
        printf("fifo (named pipe)\n");
        return;
    }
    if (S_ISSOCK(st.st_mode)) {
        printf("socket\n");
        return;
    }

    if (st.st_size == 0) {
        printf("empty\n");
        return;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("cannot open for reading\n");
        return;
    }

    unsigned char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);

    if (n <= 0) {
        printf("empty\n");
        return;
    }

    /* ELF Detection */
    if (n >= 4 && buf[0] == 0x7F && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F') {
        int is_64 = (n >= 5 && buf[4] == 2);
        int is_little = (n >= 6 && buf[5] == 1);
        uint16_t elf_type = (n >= 18) ? (buf[16] | (buf[17] << 8)) : 0;
        uint16_t machine  = (n >= 20) ? (buf[18] | (buf[19] << 8)) : 0;

        const char *type_str = "executable";
        if (elf_type == 1) type_str = "relocatable";
        else if (elf_type == 2) type_str = "executable";
        else if (elf_type == 3) type_str = "shared object";
        else if (elf_type == 4) type_str = "core dump";

        const char *arch_str = (machine == 0x3E) ? "x86-64" : (machine == 0x03 ? "Intel 80386" : "unknown architecture");

        printf("ELF %d-bit %s %s, %s, version 1 (SYSV)\n",
               is_64 ? 64 : 32,
               is_little ? "LSB" : "MSB",
               type_str,
               arch_str);
        return;
    }

    /* GZIP */
    if (n >= 2 && buf[0] == 0x1F && buf[1] == 0x8B) {
        printf("gzip compressed data, max compression, from Unix\n");
        return;
    }

    /* PNG */
    if (n >= 8 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G' &&
        buf[4] == 0x0D && buf[5] == 0x0A && buf[6] == 0x1A && buf[7] == 0x0A) {
        printf("PNG image data\n");
        return;
    }

    /* BMP */
    if (n >= 2 && buf[0] == 'B' && buf[1] == 'M') {
        printf("PC bitmap, Windows 3.x format\n");
        return;
    }

    /* TAR Archive */
    if (n >= 512 && memcmp(buf + 257, "ustar", 5) == 0) {
        printf("POSIX tar archive\n");
        return;
    }

    /* Script or Source Code Detection */
    if (n >= 2 && buf[0] == '#' && buf[1] == '!') {
        char line[128];
        size_t l = 0;
        while (l < sizeof(line) - 1 && l < (size_t)n && buf[l] != '\n' && buf[l] != '\r') {
            line[l] = (char)buf[l];
            l++;
        }
        line[l] = '\0';
        printf("%s script text executable\n", line + 2);
        return;
    }

    /* Check if plain text or source code */
    int is_ascii = 1;
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] < 0x09 || (buf[i] > 0x0D && buf[i] < 0x20) || buf[i] == 0x7F) {
            is_ascii = 0;
            break;
        }
    }

    if (is_ascii) {
        if (strstr((const char *)buf, "#include") != NULL || strstr((const char *)buf, "int main(") != NULL) {
            printf("C source, ASCII text\n");
        } else if (strstr((const char *)buf, "def ") != NULL || strstr((const char *)buf, "import ") != NULL) {
            printf("Python script, ASCII text\n");
        } else {
            printf("ASCII text\n");
        }
        return;
    }

    printf("data\n");
}

int main(int argc, char **argv)
{
    int brief = 0;
    int opt_ind = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--brief") == 0) {
            brief = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: file [options] <filename>...\n");
            printf("Options:\n");
            printf("  -b, --brief    Do not prepend filenames to output\n");
            printf("  -h, --help     Display this help and exit\n");
            return 0;
        } else if (argv[i][0] != '-') {
            opt_ind = i;
            break;
        }
    }

    if (opt_ind >= argc) {
        fprintf(stderr, "Usage: file [-b] <filename>...\n");
        return 1;
    }

    for (int i = opt_ind; i < argc; i++) {
        if (argv[i][0] == '-' && strlen(argv[i]) <= 2) continue;
        classify_file(argv[i], brief);
    }

    return 0;
}
