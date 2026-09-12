/* ============================================================================
 * AzamiOS Userspace — Directory Tree Generator (tree.elf)
 * File: userland/apps/tree/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/sys/stat.h"
#include "../../libc/include/unistd.h"

static int g_dirs = 0;
static int g_files = 0;
static int g_max_level = 20;
static bool g_dirs_only = false;
static bool g_all_files = false;
static bool g_show_size = false;
static bool g_human_size = false;
static bool g_show_perms = false;
static bool g_classify = false;
static bool g_color = false;

static void format_mode(mode_t m, char *out)
{
    out[0] = S_ISDIR(m) ? 'd' : (S_ISLNK(m) ? 'l' : (S_ISCHR(m) ? 'c' : (S_ISBLK(m) ? 'b' : '-')));
    out[1] = (m & S_IRUSR) ? 'r' : '-';
    out[2] = (m & S_IWUSR) ? 'w' : '-';
    out[3] = (m & S_IXUSR) ? 'x' : '-';
    out[4] = (m & S_IRGRP) ? 'r' : '-';
    out[5] = (m & S_IWGRP) ? 'w' : '-';
    out[6] = (m & S_IXGRP) ? 'x' : '-';
    out[7] = (m & S_IROTH) ? 'r' : '-';
    out[8] = (m & S_IWOTH) ? 'w' : '-';
    out[9] = (m & S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

static void format_human(off_t bytes, char *out, size_t out_sz)
{
    if (bytes >= 1024 * 1024 * 1024) {
        snprintf(out, out_sz, "%4lldG", (long long)(bytes / (1024 * 1024 * 1024)));
    } else if (bytes >= 1024 * 1024) {
        snprintf(out, out_sz, "%4lldM", (long long)(bytes / (1024 * 1024)));
    } else if (bytes >= 1024) {
        snprintf(out, out_sz, "%4lldK", (long long)(bytes / 1024));
    } else {
        snprintf(out, out_sz, "%4lldB", (long long)bytes);
    }
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [-adFhpstCv] [-L level] [--help] [--version] [directory...]\n\n", prog);
    printf("Listing options:\n");
    printf("  -a            All files, including hidden files\n");
    printf("  -d            List directories only\n");
    printf("  -L level      Max display depth of the directory tree\n");
    printf("  -p            Print the protections for each file\n");
    printf("  -s            Print the size in bytes of each file\n");
    printf("  -h            Print sizes in human readable format (e.g. 1K, 2M)\n");
    printf("  -F            Append '/' for dirs, '*' for executables\n");
    printf("  -C            Colorize output using ANSI colors\n");
    printf("  -v, --version Print version information\n");
    printf("  --help        Print this help message\n");
}

static void tree_walk(const char *dir_path, const char *prefix, int depth)
{
    if (depth >= g_max_level) return;

    DIR *dir = opendir(dir_path);
    if (!dir) return;

    char names[128][64];
    struct stat stats[128];
    bool is_dir[128];
    int count = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL && count < 128) {
        if (!g_all_files && de->d_name[0] == '.') continue;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        char full_path[512];
        if (strcmp(dir_path, "/") == 0) {
            snprintf(full_path, sizeof(full_path), "/%s", de->d_name);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, de->d_name);
        }

        struct stat st;
        if (lstat(full_path, &st) == 0) {
            bool dir_flag = S_ISDIR(st.st_mode);
            if (g_dirs_only && !dir_flag) continue;

            strncpy(names[count], de->d_name, sizeof(names[count]) - 1);
            names[count][sizeof(names[count]) - 1] = '\0';
            stats[count] = st;
            is_dir[count] = dir_flag;
            count++;
        }
    }
    closedir(dir);

    for (int i = 0; i < count; i++) {
        bool last = (i == count - 1);
        printf("%s%s ", prefix, last ? "`--" : "|--");

        if (g_show_perms) {
            char pstr[16];
            format_mode(stats[i].st_mode, pstr);
            printf("[%s] ", pstr);
        }

        if (g_human_size) {
            char hstr[16];
            format_human(stats[i].st_size, hstr, sizeof(hstr));
            printf("[%s] ", hstr);
        } else if (g_show_size) {
            printf("[%10lld] ", (long long)stats[i].st_size);
        }

        /* Color formatting */
        if (g_color) {
            if (is_dir[i]) {
                printf("\033[1;34m%s\033[0m", names[i]);
            } else if (S_ISLNK(stats[i].st_mode)) {
                printf("\033[1;36m%s\033[0m", names[i]);
            } else if (stats[i].st_mode & 0111) {
                printf("\033[1;32m%s\033[0m", names[i]);
            } else {
                printf("%s", names[i]);
            }
        } else {
            printf("%s", names[i]);
        }

        if (g_classify) {
            if (is_dir[i]) {
                putchar('/');
            } else if (S_ISLNK(stats[i].st_mode)) {
                putchar('@');
            } else if (stats[i].st_mode & 0111) {
                putchar('*');
            }
        }
        putchar('\n');

        if (is_dir[i]) {
            g_dirs++;
            char sub_prefix[256];
            snprintf(sub_prefix, sizeof(sub_prefix), "%s%s", prefix, last ? "    " : "|   ");

            char full_path[512];
            if (strcmp(dir_path, "/") == 0) {
                snprintf(full_path, sizeof(full_path), "/%s", names[i]);
            } else {
                snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, names[i]);
            }
            tree_walk(full_path, sub_prefix, depth + 1);
        } else {
            g_files++;
        }
    }
}

int main(int argc, char **argv)
{
    const char *roots[32];
    int root_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("tree v2.1.0 (AzamiOS Extended Utilities)\n");
            return 0;
        }
        if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            g_max_level = atoi(argv[++i]);
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (int j = 1; argv[i][j]; j++) {
                switch (argv[i][j]) {
                    case 'a': g_all_files = true; break;
                    case 'd': g_dirs_only = true; break;
                    case 'p': g_show_perms = true; break;
                    case 's': g_show_size = true; break;
                    case 'h': g_human_size = true; break;
                    case 'F': g_classify = true; break;
                    case 'C': g_color = true; break;
                    case 'v':
                        printf("tree v2.1.0 (AzamiOS Extended Utilities)\n");
                        return 0;
                    case 'L':
                        if (argv[i][j + 1]) {
                            g_max_level = atoi(&argv[i][j + 1]);
                            j = (int)strlen(argv[i]) - 1;
                        } else if (i + 1 < argc) {
                            g_max_level = atoi(argv[++i]);
                        }
                        break;
                    default:
                        fprintf(stderr, "tree: invalid option -- '%c'\nTry 'tree --help' for more information.\n", argv[i][j]);
                        return 1;
                }
            }
            continue;
        }
        if (root_count < 32) {
            roots[root_count++] = argv[i];
        }
    }

    if (root_count == 0) {
        roots[0] = ".";
        root_count = 1;
    }

    for (int r = 0; r < root_count; r++) {
        if (g_color) {
            printf("\033[1;34m%s\033[0m\n", roots[r]);
        } else {
            printf("%s\n", roots[r]);
        }
        tree_walk(roots[r], "", 0);
    }

    if (g_dirs_only) {
        printf("\n%d director%s\n", g_dirs, (g_dirs == 1) ? "y" : "ies");
    } else {
        printf("\n%d director%s, %d file%s\n",
               g_dirs, (g_dirs == 1) ? "y" : "ies",
               g_files, (g_files == 1) ? "" : "s");
    }

    return 0;
}
