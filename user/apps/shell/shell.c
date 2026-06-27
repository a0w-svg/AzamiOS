/**
 * shell.c  –  AzamiOS 640x480 Graphical Terminal UI (Ring 3 True Color)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <gui.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/cpu.h>
#include <fcntl.h>

#define COLS 78
#define ROWS 54

#define COL_BG         0x00090D16
#define COL_BAR        0x001E293B
#define COL_BTN        0x002563EB
#define COL_TEXT_WHITE 0x00FFFFFF
#define COL_TEXT_GREEN 0x0010B981
#define COL_TEXT_GOLD  0x00F59E0B
#define COL_TEXT_CYAN  0x0038BDF8
#define COL_TEXT_RED   0x00EF4444

typedef struct {
    char buf[ROWS][COLS];
    uint32_t color[ROWS][COLS];
    int r;
    int c;
} shell_state_t;

static char term_buf[ROWS][COLS];
static uint32_t term_color[ROWS][COLS];
static int term_r = 0;
static int term_c = 0;

static void tui_clear(void) {
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            term_buf[r][c] = ' ';
            term_color[r][c] = COL_TEXT_WHITE;
        }
    }
    term_r = 0;
    term_c = 0;
}

static void tui_scroll(void) {
    for (int r = 0; r < ROWS - 1; r++) {
        memcpy(term_buf[r], term_buf[r + 1], COLS);
        memcpy(term_color[r], term_color[r + 1], COLS * sizeof(uint32_t));
    }
    for (int c = 0; c < COLS; c++) {
        term_buf[ROWS - 1][c] = ' ';
        term_color[ROWS - 1][c] = COL_TEXT_WHITE;
    }
    term_r = ROWS - 1;
}

static void tui_putc(char ch, uint32_t color) {
    if (ch == '\n' || ch == '\r') {
        term_c = 0;
        term_r++;
        if (term_r >= ROWS) tui_scroll();
        return;
    }
    if (ch == '\b' || ch == 127) {
        if (term_c > 0) {
            term_c--;
            term_buf[term_r][term_c] = ' ';
            term_color[term_r][term_c] = COL_TEXT_WHITE;
        }
        return;
    }
    term_buf[term_r][term_c] = ch;
    term_color[term_r][term_c] = color;
    term_c++;
    if (term_c >= COLS) {
        term_c = 0;
        term_r++;
        if (term_r >= ROWS) tui_scroll();
    }
}

static void tui_print(const char *str, uint32_t color) {
    while (*str) {
        tui_putc(*str++, color);
    }
}

static void render_tui(void) {
    for (int r = 0; r < ROWS; r++) {
        char word[COLS + 1];
        int wp = 0;
        uint32_t cur_col = term_color[r][0];
        int start_c = 0;
        for (int c = 0; c < COLS; c++) {
            if (term_color[r][c] != cur_col) {
                if (wp > 0) {
                    word[wp] = 0;
                    draw_text(8 + start_c * 8, 24 + r * 8, word, cur_col, COL_BG);
                    wp = 0;
                }
                cur_col = term_color[r][c];
                start_c = c;
            }
            word[wp++] = term_buf[r][c];
        }
        if (wp > 0) {
            word[wp] = 0;
            draw_text(8 + start_c * 8, 24 + r * 8, word, cur_col, COL_BG);
        }
    }
}

void _start(void) {
    init_graphics();

    int s_fd = open(".shell_state", 0);
    if (s_fd >= 0) {
        shell_state_t st;
        int n = read(s_fd, &st, sizeof(st));
        close(s_fd);
        int d_fd = open(".shell_state", O_WRONLY | O_CREAT, 0);
        if (d_fd >= 0) { close(d_fd); }

        if (n == sizeof(st)) {
            memcpy(term_buf, st.buf, sizeof(term_buf));
            memcpy(term_color, st.color, sizeof(term_color));
            term_r = st.r;
            term_c = st.c;
        } else {
            tui_clear();
        }

        int o_fd = open("cmd_out", 0);
        if (o_fd >= 0) {
            char out_buf[1024];
            int rn = read(o_fd, out_buf, sizeof(out_buf) - 1);
            close(o_fd);
            int tr_fd = open("cmd_out", O_WRONLY | O_CREAT, 0);
            if (tr_fd >= 0) { close(tr_fd); }

            if (rn > 0) {
                out_buf[rn] = '\0';
                if (strcmp(out_buf, "__CLEAR__") == 0 || strncmp(out_buf, "__CLEAR__", 9) == 0) {
                    tui_clear();
                } else if (strncmp(out_buf, "\033WIN_", 5) == 0) {
                    tui_print("Opening application window in GUI...\n", COL_TEXT_CYAN);
                } else {
                    tui_print(out_buf, COL_TEXT_WHITE);
                    if (out_buf[rn - 1] != '\n') tui_print("\n", COL_TEXT_WHITE);
                }
            }
        }
    } else {
        tui_clear();
        tui_print("+------------------------------------------------------+\n", COL_TEXT_CYAN);
        tui_print("|           AzamiOS Standalone Shell v2.0              |\n", COL_TEXT_GOLD);
        tui_print("+------------------------------------------------------+\n", COL_TEXT_CYAN);
        tui_print("Type 'help' for built-in commands, 'wm' for Desktop GUI.\n\n", COL_TEXT_WHITE);
    }

    char cmd[64];
    int p = 0;
    cmd[0] = 0;
    tui_print("azami@os:~$ ", COL_TEXT_GREEN);

    bool prev_left = false;
    int blink = 0;

    for (;;) {
        mouse_state_t ms;
        get_mouse_state(&ms);

        /* Check Top Bar Button Click [ Return to Desktop ] */
        if (ms.left_btn && !prev_left) {
            if (ms.x >= 520 && ms.x <= 630 && ms.y <= 20) {
                exec("wm");
            }
        }
        prev_left = ms.left_btn;

        /* Check Keyboard */
        while (has_char()) {
            int c = getchar();
            if (c == '\n' || c == '\r') {
                tui_putc('\n', COL_TEXT_WHITE);
                cmd[p] = 0;

                if (cmd[0] == '\0') {
                    tui_print("azami@os:~$ ", COL_TEXT_GREEN);
                    continue;
                }
                if (strcmp(cmd, "exit") == 0) {
                    exit(0);
                }

                char cmd_word[32];
                int i = 0;
                char *ptr = cmd;
                while (*ptr && *ptr != ' ' && i < 31) {
                    cmd_word[i++] = *ptr++;
                }
                cmd_word[i] = '\0';
                while (*ptr == ' ') ptr++;

                int arg_fd = open("cmd_args", O_WRONLY | O_CREAT, 0);
                if (arg_fd >= 0) {
                    write(arg_fd, ptr, strlen(ptr));
                    close(arg_fd);
                }
                int out_fd = open("cmd_out", O_WRONLY | O_CREAT, 0);
                if (out_fd >= 0) { close(out_fd); }

                int st_fd = open(".shell_state", O_WRONLY | O_CREAT, 0);
                if (st_fd >= 0) {
                    shell_state_t st;
                    memcpy(st.buf, term_buf, sizeof(term_buf));
                    memcpy(st.color, term_color, sizeof(term_color));
                    st.r = term_r;
                    st.c = term_c;
                    write(st_fd, &st, sizeof(st));
                    close(st_fd);
                }

                exec(cmd_word);

                int d_fd = open(".shell_state", O_WRONLY | O_CREAT, 0);
                if (d_fd >= 0) { close(d_fd); }

                tui_print("bash: command not found: ", COL_TEXT_RED);
                tui_print(cmd_word, COL_TEXT_WHITE);
                tui_print("\n", COL_TEXT_WHITE);

                p = 0;
                cmd[0] = 0;
                tui_print("azami@os:~$ ", COL_TEXT_GREEN);
            } else if (c == '\b' || c == 127) {
                if (p > 0) {
                    p--;
                    cmd[p] = 0;
                    tui_putc('\b', COL_TEXT_WHITE);
                }
            } else if (c >= 32 && c <= 126 && p < 63) {
                cmd[p++] = c;
                cmd[p] = 0;
                tui_putc(c, COL_TEXT_WHITE);
            }
        }

        /* Render Frame */
        /* 1. Screen Canvas BG (Onyx Black) */
        draw_rect(0, 20, 640, 460, COL_BG);

        /* 2. Top Titlebar */
        draw_rect(0, 0, 640, 20, COL_BAR);
        draw_rect(0, 19, 640, 1, COL_BTN);
        draw_text(8, 6, "AzamiOS Standalone Shell v2.0", COL_TEXT_WHITE, COL_BAR);

        draw_rect(520, 3, 110, 14, COL_BTN);
        draw_text(526, 6, "Return to GUI", COL_TEXT_WHITE, COL_BTN);

        /* 3. Render Text Buffer */
        render_tui();

        /* 4. Render Blinking Cursor */
        blink = (blink + 1) % 40;
        if (blink < 20) {
            int cx = 8 + term_c * 8;
            int cy = 24 + term_r * 8;
            draw_rect(cx, cy + 6, 8, 2, COL_TEXT_GREEN);
        }

        gfx_flip();

        for (volatile int k = 0; k < 150; k++);
    }

    exit(0);
}
