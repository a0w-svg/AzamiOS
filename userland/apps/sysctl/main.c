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

static int print_param(const char *param, int value_only, int ignore_missing)
{
    char path[256];
    convert_param_to_path(param, path, sizeof(path));

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (!ignore_missing) {
            fprintf(stderr, "sysctl: cannot stat %s: No such file or directory\n", path);
        }
        return -1;
    }

    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n > 0) {
        buf[n] = '\0';
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
        char *cr = strchr(buf, '\r');
        if (cr) *cr = '\0';

        if (value_only) {
            printf("%s\n", buf);
        } else {
            printf("%s = %s\n", param, buf);
        }
    } else {
        if (value_only) {
            printf("\n");
        } else {
            printf("%s =\n", param);
        }
    }
    return 0;
}

static int write_param(const char *key, const char *val, int quiet)
{
    char path[256];
    convert_param_to_path(key, path, sizeof(path));

    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "sysctl: cannot open %s for writing: Permission denied\n", path);
        return -1;
    }

    write(fd, val, strlen(val));
    write(fd, "\n", 1);
    close(fd);

    if (!quiet) {
        printf("%s = %s\n", key, val);
    }
    return 0;
}

static void dump_sys_dir(const char *dirpath, const char *prefix, int value_only)
{
    DIR *dir = opendir(dirpath);
    if (!dir) return;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        char subpath[512];
        snprintf(subpath, sizeof(subpath), "%s/%s", dirpath, de->d_name);

        char key[256];
        if (prefix && prefix[0]) {
            snprintf(key, sizeof(key), "%s.%s", prefix, de->d_name);
        } else {
            snprintf(key, sizeof(key), "%s", de->d_name);
        }

        /* Test if subpath is a directory */
        DIR *subdir = opendir(subpath);
        if (subdir) {
            closedir(subdir);
            dump_sys_dir(subpath, key, value_only);
        } else {
            /* Leaf node / sysctl parameter */
            print_param(key, value_only, 1);
        }
    }
    closedir(dir);
}

static int load_sysctl_file(const char *filepath, int ignore_missing)
{
    FILE *f = fopen(filepath, "r");
    if (!f) {
        if (!ignore_missing) {
            fprintf(stderr, "sysctl: cannot open file %s: No such file or directory\n", filepath);
        }
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == '\0')
            continue;

        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        char *cr = strchr(p, '\r');
        if (cr) *cr = '\0';

        char *eq = strchr(p, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = p;
        char *val = eq + 1;

        /* Trim trailing whitespace from key */
        char *end = key + strlen(key) - 1;
        while (end >= key && (*end == ' ' || *end == '\t')) {
            *end = '\0';
            end--;
        }

        /* Trim leading whitespace from val */
        while (*val == ' ' || *val == '\t') val++;
        /* Trim trailing whitespace from val */
        end = val + strlen(val) - 1;
        while (end >= val && (*end == ' ' || *end == '\t')) {
            *end = '\0';
            end--;
        }

        if (key[0] && val[0]) {
            write_param(key, val, 0);
        }
    }

    fclose(f);
    return 0;
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [options] [variable[=value] ...]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -a, -A, --all    Display all values currently available\n");
    fprintf(stderr, "  -n               Do not print the key name, print only its value\n");
    fprintf(stderr, "  -e               Ignore unknown key errors\n");
    fprintf(stderr, "  -w               Set key=value variable\n");
    fprintf(stderr, "  -p [file]        Load sysctl settings from specified file (default: /etc/sysctl.conf)\n");
    fprintf(stderr, "  -h, --help       Display this help message\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    int value_only = 0;
    int ignore_missing = 0;
    int actions_done = 0;
    int ret = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(arg, "-n") == 0) {
            value_only = 1;
        } else if (strcmp(arg, "-e") == 0) {
            ignore_missing = 1;
        } else if (strcmp(arg, "-a") == 0 || strcmp(arg, "-A") == 0 || strcmp(arg, "--all") == 0) {
            dump_sys_dir("/proc/sys", "", value_only);
            actions_done++;
        } else if (strcmp(arg, "-p") == 0) {
            const char *file = "/etc/sysctl.conf";
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                file = argv[++i];
            }
            if (load_sysctl_file(file, ignore_missing) < 0) {
                ret = 1;
            }
            actions_done++;
        } else if (strcmp(arg, "-w") == 0) {
            if (i + 1 < argc) {
                char *target = argv[++i];
                char *eq = strchr(target, '=');
                if (eq) {
                    *eq = '\0';
                    if (write_param(target, eq + 1, 0) < 0) ret = 1;
                } else {
                    fprintf(stderr, "sysctl: -w requires variable=value format\n");
                    ret = 1;
                }
                actions_done++;
            } else {
                fprintf(stderr, "sysctl: option requires an argument -- 'w'\n");
                return 1;
            }
        } else {
            char *eq = strchr(arg, '=');
            if (eq) {
                char key[128];
                size_t klen = (size_t)(eq - arg);
                if (klen >= sizeof(key)) klen = sizeof(key) - 1;
                memcpy(key, arg, klen);
                key[klen] = '\0';

                if (write_param(key, eq + 1, 0) < 0) ret = 1;
            } else {
                if (print_param(arg, value_only, ignore_missing) < 0) ret = 1;
            }
            actions_done++;
        }
    }

    if (actions_done == 0) {
        print_usage(argv[0]);
        return 1;
    }

    return ret;
}
