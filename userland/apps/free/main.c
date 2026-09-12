/* ============================================================================
 * AzamiOS Userspace — Memory Free Utility (free.elf)
 * File: userland/apps/free/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/sys/sysinfo.h"

typedef enum {
    UNIT_BYTES,
    UNIT_KIBI,
    UNIT_MEBI,
    UNIT_GIBI,
    UNIT_HUMAN
} unit_mode_t;

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n\n"
           "Options:\n"
           "  -b, --bytes         show output in bytes\n"
           "  -k, --kibi          show output in kibibytes (default)\n"
           "  -m, --mebi          show output in mebibytes\n"
           "  -g, --gibi          show output in gibibytes\n"
           "  -h, --human         show human-readable output\n"
           "  -w, --wide          wide output (separate buffers and cache)\n"
           "  -t, --total         show line of totals\n"
           "      --help          display this help and exit\n"
           "  -V, --version       display version information and exit\n",
           prog);
}

static void format_val(unsigned long long bytes, unit_mode_t mode, char *out, size_t out_len)
{
    if (mode == UNIT_BYTES) {
        snprintf(out, out_len, "%llu", bytes);
    } else if (mode == UNIT_KIBI) {
        snprintf(out, out_len, "%llu", bytes / 1024ULL);
    } else if (mode == UNIT_MEBI) {
        snprintf(out, out_len, "%llu", bytes / (1024ULL * 1024ULL));
    } else if (mode == UNIT_GIBI) {
        snprintf(out, out_len, "%llu", bytes / (1024ULL * 1024ULL * 1024ULL));
    } else {
        /* Human readable */
        if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
            unsigned long long gib = bytes / (1024ULL * 1024ULL * 1024ULL);
            unsigned long long rem = ((bytes % (1024ULL * 1024ULL * 1024ULL)) * 10) / (1024ULL * 1024ULL * 1024ULL);
            snprintf(out, out_len, "%llu.%lluGi", gib, rem);
        } else if (bytes >= 1024ULL * 1024ULL) {
            unsigned long long mib = bytes / (1024ULL * 1024ULL);
            unsigned long long rem = ((bytes % (1024ULL * 1024ULL)) * 10) / (1024ULL * 1024ULL);
            snprintf(out, out_len, "%llu.%lluMi", mib, rem);
        } else if (bytes >= 1024ULL) {
            unsigned long long kib = bytes / 1024ULL;
            snprintf(out, out_len, "%lluKi", kib);
        } else {
            snprintf(out, out_len, "%lluB", bytes);
        }
    }
}

