/* ============================================================================
 * AzamiOS — Terminal Pager (less.elf / more.elf v2.0)
 * File: userland/apps/less/main.c
 *
 * Full-screen terminal pager supporting line-by-line and page-by-page
 * navigation, forward and reverse pattern searching with match highlighting,
 * percentage indicators, line numbering, jump to line, and keybinding help.
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/termios.h"
#include "../../libc/include/sys/ioctl.h"

#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24
#define MAX_LINE_LEN 1024

static char **g_lines = NULL;
static int    g_num_lines = 0;
static int    g_capacity = 0;

static int    g_top_line = 0;
static int    g_cols = DEFAULT_COLS;
static int    g_rows = DEFAULT_ROWS;

static bool   g_show_line_nums = false;
static bool   g_show_help = false;
static char   g_search_query[128] = "";
static bool   g_search_reverse = false;
static char   g_filename[256] = "stdin";
static int    g_num_acc = 0; /* Numeric prefix accumulator */

static struct termios g_orig_termios;
static bool   g_raw_mode = false;

/* ── Terminal Raw Mode ────────────────────────────────────────────────────── */
static void disable_raw_mode(void)
{
    if (g_raw_mode) {
        printf("\033[?25h\033[0m"); /* show cursor, reset color */
        fflush(stdout);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw_mode = false;
    }
}

static void enable_raw_mode(void)
{
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1) return;
    atexit(disable_raw_mode);

    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != -1) {
        g_raw_mode = true;
        printf("\033[?25l"); /* hide cursor */
        fflush(stdout);
    }
}

static void update_term_size(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        g_cols = ws.ws_col;
        g_rows = ws.ws_row;
    } else {
        g_cols = DEFAULT_COLS;
        g_rows = DEFAULT_ROWS;
    }
}

/* ── Buffer Management ─────────────────────────────────────────────────────── */
static void add_line(const char *buf, size_t len)
{
    if (g_num_lines >= g_capacity) {
        g_capacity = (g_capacity == 0) ? 256 : g_capacity * 2;
        char **new_lines = (char **)realloc(g_lines, g_capacity * sizeof(char *));
        if (!new_lines) return;
        g_lines = new_lines;
    }
    char *s = (char *)malloc(len + 1);
    if (!s) return;
    memcpy(s, buf, len);
    s[len] = '\0';
    g_lines[g_num_lines++] = s;
}

static void load_stream(int fd)
{
    char buf[4096];
    char line[MAX_LINE_LEN];
    size_t line_len = 0;
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                add_line(line, line_len);
                line_len = 0;
            } else if (c == '\r') {
                continue;
            } else {
                if (line_len < MAX_LINE_LEN - 1) {
                    line[line_len++] = c;
                }
            }
        }
    }
    if (line_len > 0) {
        add_line(line, line_len);
    }
    if (g_num_lines == 0) {
        add_line("", 0);
    }
}

/* ── Search Helpers ───────────────────────────────────────────────────────── */
static int find_next_match(int start_line)
{
    if (!g_search_query[0]) return -1;
    for (int i = start_line; i < g_num_lines; i++) {
        if (strstr(g_lines[i], g_search_query) != NULL) {
            return i;
        }
    }
    return -1;
}

static int find_prev_match(int start_line)
{
    if (!g_search_query[0]) return -1;
    for (int i = start_line; i >= 0; i--) {
        if (strstr(g_lines[i], g_search_query) != NULL) {
            return i;
        }
    }
    return -1;
}

