/* ============================================================================
 * AzamiOS Userspace — Find Utility (find.elf)
 * File: userland/apps/find/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/sys/stat.h"
#include "../../libc/include/sys/wait.h"
#include "../../libc/include/ctype.h"
#include <stdbool.h>

static const char *g_name_pattern = NULL;
static const char *g_iname_pattern = NULL;
static char g_type_filter = '\0';
static int g_min_depth = 0;
static int g_max_depth = 100;
static bool g_filter_empty = false;
static bool g_filter_size = false;
static char g_size_op = '\0'; /* '+', '-', or '=' */
static off_t g_size_val = 0;
static bool g_do_delete = false;
static char **g_exec_args = NULL;
static int g_exec_count = 0;

static bool match_pattern(const char *pattern, const char *text, bool case_fold)
{
    if (!pattern) return true;

    while (*pattern && *text) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return true;
            while (*text) {
                if (match_pattern(pattern, text, case_fold)) return true;
                text++;
            }
            return false;
        } else if (*pattern == '?') {
            pattern++;
            text++;
        } else {
            char p = *pattern;
            char t = *text;
            if (case_fold) {
                p = (char)tolower((unsigned char)p);
                t = (char)tolower((unsigned char)t);
            }
            if (p == t) {
                pattern++;
                text++;
            } else {
                return false;
            }
        }
    }

    while (*pattern == '*') pattern++;
    return (*pattern == '\0' && *text == '\0');
}

static bool is_empty_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return false;
    struct dirent *de;
    bool empty = true;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) {
            empty = false;
            break;
        }
    }
    closedir(d);
    return empty;
}

static void execute_cmd(const char *filepath)
{
    if (!g_exec_args || g_exec_count <= 0) return;

    char *argv[64];
    int argc = 0;
    for (int i = 0; i < g_exec_count && argc < 63; i++) {
        if (strcmp(g_exec_args[i], "{}") == 0) {
            argv[argc++] = (char *)filepath;
        } else {
            argv[argc++] = g_exec_args[i];
        }
    }
    argv[argc] = NULL;

    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    } else if (pid > 0) {
        int st = 0;
        waitpid(pid, &st, 0);
    }
}

static void search_directory(const char *base_path, int depth)
{
    if (depth > g_max_depth) return;

    struct stat st;
    if (lstat(base_path, &st) < 0) return;

    bool is_dir = S_ISDIR(st.st_mode);
    bool is_reg = S_ISREG(st.st_mode);
    bool is_lnk = S_ISLNK(st.st_mode);

    /* Extract the basename */
    const char *bname = strrchr(base_path, '/');
    bname = bname ? (bname + 1) : base_path;
    if (!*bname) bname = base_path;

    bool matches = true;

    /* Depth filter */
    if (depth < g_min_depth) matches = false;

    /* Type filter */
    if (matches && g_type_filter) {
        switch (g_type_filter) {
            case 'f': if (!is_reg) matches = false; break;
            case 'd': if (!is_dir) matches = false; break;
            case 'l': if (!is_lnk) matches = false; break;
            default: break;
        }
    }

    /* Name filter */
    if (matches && g_name_pattern) {
        if (!match_pattern(g_name_pattern, bname, false)) matches = false;
    }

    /* Iname filter */
    if (matches && g_iname_pattern) {
        if (!match_pattern(g_iname_pattern, bname, true)) matches = false;
    }

    /* Empty filter */
    if (matches && g_filter_empty) {
        if (is_reg && st.st_size != 0) matches = false;
        else if (is_dir && !is_empty_dir(base_path)) matches = false;
    }

    /* Size filter */
    if (matches && g_filter_size) {
        if (g_size_op == '+' && st.st_size <= g_size_val) matches = false;
        else if (g_size_op == '-' && st.st_size >= g_size_val) matches = false;
        else if (g_size_op == '=' && st.st_size != g_size_val) matches = false;
    }

    /* Action */
    if (matches) {
        if (g_exec_args) {
            execute_cmd(base_path);
        } else {
            puts(base_path);
        }

        if (g_do_delete) {
            if (is_dir) rmdir(base_path);
            else unlink(base_path);
        }
    }

    if (!is_dir) return;

    DIR *d = opendir(base_path);
    if (!d) return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char subpath[512];
        if (strcmp(base_path, "/") == 0) {
            snprintf(subpath, sizeof(subpath), "/%s", entry->d_name);
        } else {
            snprintf(subpath, sizeof(subpath), "%s/%s", base_path, entry->d_name);
        }

        search_directory(subpath, depth + 1);
    }

    closedir(d);
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [path...] [expression]\n\n", prog);
    printf("Primary filters:\n");
    printf("  -name pattern      Match base name (wildcards: *, ?)\n");
    printf("  -iname pattern     Case-insensitive name match\n");
    printf("  -type [f|d|l]      Filter by file type (file, dir, symlink)\n");
    printf("  -size [+-]N[c|k|M] Filter by size (c=bytes, k=KiB, M=MiB)\n");
    printf("  -empty             Match empty files or empty directories\n");
    printf("  -mindepth N        Do not apply tests or actions at levels less than N\n");
    printf("  -maxdepth N        Descend at most N levels of directories\n");
    printf("Actions:\n");
    printf("  -print             Print the matched path to stdout (default)\n");
    printf("  -delete            Delete matched files/directories\n");
    printf("  -exec cmd ... {} ; Execute command for each matched file\n");
    printf("  --help, -h         Display this help message and exit\n");
    printf("  --version          Display version information and exit\n");
}