int main(int argc, char **argv)
{
    unit_mode_t mode = UNIT_KIBI;
    bool wide = false;
    bool total = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("free from procps-ng (AzamiOS compatibility) 7.0\n");
            return 0;
        }
        if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bytes") == 0) { mode = UNIT_BYTES; continue; }
        if (strcmp(argv[i], "-k") == 0 || strcmp(argv[i], "--kibi") == 0)  { mode = UNIT_KIBI;  continue; }
        if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--mebi") == 0)  { mode = UNIT_MEBI;  continue; }
        if (strcmp(argv[i], "-g") == 0 || strcmp(argv[i], "--gibi") == 0)  { mode = UNIT_GIBI;  continue; }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--human") == 0) { mode = UNIT_HUMAN; continue; }
        if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--wide") == 0)  { wide = true;       continue; }
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--total") == 0) { total = true;      continue; }

        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j]; j++) {
                switch (argv[i][j]) {
                    case 'b': mode = UNIT_BYTES; break;
                    case 'k': mode = UNIT_KIBI; break;
                    case 'm': mode = UNIT_MEBI; break;
                    case 'g': mode = UNIT_GIBI; break;
                    case 'h': mode = UNIT_HUMAN; break;
                    case 'w': wide = true; break;
                    case 't': total = true; break;
                    case 'V':
                        printf("free from procps-ng (AzamiOS compatibility) 7.0\n");
                        return 0;
                    default:
                        fprintf(stderr, "free: invalid option -- '%c'\n"
                                        "Try 'free --help' for more information.\n", argv[i][j]);
                        return 1;
                }
            }
        }
    }

    unsigned long long mem_total = 0;
    unsigned long long mem_free = 0;
    unsigned long long mem_avail = 0;
    unsigned long long buffers = 0;
    unsigned long long cached = 0;
    unsigned long long swap_total = 0;
    unsigned long long swap_free = 0;

    /* Try reading /proc/meminfo */
    int fd = open("/proc/meminfo", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[2048];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            char *line = strtok(buf, "\n");
            while (line) {
                unsigned long long val = 0;
                if (sscanf(line, "MemTotal: %llu kB", &val) == 1)      mem_total = val * 1024ULL;
                else if (sscanf(line, "MemFree: %llu kB", &val) == 1)   mem_free = val * 1024ULL;
                else if (sscanf(line, "MemAvailable: %llu kB", &val) == 1) mem_avail = val * 1024ULL;
                else if (sscanf(line, "Buffers: %llu kB", &val) == 1)   buffers = val * 1024ULL;
                else if (sscanf(line, "Cached: %llu kB", &val) == 1)    cached = val * 1024ULL;
                else if (sscanf(line, "SwapTotal: %llu kB", &val) == 1) swap_total = val * 1024ULL;
                else if (sscanf(line, "SwapFree: %llu kB", &val) == 1)  swap_free = val * 1024ULL;
                line = strtok(NULL, "\n");
            }
        }
    }

    /* Fallback to sysinfo */
    if (mem_total == 0) {
        struct sysinfo si;
        if (sysinfo(&si) == 0) {
            unsigned long long u = si.mem_unit ? si.mem_unit : 4096;
            mem_total = (unsigned long long)si.totalram * u;
            mem_free  = (unsigned long long)si.freeram * u;
            buffers   = (unsigned long long)si.bufferram * u;
            swap_total= (unsigned long long)si.totalswap * u;
            swap_free = (unsigned long long)si.freeswap * u;
            mem_avail = mem_free + buffers + cached;
        }
    }

    if (mem_avail == 0 && mem_free > 0) {
        mem_avail = mem_free;
    }

    unsigned long long buff_cache = buffers + cached;
    unsigned long long mem_used = 0;
    if (mem_total > (mem_free + buff_cache)) {
        mem_used = mem_total - mem_free - buff_cache;
    } else if (mem_total > mem_free) {
        mem_used = mem_total - mem_free;
    }
    unsigned long long swap_used = (swap_total > swap_free) ? (swap_total - swap_free) : 0;

    char s_mtot[32], s_musd[32], s_mfre[32], s_mshd[32], s_mbuf[32], s_mcac[32], s_mbc[32], s_mavl[32];
    char s_stot[32], s_susd[32], s_sfre[32];
    char s_ttot[32], s_tusd[32], s_tfre[32];

    format_val(mem_total, mode, s_mtot, sizeof(s_mtot));
    format_val(mem_used,  mode, s_musd, sizeof(s_musd));
    format_val(mem_free,  mode, s_mfre, sizeof(s_mfre));
    format_val(0ULL,      mode, s_mshd, sizeof(s_mshd));
    format_val(buffers,   mode, s_mbuf, sizeof(s_mbuf));
    format_val(cached,    mode, s_mcac, sizeof(s_mcac));
    format_val(buff_cache,mode, s_mbc,  sizeof(s_mbc));
    format_val(mem_avail, mode, s_mavl, sizeof(s_mavl));

    format_val(swap_total,mode, s_stot, sizeof(s_stot));
    format_val(swap_used, mode, s_susd, sizeof(s_susd));
    format_val(swap_free, mode, s_sfre, sizeof(s_sfre));

    if (wide) {
        printf("%-7s %11s %11s %11s %11s %11s %11s %11s\n",
               "", "total", "used", "free", "shared", "buffers", "cache", "available");
        printf("%-7s %11s %11s %11s %11s %11s %11s %11s\n",
               "Mem:", s_mtot, s_musd, s_mfre, s_mshd, s_mbuf, s_mcac, s_mavl);
        printf("%-7s %11s %11s %11s\n",
               "Swap:", s_stot, s_susd, s_sfre);
        if (total) {
            format_val(mem_total + swap_total, mode, s_ttot, sizeof(s_ttot));
            format_val(mem_used  + swap_used,  mode, s_tusd, sizeof(s_tusd));
            format_val(mem_free  + swap_free,  mode, s_tfre, sizeof(s_tfre));
            printf("%-7s %11s %11s %11s\n",
                   "Total:", s_ttot, s_tusd, s_tfre);
        }
    } else {
        printf("%-7s %11s %11s %11s %11s %11s %11s\n",
               "", "total", "used", "free", "shared", "buff/cache", "available");
        printf("%-7s %11s %11s %11s %11s %11s %11s\n",
               "Mem:", s_mtot, s_musd, s_mfre, s_mshd, s_mbc, s_mavl);
        printf("%-7s %11s %11s %11s\n",
               "Swap:", s_stot, s_susd, s_sfre);
        if (total) {
            format_val(mem_total + swap_total, mode, s_ttot, sizeof(s_ttot));
            format_val(mem_used  + swap_used,  mode, s_tusd, sizeof(s_tusd));
            format_val(mem_free  + swap_free,  mode, s_tfre, sizeof(s_tfre));
            printf("%-7s %11s %11s %11s\n",
                   "Total:", s_ttot, s_tusd, s_tfre);
        }
    }

    return 0;
}
