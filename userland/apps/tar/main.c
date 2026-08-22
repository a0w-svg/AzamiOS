/* ============================================================================
 * AzamiOS Userspace — POSIX Tar Archive Utility (tar.elf)
 * File: userland/apps/tar/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/sys/stat.h"
#include "../../libc/include/dirent.h"

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

static void compute_checksum(struct tar_header *h)
{
    memset(h->chksum, ' ', 8);
    unsigned int sum = 0;
    const unsigned char *p = (const unsigned char *)h;
    for (size_t i = 0; i < sizeof(struct tar_header); i++) {
        sum += p[i];
    }
    snprintf(h->chksum, 8, "%06o", sum);
    h->chksum[6] = '\0';
    h->chksum[7] = ' ';
}

static unsigned long parse_octal(const char *p, size_t len)
{
    unsigned long val = 0;
    while (len > 0 && (*p == ' ' || *p == '\0')) { p++; len--; }
    while (len > 0 && *p >= '0' && *p <= '7') {
        val = (val << 3) + (*p - '0');
        p++;
        len--;
    }
    return val;
}

static int tar_list(const char *tarfile, int verbose)
{
    int fd = (strcmp(tarfile, "-") == 0) ? 0 : open(tarfile, O_RDONLY);
    if (fd < 0) {
        printf("tar: cannot open %s\n", tarfile);
        return 1;
    }

    struct tar_header h;
    while (read(fd, &h, sizeof(h)) == sizeof(h)) {
        if (h.name[0] == '\0') break;

        unsigned long size = parse_octal(h.size, 12);
        if (verbose) {
            unsigned long mode = parse_octal(h.mode, 8);
            printf("%c%c%c%c%c%c%c%c%c%c %8lu %s\n",
                   (h.typeflag == '5') ? 'd' : '-',
                   (mode & 0400) ? 'r' : '-', (mode & 0200) ? 'w' : '-', (mode & 0100) ? 'x' : '-',
                   (mode & 0040) ? 'r' : '-', (mode & 0020) ? 'w' : '-', (mode & 0010) ? 'x' : '-',
                   (mode & 0004) ? 'r' : '-', (mode & 0002) ? 'w' : '-', (mode & 0001) ? 'x' : '-',
                   size, h.name);
        } else {
            printf("%s\n", h.name);
        }

        /* Skip file data blocks */
        unsigned long blocks = (size + 511) / 512;
        lseek(fd, (off_t)(blocks * 512), SEEK_CUR);
    }

    if (fd != 0) close(fd);
    return 0;
}