int main(int argc, char **argv)
{
    const char *start_paths[32];
    int start_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("find (GNU findutils compatibility) 4.9.0\n");
            return 0;
        }
        if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            g_name_pattern = argv[++i];
        } else if (strcmp(argv[i], "-iname") == 0 && i + 1 < argc) {
            g_iname_pattern = argv[++i];
        } else if (strcmp(argv[i], "-type") == 0 && i + 1 < argc) {
            g_type_filter = argv[++i][0];
        } else if (strcmp(argv[i], "-maxdepth") == 0 && i + 1 < argc) {
            g_max_depth = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-mindepth") == 0 && i + 1 < argc) {
            g_min_depth = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-empty") == 0) {
            g_filter_empty = true;
        } else if (strcmp(argv[i], "-delete") == 0) {
            g_do_delete = true;
        } else if (strcmp(argv[i], "-size") == 0 && i + 1 < argc) {
            const char *sz = argv[++i];
            g_filter_size = true;
            if (sz[0] == '+' || sz[0] == '-') {
                g_size_op = sz[0];
                sz++;
            } else {
                g_size_op = '=';
            }
            long long val = atoll(sz);
            size_t slen = strlen(sz);
            char unit = slen > 0 ? sz[slen - 1] : '\0';
            if (unit == 'k' || unit == 'K') val *= 1024;
            else if (unit == 'M') val *= 1024 * 1024;
            else if (unit == 'G') val *= 1024 * 1024 * 1024;
            g_size_val = (off_t)val;
        } else if (strcmp(argv[i], "-exec") == 0 && i + 1 < argc) {
            int exec_start = ++i;
            while (i < argc && strcmp(argv[i], ";") != 0 && strcmp(argv[i], "\\;") != 0) {
                i++;
            }
            g_exec_args = &argv[exec_start];
            g_exec_count = i - exec_start;
        } else if (strcmp(argv[i], "-print") == 0) {
            /* Default action */
        } else if (argv[i][0] != '-') {
            if (start_count < 32) {
                start_paths[start_count++] = argv[i];
            }
        }
    }

    if (start_count == 0) {
        start_paths[0] = ".";
        start_count = 1;
    }

    for (int i = 0; i < start_count; i++) {
        search_directory(start_paths[i], 0);
    }

    return 0;
}
