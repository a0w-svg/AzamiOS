/**
 * shell.c  –  AzamiOS 640x480 Graphical Terminal UI (Ring 3 True Color)
 */
#include "../libc/include/stdio.h"
#include "../libc/include/stdlib.h"
#include "../libc/include/string.h"
#include "../libc/include/time.h"
#include "../libc/include/gui.h"

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
    tui_clear();

    tui_print("+------------------------------------------------------+\n", COL_TEXT_CYAN);
    tui_print("|           AzamiOS Standalone Shell v2.0              |\n", COL_TEXT_GOLD);
    tui_print("+------------------------------------------------------+\n", COL_TEXT_CYAN);
    tui_print("Type 'help' for built-in commands, 'wm' for Desktop GUI.\n\n", COL_TEXT_WHITE);

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

                if (strcmp(cmd, "help") == 0) {
                    tui_print("Built-in TUI commands:\n", COL_TEXT_GOLD);
                    tui_print("  help  - show this help menu\n", COL_TEXT_WHITE);
                    tui_print("  time  - display live RTC time\n", COL_TEXT_WHITE);
                    tui_print("  clear - clear terminal screen\n", COL_TEXT_WHITE);
                    tui_print("  wm    - return to Desktop GUI\n", COL_TEXT_CYAN);
                    tui_print("  exit  - shutdown terminal\n", COL_TEXT_RED);
                } else if (strcmp(cmd, "time") == 0) {
                    time_t t;
                    rtc_get_time(&t);
                    char time_str[32];
                    itoa(t.hour, time_str, 10);
                    tui_print("Hardware RTC Time: ", COL_TEXT_CYAN);
                    if (t.hour < 10) tui_print("0", COL_TEXT_WHITE);
                    tui_print(time_str, COL_TEXT_WHITE);
                    tui_print(":", COL_TEXT_WHITE);
                    itoa(t.minute, time_str, 10);
                    if (t.minute < 10) tui_print("0", COL_TEXT_WHITE);
                    tui_print(time_str, COL_TEXT_WHITE);
                    tui_print(":", COL_TEXT_WHITE);
                    itoa(t.second, time_str, 10);
                    if (t.second < 10) tui_print("0", COL_TEXT_WHITE);
                    tui_print(time_str, COL_TEXT_WHITE);
                    tui_print("\n", COL_TEXT_WHITE);
                } else if (strcmp(cmd, "clear") == 0) {
                    tui_clear();
                } else if (strcmp(cmd, "wm") == 0 || strcmp(cmd, "gui") == 0) {
                    tui_print("Launching Window Manager...\n", COL_TEXT_GOLD);
                    exec("wm");
                } else if (strcmp(cmd, "exit") == 0) {
                    exit(0);
                } else if (cmd[0] != 0) {
                    tui_print("bash: command not found: ", COL_TEXT_RED);
                    tui_print(cmd, COL_TEXT_WHITE);
                    tui_print("\n", COL_TEXT_WHITE);
                }

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