static int tar_extract(const char *tarfile, int verbose)
{
    int fd = (strcmp(tarfile, "-") == 0) ? 0 : open(tarfile, O_RDONLY);
    if (fd < 0) {
        printf("tar: cannot open %s\n", tarfile);
        return 1;
    }

    struct tar_header h;
    while (read(fd, &h, sizeof(h)) == sizeof(h)) {
        if (h.name[0] == '\0') break;

        unsigned long size = parse_octal(h.size, 12);
        if (verbose) printf("x %s\n", h.name);

        if (h.typeflag == '5' || h.name[strlen(h.name) - 1] == '/') {
            mkdir(h.name, 0755);
        } else {
            int out_fd = open(h.name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (out_fd >= 0) {
                unsigned long remaining = size;
                char buf[512];
                while (remaining > 0) {
                    size_t to_read = (remaining > 512) ? 512 : remaining;
                    read(fd, buf, 512);
                    write(out_fd, buf, to_read);
                    remaining -= to_read;
                }
                close(out_fd);
            } else {
                /* Skip data if couldn't open */
                unsigned long blocks = (size + 511) / 512;
                lseek(fd, (off_t)(blocks * 512), SEEK_CUR);
            }
        }
    }

    if (fd != 0) close(fd);
    return 0;
}

static int add_file_to_tar(int tar_fd, const char *path, int verbose)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        printf("tar: %s: Cannot stat\n", path);
        return -1;
    }

    struct tar_header h;
    memset(&h, 0, sizeof(h));
    strncpy(h.name, path, 99);
    snprintf(h.mode, 8, "%07o", (unsigned int)(st.st_mode & 0777));
    snprintf(h.uid, 8, "%07o", (unsigned int)st.st_uid);
    snprintf(h.gid, 8, "%07o", (unsigned int)st.st_gid);
    snprintf(h.size, 12, "%011o", (unsigned int)st.st_size);
    snprintf(h.mtime, 12, "%011o", (unsigned int)st.st_mtime);
    memcpy(h.magic, "ustar", 6);
    memcpy(h.version, "00", 2);
    strcpy(h.uname, "root");
    strcpy(h.gname, "root");

    if (S_ISDIR(st.st_mode)) {
        h.typeflag = '5';
        if (h.name[strlen(h.name) - 1] != '/') strcat(h.name, "/");
        compute_checksum(&h);
        write(tar_fd, &h, sizeof(h));
        if (verbose) printf("a %s\n", h.name);

        DIR *d = opendir(path);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
                char subpath[256];
                snprintf(subpath, sizeof(subpath), "%s/%s", path, ent->d_name);
                add_file_to_tar(tar_fd, subpath, verbose);
            }
            closedir(d);
        }
    } else {
        h.typeflag = '0';
        compute_checksum(&h);
        write(tar_fd, &h, sizeof(h));
        if (verbose) printf("a %s\n", h.name);

        int in_fd = open(path, O_RDONLY);
        if (in_fd >= 0) {
            char buf[512];
            ssize_t n;
            while ((n = read(in_fd, buf, 512)) > 0) {
                if (n < 512) memset(buf + n, 0, 512 - (size_t)n);
                write(tar_fd, buf, 512);
            }
            close(in_fd);
        }
    }
    return 0;
}

static int tar_create(const char *tarfile, int argc, char **argv, int start_idx, int verbose)
{
    int fd = (strcmp(tarfile, "-") == 0) ? 1 : open(tarfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("tar: cannot create %s\n", tarfile);
        return 1;
    }

    for (int i = start_idx; i < argc; i++) {
        add_file_to_tar(fd, argv[i], verbose);
    }

    /* Write 1024 zero bytes as end-of-archive marker */
    char zero[1024];
    memset(zero, 0, sizeof(zero));
    write(fd, zero, sizeof(zero));

    if (fd != 1) close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        puts("Usage: tar [-c|-x|-t] [-v] [-f archive.tar] [files...]");
        puts("Options:");
        puts("  -c  Create archive");
        puts("  -x  Extract archive");
        puts("  -t  List archive contents");
        puts("  -v  Verbose output");
        puts("  -f  Archive file name (or '-' for stdout/stdin)");
        return 1;
    }

    int mode = 0; /* 'c', 'x', 't' */
    int verbose = 0;
    const char *tarfile = NULL;
    int files_start = argc;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (size_t j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'c') mode = 'c';
                else if (argv[i][j] == 'x') mode = 'x';
                else if (argv[i][j] == 't') mode = 't';
                else if (argv[i][j] == 'v') verbose = 1;
                else if (argv[i][j] == 'f') {
                    if (i + 1 < argc) {
                        tarfile = argv[++i];
                        files_start = i + 1;
                    }
                    break;
                }
            }
        } else if (!tarfile) {
            /* Support bundled options e.g. "cvf file.tar" */
            for (size_t j = 0; argv[i][j]; j++) {
                if (argv[i][j] == 'c') mode = 'c';
                else if (argv[i][j] == 'x') mode = 'x';
                else if (argv[i][j] == 't') mode = 't';
                else if (argv[i][j] == 'v') verbose = 1;
                else if (argv[i][j] == 'f') {
                    if (i + 1 < argc) {
                        tarfile = argv[++i];
                        files_start = i + 1;
                    }
                    break;
                }
            }
        } else {
            files_start = i;
            break;
        }
    }

    if (!tarfile) tarfile = "-";

    if (mode == 'c') return tar_create(tarfile, argc, argv, files_start, verbose);
    if (mode == 'x') return tar_extract(tarfile, verbose);
    if (mode == 't') return tar_list(tarfile, verbose);

    puts("tar: Must specify one of -c, -x, or -t");
    return 1;
}
