/* ============================================================================
 * AzamiOS — Terminal Hex Editor (hexedit v1.0)
 * File: userland/apps/hexedit/main.c
 *
 * Features:
 *   • Full-screen interactive terminal raw mode hex viewer & editor
 *   • Dual-pane view: 16-byte Hex representation & ASCII text column
 *   • Tab toggles between Hex nibble editing and direct ASCII editing
 *   • In-place binary modification with dirty indicator & Ctrl+S save
 *   • Goto offset ('g') & Search string/hex ('/')
 *   • Catppuccin Mocha themed ANSI styling
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>

#define MAX_BUFFER_SIZE (2 * 1024 * 1024) /* 2 MB max file size */

typedef enum {
    PANE_HEX = 0,
    PANE_ASCII = 1
} edit_pane_t;

static uint8_t *g_buf = NULL;
static size_t   g_buf_size = 0;
static size_t   g_cursor_offset = 0;
static int      g_cursor_nibble = 0; /* 0 = high nibble, 1 = low nibble */
static size_t   g_top_offset = 0;
static bool     g_dirty = false;
static bool     g_readonly = false;
static char     g_filepath[256] = "";
static edit_pane_t g_active_pane = PANE_HEX;

static struct termios g_orig_termios;
static int g_term_cols = 80;
static int g_term_rows = 24;
static int g_page_bytes = 256; /* 16 bytes * 16 rows */

/* ── Terminal Raw Mode ─────────────────────────────────────────────────────── */
static void disable_raw_mode(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
    printf("\033[?25h\033[0m"); /* Show cursor, reset colors */
    fflush(stdout);
}

static void enable_raw_mode(void)
{
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    atexit(disable_raw_mode);

    struct termios raw = g_orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void update_window_size(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        g_term_cols = ws.ws_col;
        g_term_rows = ws.ws_row;
    } else {
        g_term_cols = 80;
        g_term_rows = 24;
    }
    int usable_rows = g_term_rows - 3; /* top header + status + footer */
    if (usable_rows < 4) usable_rows = 4;
    g_page_bytes = usable_rows * 16;
}

/* ── File Operations ───────────────────────────────────────────────────────── */
static bool load_file(const char *path)
{
    strncpy(g_filepath, path, sizeof(g_filepath) - 1);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        /* New file */
        g_buf = malloc(16);
        if (!g_buf) return false;
        memset(g_buf, 0, 16);
        g_buf_size = 16;
        g_dirty = true;
        return true;
    }

    off_t sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (sz < 0) { close(fd); return false; }

    if (sz == 0) {
        g_buf = malloc(16);
        memset(g_buf, 0, 16);
        g_buf_size = 16;
        close(fd);
        return true;
    }

    if (sz > MAX_BUFFER_SIZE) sz = MAX_BUFFER_SIZE;
    g_buf = malloc(sz);
    if (!g_buf) { close(fd); return false; }

    ssize_t rd = read(fd, g_buf, sz);
    close(fd);
    g_buf_size = (rd > 0) ? (size_t)rd : 0;
    g_dirty = false;
    return true;
}