/* ── Screen Rendering ─────────────────────────────────────────────────────── */
static void render_screen(void)
{
    update_term_size();
    int visible_rows = g_rows - 1;

    /* Home & Clear */
    printf("\033[H");

    if (g_show_help) {
        /* Help Overlay */
        printf("\033[48;2;24;24;37m\033[38;2;203;166;247m\033[1m AZAMI LESS — KEYBINDINGS REFERENCE \033[0m\033[K\r\n");
        printf("\033[38;2;166;227;161m  Navigation:\033[0m\033[K\r\n");
        printf("    j, Down, Enter      Scroll down one line\033[K\r\n");
        printf("    k, Up               Scroll up one line\033[K\r\n");
        printf("    Space, PageDown     Scroll down one screen\033[K\r\n");
        printf("    b, PageUp           Scroll up one screen\033[K\r\n");
        printf("    g, Home             Go to first line\033[K\r\n");
        printf("    G, End              Go to last line\033[K\r\n");
        printf("    <num>g, :<num>      Jump to specific line number\033[K\r\n");
        printf("\033[38;2;166;227;161m  Searching:\033[0m\033[K\r\n");
        printf("    /pattern            Search forward for pattern\033[K\r\n");
        printf("    ?pattern            Search backward for pattern\033[K\r\n");
        printf("    n                   Repeat search in same direction\033[K\r\n");
        printf("    N                   Repeat search in opposite direction\033[K\r\n");
        printf("\033[38;2;166;227;161m  General:\033[0m\033[K\r\n");
        printf("    -N                  Toggle line numbers\033[K\r\n");
        printf("    h, ?                Toggle this help screen\033[K\r\n");
        printf("    q, Q                Exit pager\033[K\r\n");

        for (int r = 16; r < visible_rows; r++) {
            printf("\033[K\r\n");
        }
        /* Bottom help status */
        printf("\033[7m Press 'q' or 'h' to return to file \033[0m\033[K");
        fflush(stdout);
        return;
    }

    /* Print text rows */
    for (int r = 0; r < visible_rows; r++) {
        int line_idx = g_top_line + r;
        if (line_idx < g_num_lines) {
            char *line = g_lines[line_idx];

            if (g_show_line_nums) {
                printf("\033[38;2;88;91;112m%5d \033[0m", line_idx + 1);
            }

            if (g_search_query[0] && strstr(line, g_search_query) != NULL) {
                const char *p = line;
                size_t qlen = strlen(g_search_query);
                while (*p) {
                    const char *match = strstr(p, g_search_query);
                    if (match) {
                        fwrite(p, 1, match - p, stdout);
                        printf("\033[48;2;249;226;175m\033[38;2;17;17;27m\033[1m%s\033[0m", g_search_query);
                        p = match + qlen;
                    } else {
                        fputs(p, stdout);
                        break;
                    }
                }
            } else {
                fputs(line, stdout);
            }
            printf("\033[K\r\n");
        } else {
            /* Past end of file: tilde */
            printf("\033[38;2;69;71;90m~\033[0m\033[K\r\n");
        }
    }

    /* ── Status Bar with Position & Percentage ── */
    char pos_str[32];
    if (g_top_line == 0) {
        snprintf(pos_str, sizeof(pos_str), "[Top]");
    } else if (g_top_line + visible_rows >= g_num_lines) {
        snprintf(pos_str, sizeof(pos_str), "[Bot]");
    } else {
        int max_scroll = g_num_lines - visible_rows;
        int pct = (max_scroll > 0) ? (g_top_line * 100) / max_scroll : 100;
        snprintf(pos_str, sizeof(pos_str), "[%d%%]", pct);
    }

    printf("\033[7m %s  Line %d/%d  %s", g_filename, g_top_line + 1, g_num_lines, pos_str);
    if (g_num_acc > 0) {
        printf("  Prefix: %d", g_num_acc);
    }
    printf("  (press 'h' for help, 'q' to quit) \033[0m\033[K");
    fflush(stdout);
}

/* ── Interactive Search & Goto Prompts ────────────────────────────────────── */
static void prompt_search(bool reverse)
{
    printf("\033[%d;1H\033[7m%c\033[0m", g_rows, reverse ? '?' : '/');
    fflush(stdout);

    char buf[128] = "";
    int len = 0;

    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) break;
        if (c == '\r' || c == '\n') break;
        if (c == 27) return; /* Esc */
        if ((c == 127 || c == 8) && len > 0) {
            buf[--len] = '\0';
            printf("\b \b");
            fflush(stdout);
        } else if (c >= 32 && c <= 126 && len < (int)sizeof(buf) - 1) {
            buf[len++] = c;
            buf[len] = '\0';
            putchar(c);
            fflush(stdout);
        }
    }

    if (len > 0) {
        strncpy(g_search_query, buf, sizeof(g_search_query) - 1);
        g_search_reverse = reverse;
        int match = reverse ? find_prev_match(g_top_line - 1) : find_next_match(g_top_line);
        if (match >= 0) g_top_line = match;
    }
}

static void prompt_goto_line(void)
{
    printf("\033[%d;1H\033[7m: \033[0m", g_rows);
    fflush(stdout);

    char buf[32] = "";
    int len = 0;

    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) break;
        if (c == '\r' || c == '\n') break;
        if (c == 27) return;
        if ((c == 127 || c == 8) && len > 0) {
            buf[--len] = '\0';
            printf("\b \b");
            fflush(stdout);
        } else if (c >= '0' && c <= '9' && len < (int)sizeof(buf) - 1) {
            buf[len++] = c;
            buf[len] = '\0';
            putchar(c);
            fflush(stdout);
        }
    }

    if (len > 0) {
        int target = atoi(buf);
        if (target > 0) {
            g_top_line = target - 1;
            if (g_top_line >= g_num_lines) g_top_line = g_num_lines - 1;
            if (g_top_line < 0) g_top_line = 0;
        }
    }
}

