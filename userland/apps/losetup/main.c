/* ============================================================================
 * AzamiOS — losetup (Set up and control loop devices)
 * File: userland/apps/losetup/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#define LOOP_SET_FD         0x4C00
#define LOOP_CLR_FD         0x4C01
#define LOOP_GET_STATUS64   0x4C05
#define LOOP_SET_STATUS64   0x4C04
#define LOOP_CTL_GET_FREE   0x4C82

struct loop_info64 {
    unsigned long long lo_device;
    unsigned long long lo_inode;
    unsigned long long lo_rdevice;
    unsigned long long lo_offset;
    unsigned long long lo_sizelimit;
    unsigned int lo_number;
    unsigned int lo_encrypt_type;
    unsigned int lo_encrypt_key_size;
    unsigned int lo_flags;
    unsigned char lo_file_name[64];
    unsigned char lo_crypt_name[64];
    unsigned char lo_encrypt_key[32];
    unsigned long long lo_init[2];
};

static void print_status(const char *loop_path)
{
    int fd = open(loop_path, O_RDONLY);
    if (fd < 0) return;

    struct loop_info64 info;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, LOOP_GET_STATUS64, &info) == 0) {
        printf("%s: [%04llx]:%llu (%s)",
               loop_path, info.lo_device, info.lo_inode,
               info.lo_file_name[0] ? (char *)info.lo_file_name : "backing file");
        if (info.lo_offset) printf(", offset %llu", info.lo_offset);
        if (info.lo_sizelimit) printf(", sizelimit %llu", info.lo_sizelimit);
        printf("\n");
    }
    close(fd);
}

static void show_all(void)
{
    char devname[32];
    for (int i = 0; i < 8; i++) {
        snprintf(devname, sizeof(devname), "/dev/loop%d", i);
        print_status(devname);
    }
}

static int find_free(void)
{
    int ctl = open("/dev/loop-control", O_RDWR);
    if (ctl >= 0) {
        int free_idx = ioctl(ctl, LOOP_CTL_GET_FREE, 0);
        close(ctl);
        if (free_idx >= 0) {
            printf("/dev/loop%d\n", free_idx);
            return 0;
        }
    }

    /* Fallback: scan /dev/loop0..7 */
    char devname[32];
    for (int i = 0; i < 8; i++) {
        snprintf(devname, sizeof(devname), "/dev/loop%d", i);
        int fd = open(devname, O_RDONLY);
        if (fd >= 0) {
            struct loop_info64 info;
            if (ioctl(fd, LOOP_GET_STATUS64, &info) != 0) {
                close(fd);
                printf("%s\n", devname);
                return 0;
            }
            close(fd);
        }
    }
    fprintf(stderr, "losetup: no free loop device found\n");
    return 1;
}

static int detach(const char *loop_path)
{
    int fd = open(loop_path, O_RDWR);
    if (fd < 0) {
        perror(loop_path);
        return 1;
    }
    if (ioctl(fd, LOOP_CLR_FD, 0) < 0) {
        perror("ioctl(LOOP_CLR_FD)");
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

static int attach(const char *loop_path, const char *file_path, unsigned long long offset)
{
    int file_fd = open(file_path, O_RDWR);
    int ro = 0;
    if (file_fd < 0) {
        file_fd = open(file_path, O_RDONLY);
        ro = 1;
    }
    if (file_fd < 0) {
        perror(file_path);
        return 1;
    }

    int loop_fd = open(loop_path, O_RDWR);
    if (loop_fd < 0) {
        perror(loop_path);
        close(file_fd);
        return 1;
    }

    if (ioctl(loop_fd, LOOP_SET_FD, file_fd) < 0) {
        perror("ioctl(LOOP_SET_FD)");
        close(loop_fd);
        close(file_fd);
        return 1;
    }

    struct loop_info64 info;
    memset(&info, 0, sizeof(info));
    info.lo_offset = offset;
    if (ro) info.lo_flags |= 1;
    strncpy((char *)info.lo_file_name, file_path, sizeof(info.lo_file_name) - 1);
    ioctl(loop_fd, LOOP_SET_STATUS64, &info);

    close(loop_fd);
    close(file_fd);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "-a") == 0 || strcmp(argv[1], "--all") == 0) {
        show_all();
        return 0;
    }

    if (strcmp(argv[1], "-f") == 0 || strcmp(argv[1], "--find") == 0) {
        return find_free();
    }

    if (strcmp(argv[1], "-d") == 0 || strcmp(argv[1], "--detach") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: losetup -d /dev/loopN\n");
            return 1;
        }
        return detach(argv[2]);
    }

    if (argc >= 3) {
        unsigned long long offset = 0;
        const char *loop_dev = argv[1];
        const char *file_name = argv[2];
        return attach(loop_dev, file_name, offset);
    }

    print_status(argv[1]);
    return 0;
}