static bool save_file(void)
{
    if (g_readonly) return false;
    int fd = open(g_filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;

    ssize_t wr = write(fd, g_buf, g_buf_size);
    close(fd);
    if (wr == (ssize_t)g_buf_size) {
        g_dirty = false;
        return true;
    }
    return false;
}

/* ── Rendering ─────────────────────────────────────────────────────────────── */
static void render_screen(void)
{
    update_window_size();
    int visible_rows = g_term_rows - 3;

    /* Hide cursor, move to home */
    printf("\033[?25l\033[H");

    /* 1. Header Bar (Catppuccin Mauve / Mantle) */
    printf("\033[48;2;24;24;37m\033[38;2;203;166;247m\033[1m AZAMI HEX EDIT \033[0m");
    printf("\033[48;2;24;24;37m\033[38;2;205;214;244m %-20s \033[38;2;166;173;200m(%zu bytes)%s\033[K",
           g_filepath, g_buf_size, g_dirty ? " \033[38;2;250;179;135m[MODIFIED*]\033[0m\033[48;2;24;24;37m" : "");

    /* Mode indicator on right */
    printf("\033[%d;%dH\033[48;2;24;24;37m\033[38;2;116;199;236mPane: \033[1m%s \033[0m\r\n",
           1, g_term_cols - 14, (g_active_pane == PANE_HEX) ? "HEX  " : "ASCII");

    /* 2. Hex Dump Rows */
    for (int r = 0; r < visible_rows; r++) {
        size_t row_offset = g_top_offset + (size_t)r * 16;
        if (row_offset >= g_buf_size) {
            printf("\033[38;2;69;71;90m~\033[K\r\n");
            continue;
        }

        /* Offset column in Mauve */
        printf("\033[38;2;203;166;247m%08zX: \033[0m", row_offset);

        /* 16 Hex bytes */
        for (int b = 0; b < 16; b++) {
            size_t off = row_offset + b;
            if (b == 8) printf(" ");

            if (off < g_buf_size) {
                uint8_t val = g_buf[off];
                bool is_cursor = (off == g_cursor_offset);

                if (is_cursor && g_active_pane == PANE_HEX) {
                    /* Active editing byte */
                    printf("\033[48;2;203;166;247m\033[38;2;17;17;27m\033[1m%02X\033[0m ", val);
                } else if (is_cursor && g_active_pane == PANE_ASCII) {
                    /* Sub-cursor outline */
                    printf("\033[48;2;69;71;90m\033[38;2;205;214;244m%02X\033[0m ", val);
                } else if (val == 0) {
                    /* Zero byte: dim gray */
                    printf("\033[38;2;88;91;112m%02X\033[0m ", val);
                } else if (isprint(val)) {
                    /* Printable: sapphire */
                    printf("\033[38;2;116;199;236m%02X\033[0m ", val);
                } else {
                    /* Other byte: text color */
                    printf("\033[38;2;205;214;244m%02X\033[0m ", val);
                }
            } else {
                printf("   ");
            }
        }

        /* ASCII representation */
        printf(" \033[38;2;88;91;112m|\033[0m");
        for (int b = 0; b < 16; b++) {
            size_t off = row_offset + b;
            if (off < g_buf_size) {
                uint8_t val = g_buf[off];
                char ch = isprint(val) ? (char)val : '.';
                bool is_cursor = (off == g_cursor_offset);

                if (is_cursor && g_active_pane == PANE_ASCII) {
                    printf("\033[48;2;166;227;161m\033[38;2;17;17;27m\033[1m%c\033[0m", ch);
                } else if (is_cursor && g_active_pane == PANE_HEX) {
                    printf("\033[48;2;69;71;90m\033[38;2;205;214;244m%c\033[0m", ch);
                } else if (val == 0) {
                    printf("\033[38;2;88;91;112m%c\033[0m", ch);
                } else {
                    printf("\033[38;2;166;227;161m%c\033[0m", ch);
                }
            } else {
                printf(" ");
            }
        }
        printf("\033[38;2;88;91;112m|\033[0m\033[K\r\n");
    }

    /* 3. Status Bar */
    printf("\033[48;2;30;30;46m\033[38;2;205;214;244m");
    printf(" Offset: \033[1m0x%08zX\033[0m (%zu / %zu) | Val: 0x%02X ('%c')",
           g_cursor_offset, g_cursor_offset, g_buf_size,
           (g_cursor_offset < g_buf_size) ? g_buf[g_cursor_offset] : 0,
           (g_cursor_offset < g_buf_size && isprint(g_buf[g_cursor_offset])) ? g_buf[g_cursor_offset] : '.');
    printf("\033[K\r\n");

    /* 4. Footer Help */
    printf("\033[48;2;24;24;37m\033[38;2;166;173;200m");
    printf(" \033[1mTab\033[0m:Switch  \033[1m^S\033[0m:Save  \033[1m^Q\033[0m/q:Quit  \033[1mg\033[0m:Goto  \033[1m/\033[0m:Search  \033[1mArrows\033[0m:Navigate");
    printf("\033[K");

    fflush(stdout);
}

/* ── User Interaction ─────────────────────────────────────────────────────── */
static void prompt_goto(void)
{
    printf("\033[%d;1H\033[48;2;49;50;68m\033[38;2;249;226;175m Goto Offset (hex/dec): \033[0m\033[K", g_term_rows);
    fflush(stdout);

    char input[32] = "";
    int len = 0;
    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) break;
        if (c == '\r' || c == '\n') break;
        if (c == 27) return; /* Esc cancel */
        if ((c == 127 || c == 8) && len > 0) {
            input[--len] = '\0';
            printf("\b \b");
            fflush(stdout);
        } else if (isxdigit(c) || c == 'x' || c == 'X') {
            if (len < (int)sizeof(input) - 1) {
                input[len++] = c;
                input[len] = '\0';
                putchar(c);
                fflush(stdout);
            }
        }
    }

    if (len > 0) {
        size_t target = 0;
        if (strncmp(input, "0x", 2) == 0 || strncmp(input, "0X", 2) == 0) {
            target = (size_t)strtoull(input + 2, NULL, 16);
        } else {
            target = (size_t)strtoull(input, NULL, 0);
        }
        if (target < g_buf_size) {
            g_cursor_offset = target;
            if (g_cursor_offset < g_top_offset || g_cursor_offset >= g_top_offset + (size_t)g_page_bytes) {
                g_top_offset = (g_cursor_offset / 16) * 16;
            }
        }
    }
}

