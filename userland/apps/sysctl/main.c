/* ============================================================================
 * AzamiOS Userland — Linux sysctl (Kernel Runtime Parameters) Utility
 * File: userland/apps/sysctl/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

static void convert_param_to_path(const char *param, char *out_path, size_t max_len)
{
    snprintf(out_path, max_len, "/proc/sys/");
    size_t base_len = strlen(out_path);

    for (size_t i = 0; param[i] && base_len + i < max_len - 1; i++) {
        if (param[i] == '.') {
            out_path[base_len + i] = '/';
        } else {
            out_path[base_len + i] = param[i];
        }
        out_path[base_len + i + 1] = '\0';
    }
}

static void print_param(const char *param)
{
    char path[128];
    convert_param_to_path(param, path, sizeof(path));

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "sysctl: cannot stat %s: No such file or directory\n", path);
        return;
    }

    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n > 0) {
        buf[n] = '\0';
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
        printf("%s = %s\n", param, buf);
    } else {
        printf("%s =\n", param);
    }
}

static void write_param(const char *key, const char *val)
{
    char path[128];
    convert_param_to_path(key, path, sizeof(path));

    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "sysctl: cannot open %s for writing: Permission denied\n", path);
        return;
    }

    write(fd, val, strlen(val));
    write(fd, "\n", 1);
    close(fd);

    printf("%s = %s\n", key, val);
}

static void show_all_params(void)
{
    print_param("kernel.ostype");
    print_param("kernel.osrelease");
    print_param("kernel.version");
    print_param("kernel.hostname");
    print_param("fs.file-max");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: sysctl [options] [variable[=value] ...]\n");
        fprintf(stderr, "Options:\n");
        fprintf(stderr, "  -a, --all        Display all values currently available\n");
        fprintf(stderr, "  -w               Set key=value variable\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "-A") == 0 || strcmp(argv[i], "--all") == 0) {
            show_all_params();
            return 0;
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            char *eq = strchr(argv[i + 1], '=');
            if (eq) {
                *eq = '\0';
                write_param(argv[i + 1], eq + 1);
            }
            i++;
        } else {
            char *eq = strchr(argv[i], '=');
            if (eq) {
                *eq = '\0';
                write_param(argv[i], eq + 1);
            } else {
                print_param(argv[i]);
            }
        }
    }

    return 0;
}
