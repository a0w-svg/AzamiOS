/* ============================================================================
 * AzamiOS Userspace — POSIX od (Octal Dump) Utility (main.c)
 * File: userland/apps/od/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <ctype.h>

typedef enum {
    FMT_OCTAL_2,
    FMT_OCTAL_1,
    FMT_HEX_2,
    FMT_HEX_4,
    FMT_CHAR,
    FMT_DECIMAL_2
} OutputFormat;

static void print_help(void)
{
    printf("Usage: od [OPTION]... [FILE]...\n"
           "Write an unambiguous representation of FILE to standard output.\n\n"
           "  -A, --address-radix=RADIX   output format for file offsets (d, o, x, n)\n"
           "  -b, --byte-octal            same as -t o1, select octal bytes\n"
           "  -c, --character             select printable characters or backslash escapes\n"
           "  -d, --decimal-2             select unsigned decimal 2-byte units\n"
           "  -o, --octal                 same as -t o2, select octal 2-byte units\n"
           "  -x, --hex                   same as -t x2, select hexadecimal 2-byte units\n"
           "  -j, --skip-bytes=BYTES      skip BYTES input bytes before formatting\n"
           "  -N, --read-bytes=BYTES      limit dump to BYTES input bytes\n"
           "      --help                  display this help and exit\n");
}

static void print_offset(size_t offset, char radix)
{
    switch (radix) {
        case 'd': printf("%07lu ", (unsigned long)offset); break;
        case 'x': printf("%06lx ", (unsigned long)offset); break;
        case 'n': break;
        case 'o':
        default:  printf("%07lo ", (unsigned long)offset); break;
    }
}

static void dump_stream(FILE *fp, OutputFormat fmt, char radix, size_t skip, size_t limit)
{
    unsigned char buf[16];
    size_t current_offset = 0;
    size_t bytes_read_total = 0;

    if (skip > 0) {
        fseek(fp, (long)skip, SEEK_SET);
        current_offset = skip;
    }

    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (limit > 0 && bytes_read_total + n > limit) {
            n = limit - bytes_read_total;
        }
        if (n == 0) break;

        print_offset(current_offset, radix);

        switch (fmt) {
            case FMT_OCTAL_2:
                for (size_t i = 0; i < n; i += 2) {
                    if (i + 1 < n) {
                        unsigned short val = (unsigned short)(buf[i] | (buf[i+1] << 8));
                        printf(" %06o", val);
                    } else {
                        printf(" %06o", buf[i]);
                    }
                }
                break;

            case FMT_HEX_2:
                for (size_t i = 0; i < n; i += 2) {
                    if (i + 1 < n) {
                        unsigned short val = (unsigned short)(buf[i] | (buf[i+1] << 8));
                        printf("   %04x", val);
                    } else {
                        printf("   %02x", buf[i]);
                    }
                }
                break;

            case FMT_OCTAL_1:
                for (size_t i = 0; i < n; i++) {
                    printf(" %03o", buf[i]);
                }
                break;

            case FMT_DECIMAL_2:
                for (size_t i = 0; i < n; i += 2) {
                    if (i + 1 < n) {
                        unsigned short val = (unsigned short)(buf[i] | (buf[i+1] << 8));
                        printf("  %5u", val);
                    } else {
                        printf("  %5u", buf[i]);
                    }
                }
                break;

            case FMT_CHAR:
                for (size_t i = 0; i < n; i++) {
                    unsigned char c = buf[i];
                    if (c == '\n') printf("  \\n");
                    else if (c == '\t') printf("  \\t");
                    else if (c == '\r') printf("  \\r");
                    else if (c == '\0') printf("  \\0");
                    else if (isprint(c)) printf("   %c", c);
                    else printf(" %03o", c);
                }
                break;

            default:
                break;
        }

        printf("\n");
        current_offset += n;
        bytes_read_total += n;
        if (limit > 0 && bytes_read_total >= limit) break;
    }

    print_offset(current_offset, radix);
    printf("\n");
}

int main(int argc, char *argv[])
{
    int opt;
    OutputFormat fmt = FMT_OCTAL_2;
    char radix = 'o';
    size_t skip = 0;
    size_t limit = 0;

    static struct option long_options[] = {
        {"address-radix", required_argument, 0, 'A'},
        {"byte-octal",    no_argument,       0, 'b'},
        {"character",     no_argument,       0, 'c'},
        {"decimal-2",     no_argument,       0, 'd'},
        {"octal",         no_argument,       0, 'o'},
        {"hex",           no_argument,       0, 'x'},
        {"skip-bytes",    required_argument, 0, 'j'},
        {"read-bytes",    required_argument, 0, 'N'},
        {"help",          no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "A:bcdoxj:N:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'A': radix = optarg[0]; break;
            case 'b': fmt = FMT_OCTAL_1; break;
            case 'c': fmt = FMT_CHAR; break;
            case 'd': fmt = FMT_DECIMAL_2; break;
            case 'o': fmt = FMT_OCTAL_2; break;
            case 'x': fmt = FMT_HEX_2; break;
            case 'j': skip = (size_t)strtoul(optarg, NULL, 0); break;
            case 'N': limit = (size_t)strtoul(optarg, NULL, 0); break;
            case 'h': print_help(); return 0;
            default:
                fprintf(stderr, "Try 'od --help' for more information.\n");
                return 1;
        }
    }

    if (optind >= argc) {
        dump_stream(stdin, fmt, radix, skip, limit);
        return 0;
    }

    for (int i = optind; i < argc; i++) {
        FILE *fp = fopen(argv[i], "rb");
        if (!fp) {
            perror(argv[i]);
            continue;
        }
        dump_stream(fp, fmt, radix, skip, limit);
        fclose(fp);
    }

    return 0;
}