int main(int argc, char **argv)
{
    const char *filepath = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-N") == 0) {
            g_show_line_nums = true;
        } else if (argv[i][0] != '-') {
            filepath = argv[i];
        }
    }

    if (filepath) {
        int fd = open(filepath, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "less: %s: No such file or directory\n", filepath);
            return 1;
        }
        strncpy(g_filename, filepath, sizeof(g_filename) - 1);
        load_stream(fd);
        close(fd);
    } else {
        load_stream(STDIN_FILENO);
    }

    enable_raw_mode();

    bool running = true;
    while (running) {
        render_screen();

        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) continue;

        if (g_show_help) {
            if (c == 'q' || c == 'h' || c == 27 || c == ' ') {
                g_show_help = false;
            }
            continue;
        }

        /* Number prefix accumulator (e.g. 50g) */
        if (c >= '0' && c <= '9' && (g_num_acc > 0 || c != '0')) {
            g_num_acc = g_num_acc * 10 + (c - '0');
            continue;
        }

        int visible_rows = g_rows - 1;

        if (c == 'q' || c == 'Q') {
            running = false;
            break;
        } else if (c == 'h' || c == 'H') {
            g_show_help = true;
            g_num_acc = 0;
            continue;
        } else if (c == 'j' || c == '\r' || c == '\n') {
            int step = (g_num_acc > 0) ? g_num_acc : 1;
            g_top_line += step;
            if (g_top_line >= g_num_lines) g_top_line = g_num_lines - 1;
            g_num_acc = 0;
        } else if (c == 'k') {
            int step = (g_num_acc > 0) ? g_num_acc : 1;
            g_top_line -= step;
            if (g_top_line < 0) g_top_line = 0;
            g_num_acc = 0;
        } else if (c == ' ' || c == 6) { /* Space / Ctrl+F */
            g_top_line += visible_rows;
            if (g_top_line >= g_num_lines) g_top_line = g_num_lines - 1;
            g_num_acc = 0;
        } else if (c == 'b' || c == 2) { /* b / Ctrl+B */
            g_top_line -= visible_rows;
            if (g_top_line < 0) g_top_line = 0;
            g_num_acc = 0;
        } else if (c == 'g') {
            if (g_num_acc > 0) {
                g_top_line = g_num_acc - 1;
                if (g_top_line >= g_num_lines) g_top_line = g_num_lines - 1;
                if (g_top_line < 0) g_top_line = 0;
            } else {
                g_top_line = 0;
            }
            g_num_acc = 0;
        } else if (c == 'G') {
            if (g_num_acc > 0) {
                g_top_line = g_num_acc - 1;
                if (g_top_line >= g_num_lines) g_top_line = g_num_lines - 1;
            } else {
                g_top_line = g_num_lines - visible_rows;
            }
            if (g_top_line < 0) g_top_line = 0;
            g_num_acc = 0;
        } else if (c == ':') {
            prompt_goto_line();
            g_num_acc = 0;
        } else if (c == '/') {
            prompt_search(false);
            g_num_acc = 0;
        } else if (c == '?') {
            prompt_search(true);
            g_num_acc = 0;
        } else if (c == 'n') {
            int match = g_search_reverse ? find_prev_match(g_top_line - 1) : find_next_match(g_top_line + 1);
            if (match >= 0) g_top_line = match;
            g_num_acc = 0;
        } else if (c == 'N') {
            int match = g_search_reverse ? find_next_match(g_top_line + 1) : find_prev_match(g_top_line - 1);
            if (match >= 0) g_top_line = match;
            g_num_acc = 0;
        } else if (c == 27) {
            char seq[3];
            if (read(STDIN_FILENO, &seq[0], 1) == 0) continue;
            if (read(STDIN_FILENO, &seq[1], 1) == 0) continue;

            if (seq[0] == '[') {
                if (seq[1] == 'A') { /* Up */
                    if (g_top_line > 0) g_top_line--;
                } else if (seq[1] == 'B') { /* Down */
                    if (g_top_line < g_num_lines - 1) g_top_line++;
                } else if (seq[1] == '5') { /* PgUp */
                    read(STDIN_FILENO, &seq[2], 1);
                    g_top_line -= visible_rows;
                    if (g_top_line < 0) g_top_line = 0;
                } else if (seq[1] == '6') { /* PgDn */
                    read(STDIN_FILENO, &seq[2], 1);
                    g_top_line += visible_rows;
                    if (g_top_line >= g_num_lines) g_top_line = g_num_lines - 1;
                }
            }
            g_num_acc = 0;
        }
    }

    disable_raw_mode();
    for (int i = 0; i < g_num_lines; i++) free(g_lines[i]);
    free(g_lines);
    return 0;
}
