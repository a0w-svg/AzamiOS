/* ============================================================================
 * AzamiOS Userspace — Linux mktemp Utility (main.c)
 * File: userland/apps/mktemp/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>

static void print_help(void)
{
    printf("Usage: mktemp [OPTION]... [TEMPLATE]\n"
           "Create a temporary file or directory, safely, and print its name.\n"
           "TEMPLATE must contain at least 3 consecutive 'X's in last component.\n"
           "If TEMPLATE is not specified, use tmp.XXXXXXXXXX.\n\n"
           "  -d, --directory     create a directory, not a file\n"
           "  -u, --dry-run       do not create anything; merely print a name (unsafe)\n"
           "  -q, --quiet         suppress diagnostics about file/dir-creation failure\n"
           "  -p DIR, --tmpdir=DIR\n"
           "                      interpret TEMPLATE relative to DIR;\n"
           "                      if DIR is not specified, use $TMPDIR if set,\n"
           "                      else /tmp\n"
           "      --help          display this help and exit\n");
}

int main(int argc, char *argv[])
{
    int opt;
    int opt_dir = 0;
    int opt_dry_run = 0;
    int opt_quiet = 0;
    char *opt_tmpdir = NULL;

    static struct option long_options[] = {
        {"directory", no_argument,       0, 'd'},
        {"dry-run",   no_argument,       0, 'u'},
        {"quiet",     no_argument,       0, 'q'},
        {"tmpdir",    optional_argument, 0, 'p'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "duqp:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'd': opt_dir = 1; break;
            case 'u': opt_dry_run = 1; break;
            case 'q': opt_quiet = 1; break;
            case 'p': opt_tmpdir = optarg ? optarg : "/tmp"; break;
            case 'h': print_help(); return 0;
            default:
                if (!opt_quiet) fprintf(stderr, "Try 'mktemp --help' for more information.\n");
                return 1;
        }
    }

    const char *template_in = "tmp.XXXXXXXXXX";
    if (optind < argc) {
        template_in = argv[optind];
    }

    char template_path[4096];
    if (strchr(template_in, '/')) {
        strncpy(template_path, template_in, sizeof(template_path) - 1);
    } else {
        const char *base_dir = opt_tmpdir;
        if (!base_dir) base_dir = getenv("TMPDIR");
        if (!base_dir || !*base_dir) base_dir = "/tmp";
        snprintf(template_path, sizeof(template_path), "%s/%s", base_dir, template_in);
    }

    if (opt_dry_run) {
        mktemp(template_path);
        printf("%s\n", template_path);
        return 0;
    }

    if (opt_dir) {
        char *res = mkdtemp(template_path);
        if (!res) {
            if (!opt_quiet) perror("mktemp");
            return 1;
        }
        printf("%s\n", res);
    } else {
        int fd = mkstemp(template_path);
        if (fd < 0) {
            if (!opt_quiet) perror("mktemp");
            return 1;
        }
        close(fd);
        printf("%s\n", template_path);
    }

    return 0;
}
