/* ============================================================================
 * AzamiOS Userspace — POSIX pr Utility (main.c)
 * File: userland/apps/pr/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>

static void print_help(void)
{
    printf("Usage: pr [OPTION]... [FILE]...\n"
           "Paginate or columnate FILE(s) for printing.\n\n"
           "  +FIRST_PAGE       begin printing with page FIRST_PAGE\n"
           "  -d, --double-space\n"
           "                    double space the output\n"
           "  -h, --header=HEADER\n"
           "                    use a centered HEADER instead of filename in header\n"
           "  -l, --length=PAGE_LENGTH\n"
           "                    set the page length to PAGE_LENGTH (66) lines\n"
           "  -n, --number-lines[=SEP[DIGITS]]\n"
           "                    number lines, default 5 digits\n"
           "  -t, --omit-header\n"
           "                    omit page headers and trailers\n"
           "  -w, --width=PAGE_WIDTH\n"
           "                    set page width to PAGE_WIDTH (72) characters\n"
           "      --help        display this help and exit\n");
}

static void format_file(FILE *fp, const char *fname, const char *custom_header,
                        int page_len, int double_space, int num_lines, int omit_header, int first_page)
{
    char time_str[32] = {0};
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    if (tm_info) {
        strftime(time_str, sizeof(time_str), "%b %d %H:%M %Y", tm_info);
    } else {
        snprintf(time_str, sizeof(time_str), "Aug 24 16:00 2026");
    }

    const char *hdr_title = custom_header ? custom_header : fname;
    int page = 1;
    int line_in_page = 0;
    int global_line = 1;
    char line[1024];

    int body_len = omit_header ? page_len : (page_len > 10 ? page_len - 10 : page_len);

    while (fgets(line, sizeof(line), fp)) {
        if (!omit_header && line_in_page == 0) {
            if (page >= first_page) {
                printf("\n\n%s  %s  Page %d\n\n\n", time_str, hdr_title, page);
            }
        }

        if (page >= first_page) {
            if (num_lines) {
                printf("%5d\t%s", global_line, line);
            } else {
                printf("%s", line);
            }
            if (double_space) printf("\n");
        }

        global_line++;
        line_in_page += double_space ? 2 : 1;

        if (line_in_page >= body_len) {
            if (!omit_header && page >= first_page) {
                printf("\n\n\n\n\n");
            }
            line_in_page = 0;
            page++;
        }
    }

    if (!omit_header && line_in_page > 0 && page >= first_page) {
        while (line_in_page < body_len + 5) {
            printf("\n");
            line_in_page++;
        }
    }
}

int main(int argc, char *argv[])
{
    int opt;
    char *custom_header = NULL;
    int page_len = 66;
    int double_space = 0;
    int num_lines = 0;
    int omit_header = 0;
    int first_page = 1;

    static struct option long_options[] = {
        {"header",       required_argument, 0, 'h'},
        {"length",       required_argument, 0, 'l'},
        {"double-space", no_argument,       0, 'd'},
        {"number-lines", no_argument,       0, 'n'},
        {"omit-header",  no_argument,       0, 't'},
        {"help",         no_argument,       0, 'H'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "h:l:dntH", long_options, NULL)) != -1) {
        switch (opt) {
            case 'h': custom_header = optarg; break;
            case 'l': page_len = atoi(optarg); if (page_len <= 0) page_len = 66; break;
            case 'd': double_space = 1; break;
            case 'n': num_lines = 1; break;
            case 't': omit_header = 1; break;
            case 'H': print_help(); return 0;
            default:
                fprintf(stderr, "Try 'pr --help' for more information.\n");
                return 1;
        }
    }

    if (optind >= argc) {
        format_file(stdin, "", custom_header, page_len, double_space, num_lines, omit_header, first_page);
        return 0;
    }

    for (int i = optind; i < argc; i++) {
        FILE *fp = fopen(argv[i], "r");
        if (!fp) {
            perror(argv[i]);
            continue;
        }
        format_file(fp, argv[i], custom_header, page_len, double_space, num_lines, omit_header, first_page);
        fclose(fp);
    }

    return 0;
}
