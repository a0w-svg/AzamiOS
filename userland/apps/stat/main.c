/* ============================================================================
 * AzamiOS Userspace — Display File Status (stat.elf)
 * File: userland/apps/stat/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/sys/stat.h"
#include "../../libc/include/time.h"

static void format_mode(mode_t mode, char *str)
{
    str[0] = S_ISDIR(mode) ? 'd' : (S_ISLNK(mode) ? 'l' : (S_ISCHR(mode) ? 'c' : (S_ISBLK(mode) ? 'b' : (S_ISFIFO(mode) ? 'p' : '-'))));
    str[1] = (mode & 0400) ? 'r' : '-';
    str[2] = (mode & 0200) ? 'w' : '-';
    str[3] = (mode & 0100) ? 'x' : '-';
    str[4] = (mode & 0040) ? 'r' : '-';
    str[5] = (mode & 0020) ? 'w' : '-';
    str[6] = (mode & 0010) ? 'x' : '-';
    str[7] = (mode & 0004) ? 'r' : '-';
    str[8] = (mode & 0002) ? 'w' : '-';
    str[9] = (mode & 0001) ? 'x' : '-';
    str[10] = '\0';
}

static const char *get_file_type(mode_t mode)
{
    if (S_ISREG(mode)) return "regular file";
    if (S_ISDIR(mode)) return "directory";
    if (S_ISLNK(mode)) return "symbolic link";
    if (S_ISCHR(mode)) return "character special file";
    if (S_ISBLK(mode)) return "block special file";
    if (S_ISFIFO(mode)) return "fifo/pipe";
    if (S_ISSOCK(mode)) return "socket";
    return "unknown";
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        puts("Usage: stat <file...>");
        return 1;
    }

    int ret = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (lstat(argv[i], &st) < 0) {
            printf("stat: cannot stat '%s': No such file or directory\n", argv[i]);
            ret = 1;
            continue;
        }

        char modestr[16];
        format_mode(st.st_mode, modestr);

        printf("  File: %s\n", argv[i]);
        printf("  Size: %-15lld Blocks: %-10lld IO Block: %-6lld %s\n",
               (long long)st.st_size, (long long)st.st_blocks, (long long)st.st_blksize, get_file_type(st.st_mode));
        printf("Device: %llxh/%llud      Inode: %-12llu Links: %llu\n",
               (unsigned long long)st.st_dev, (unsigned long long)st.st_dev,
               (unsigned long long)st.st_ino, (unsigned long long)st.st_nlink);
        printf("Access: (%04o/%s)  Uid: (%5u)   Gid: (%5u)\n",
               (unsigned int)(st.st_mode & 07777), modestr, (unsigned int)st.st_uid, (unsigned int)st.st_gid);
        printf("Access: %lu\n", (unsigned long)st.st_atime);
        printf("Modify: %lu\n", (unsigned long)st.st_mtime);
        printf("Change: %lu\n", (unsigned long)st.st_ctime);
    }
    return ret;
}