static void prompt_search(void)
{
    printf("\033[%d;1H\033[48;2;49;50;68m\033[38;2;166;227;161m Search text: \033[0m\033[K", g_term_rows);
    fflush(stdout);

    char query[64] = "";
    int len = 0;
    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) break;
        if (c == '\r' || c == '\n') break;
        if (c == 27) return;
        if ((c == 127 || c == 8) && len > 0) {
            query[--len] = '\0';
            printf("\b \b");
            fflush(stdout);
        } else if (isprint(c) && len < (int)sizeof(query) - 1) {
            query[len++] = c;
            query[len] = '\0';
            putchar(c);
            fflush(stdout);
        }
    }

    if (len > 0) {
        for (size_t i = g_cursor_offset + 1; i + len <= g_buf_size; i++) {
            if (memcmp(g_buf + i, query, len) == 0) {
                g_cursor_offset = i;
                g_top_offset = (i / 16) * 16;
                return;
            }
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file>\n", argv[0]);
        return 1;
    }

    if (!load_file(argv[1])) {
        fprintf(stderr, "Error: Cannot open '%s'\n", argv[1]);
        return 1;
    }

    enable_raw_mode();

    bool running = true;
    while (running) {
        render_screen();

        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) continue;

        /* Exit on Ctrl+Q or 'q' */
        if (c == 17 || c == 'q') {
            running = false;
            break;
        }

        /* Save on Ctrl+S */
        if (c == 19) {
            save_file();
            continue;
        }

        /* Switch pane on Tab */
        if (c == '\t') {
            g_active_pane = (g_active_pane == PANE_HEX) ? PANE_ASCII : PANE_HEX;
            g_cursor_nibble = 0;
            continue;
        }

        /* Goto offset */
        if (c == 'g') {
            prompt_goto();
            continue;
        }

        /* Search */
        if (c == '/') {
            prompt_search();
            continue;
        }

        /* Arrow / Navigation Keys */
        if (c == 27) {
            char seq[3];
            if (read(STDIN_FILENO, &seq[0], 1) == 0) continue;
            if (read(STDIN_FILENO, &seq[1], 1) == 0) continue;

            if (seq[0] == '[') {
                if (seq[1] == 'A') { /* Up */
                    if (g_cursor_offset >= 16) g_cursor_offset -= 16;
                } else if (seq[1] == 'B') { /* Down */
                    if (g_cursor_offset + 16 < g_buf_size) g_cursor_offset += 16;
                } else if (seq[1] == 'C') { /* Right */
                    if (g_cursor_offset + 1 < g_buf_size) g_cursor_offset++;
                } else if (seq[1] == 'D') { /* Left */
                    if (g_cursor_offset > 0) g_cursor_offset--;
                } else if (seq[1] == '5') { /* Page Up */
                    read(STDIN_FILENO, &seq[2], 1);
                    if (g_cursor_offset >= (size_t)g_page_bytes) g_cursor_offset -= g_page_bytes;
                    else g_cursor_offset = 0;
                } else if (seq[1] == '6') { /* Page Down */
                    read(STDIN_FILENO, &seq[2], 1);
                    if (g_cursor_offset + g_page_bytes < g_buf_size) g_cursor_offset += g_page_bytes;
                    else if (g_buf_size > 0) g_cursor_offset = g_buf_size - 1;
                }
            }
        } else if (c == 'h') {
            if (g_cursor_offset > 0) g_cursor_offset--;
        } else if (c == 'l') {
            if (g_cursor_offset + 1 < g_buf_size) g_cursor_offset++;
        } else if (c == 'k') {
            if (g_cursor_offset >= 16) g_cursor_offset -= 16;
        } else if (c == 'j') {
            if (g_cursor_offset + 16 < g_buf_size) g_cursor_offset += 16;
        } else {
            /* Editing */
            if (g_active_pane == PANE_HEX && isxdigit((unsigned char)c)) {
                int nibble = (c >= '0' && c <= '9') ? (c - '0') :
                             (c >= 'a' && c <= 'f') ? (c - 'a' + 10) : (c - 'A' + 10);
                if (g_cursor_offset < g_buf_size) {
                    if (g_cursor_nibble == 0) {
                        g_buf[g_cursor_offset] = (g_buf[g_cursor_offset] & 0x0F) | (nibble << 4);
                        g_cursor_nibble = 1;
                    } else {
                        g_buf[g_cursor_offset] = (g_buf[g_cursor_offset] & 0xF0) | nibble;
                        g_cursor_nibble = 0;
                        if (g_cursor_offset + 1 < g_buf_size) g_cursor_offset++;
                    }
                    g_dirty = true;
                }
            } else if (g_active_pane == PANE_ASCII && isprint((unsigned char)c)) {
                if (g_cursor_offset < g_buf_size) {
                    g_buf[g_cursor_offset] = (uint8_t)c;
                    g_dirty = true;
                    if (g_cursor_offset + 1 < g_buf_size) g_cursor_offset++;
                }
            }
        }

        /* Adjust scroll window */
        int visible_rows = g_term_rows - 3;
        size_t visible_bytes = visible_rows * 16;
        if (g_cursor_offset < g_top_offset) {
            g_top_offset = (g_cursor_offset / 16) * 16;
        } else if (g_cursor_offset >= g_top_offset + visible_bytes) {
            g_top_offset = ((g_cursor_offset - visible_bytes + 16) / 16) * 16;
        }
    }

    disable_raw_mode();
    if (g_buf) free(g_buf);
    return 0;
}
