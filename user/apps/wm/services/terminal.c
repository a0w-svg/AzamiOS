/**
 * terminal.c — AzamiOS Interactive Terminal Window Service
 *
 * EDUCATIONAL SECURITY & BOUNDARY EXPLANATIONS:
 * 1. Safe Terminal Text Buffering:
 *    Terminal display grids (`term_buf`) are strictly bounded by `TERM_ROWS` and `TERM_COLS`.
 *    Character injection loops verify column pointers (`term_c`) before memory assignment,
 *    triggering clean circular scrolling (`term_scroll`) to prevent out-of-bounds writes.
 * 2. Secure Command Execution Lifecycle:
 *    Shell commands execute within User Mode (Ring 3) isolation. Issuing window termination
 *    commands (`exit`) invokes the secure mutator API `wm_close_window` rather than directly
 *    flipping global flags, guaranteeing deterministic resource cleanup.
 */

#include "../wm.h"

/* ── Terminal state ──────────────────────────────────────────────────────── */
typedef struct {
    char buf[TERM_ROWS][TERM_COLS];
    uint32_t color[TERM_ROWS][TERM_COLS];
    int r;
    int c;
} wm_term_state_t;

static char     term_buf[TERM_ROWS][TERM_COLS];
static uint32_t term_color[TERM_ROWS][TERM_COLS];
static int term_r = 0, term_c = 0;
static char cmd_buf[64];
static int  cmd_p = 0;

static void term_clear(void) {
    for (int r = 0; r < TERM_ROWS; r++)
        for (int c = 0; c < TERM_COLS; c++) {
            term_buf[r][c] = ' ';
            term_color[r][c] = COL_TEXT_WHITE;
        }
    term_r = 0; term_c = 0;
}

static void term_scroll(void) {
    for (int r = 0; r < TERM_ROWS - 1; r++) {
        memcpy(term_buf[r], term_buf[r + 1], TERM_COLS);
        memcpy(term_color[r], term_color[r + 1], TERM_COLS * sizeof(uint32_t));
    }
    for (int c = 0; c < TERM_COLS; c++) {
        term_buf[TERM_ROWS - 1][c] = ' ';
        term_color[TERM_ROWS - 1][c] = COL_TEXT_WHITE;
    }
    term_r = TERM_ROWS - 1;
}

static void term_putc(char ch, uint32_t col) {
    if (ch == '\n' || ch == '\r') {
        term_c = 0; term_r++;
        if (term_r >= TERM_ROWS) term_scroll();
        return;
    }
    if (ch == '\b' || ch == 127) {
        if (term_c > 0) { term_c--; term_buf[term_r][term_c] = ' '; }
        return;
    }
    term_buf[term_r][term_c] = ch;
    term_color[term_r][term_c] = col;
    term_c++;
    if (term_c >= TERM_COLS) { term_c = 0; term_r++; if (term_r >= TERM_ROWS) term_scroll(); }
}

static void term_print(const char *str, uint32_t col) {
    while (*str) term_putc(*str++, col);
}

static void process_command(rtc_time_t *t, uint32_t frame_cnt) {
    (void)t; (void)frame_cnt;
    if (cmd_buf[0] == '\0') {
        term_print("azami:~$ ", COL_TEXT_GREEN);
        return;
    }
    if (strcmp(cmd_buf, "exit") == 0) {
        int idx = find_win_by_type(WIN_TERMINAL);
        if (idx >= 0) wm_close_window(&g_wins[idx]);
        cmd_buf[0] = 0;
        cmd_p = 0;
        return;
    }

    char cmd_word[32];
    int i = 0;
    char *ptr = cmd_buf;
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

    int st_fd = open(".wm_term_state", O_WRONLY | O_CREAT, 0);
    if (st_fd >= 0) {
        wm_term_state_t st;
        memcpy(st.buf, term_buf, sizeof(term_buf));
        memcpy(st.color, term_color, sizeof(term_color));
        st.r = term_r;
        st.c = term_c;
        write(st_fd, &st, sizeof(st));
        close(st_fd);
    }

    exec(cmd_word);

    int d_fd = open(".wm_term_state", O_WRONLY | O_CREAT, 0);
    if (d_fd >= 0) { close(d_fd); }

    term_print("bash: command not found: ", COL_TEXT_RED);
    term_print(cmd_word, COL_TEXT_WHITE);
    term_print("\n", COL_TEXT_WHITE);

    cmd_buf[0] = 0;
    cmd_p = 0;
    term_print("azami:~$ ", COL_TEXT_GREEN);
}

static void terminal_init(window_t *w) {
    (void)w;
    int s_fd = open(".wm_term_state", 0);
    if (s_fd >= 0) {
        wm_term_state_t st;
        int n = read(s_fd, &st, sizeof(st));
        close(s_fd);
        int d_fd = open(".wm_term_state", O_WRONLY | O_CREAT, 0);
        if (d_fd >= 0) { close(d_fd); }

        if (n == sizeof(st)) {
            memcpy(term_buf, st.buf, sizeof(term_buf));
            memcpy(term_color, st.color, sizeof(term_color));
            term_r = st.r;
            term_c = st.c;
        } else {
            term_clear();
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
                    term_clear();
                } else if (strncmp(out_buf, "\033WIN_", 5) == 0) {
                    if (strncmp(out_buf + 5, "ABOUT", 5) == 0) open_win_type(WIN_ABOUT);
                    else if (strncmp(out_buf + 5, "NOTEPAD", 7) == 0) open_win_type(WIN_NOTEPAD);
                    else if (strncmp(out_buf + 5, "FILES", 5) == 0) open_win_type(WIN_FILES);
                    term_print("Opened GUI application window.\n", COL_TEXT_CYAN);
                } else {
                    term_print(out_buf, COL_TEXT_WHITE);
                    if (out_buf[rn - 1] != '\n') term_print("\n", COL_TEXT_WHITE);
                }
            }
        }
    } else {
        term_clear();
        term_print("AzamiOS True Color Shell v3.0\n", COL_TEXT_CYAN);
        term_print("Type 'help' for commands\n", COL_TEXT_WHITE);
    }
    term_print("azami:~$ ", COL_TEXT_GREEN);
    cmd_buf[0] = 0;
    cmd_p = 0;
}

static void terminal_render(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    (void)t; (void)frame_cnt;
    if (!w) return;
    int bx = w->x + 1;
    int by = w->y + TITLEBAR_H;
    int bw = w->w - 2;
    int bh = w->h - TITLEBAR_H - 1;

    draw_rect(bx, by, bw, bh, COL_TERM_BG);
    for (int r = 0; r < TERM_ROWS; r++) {
        char wb[TERM_COLS + 1];
        int wp = 0;
        uint32_t cc = term_color[r][0];
        int st = 0;
        for (int c = 0; c < TERM_COLS; c++) {
            if (term_color[r][c] != cc) {
                if (wp > 0) {
                    wb[wp] = 0;
                    draw_text(bx + 6 + st * 8, by + 4 + r * 8, wb, cc, COL_TERM_BG);
                    wp = 0;
                }
                cc = term_color[r][c];
                st = c;
            }
            wb[wp++] = term_buf[r][c];
        }
        if (wp > 0) {
            wb[wp] = 0;
            draw_text(bx + 6 + st * 8, by + 4 + r * 8, wb, cc, COL_TERM_BG);
        }
    }
    if (g_focus == w->id && blink < 20) {
        draw_rect(bx + 6 + term_c * 8, by + 10 + term_r * 8, 8, 2, COL_TEXT_GREEN);
    }
}

static void terminal_on_key(window_t *w, int c, rtc_time_t *t, uint32_t frame_cnt) {
    if (!w || !w->open || w->minimized) return;
    if (c == '\n' || c == '\r') {
        term_putc('\n', COL_TEXT_WHITE);
        cmd_buf[cmd_p] = 0;
        process_command(t, frame_cnt);
        cmd_p = 0;
        cmd_buf[0] = 0;
        term_print("azami:~$ ", COL_TEXT_GREEN);
    } else if (c == '\b' || c == 127) {
        if (cmd_p > 0) {
            cmd_p--;
            cmd_buf[cmd_p] = 0;
            term_putc('\b', COL_TEXT_WHITE);
        }
    } else if (c >= 32 && c <= 126 && cmd_p < (int)sizeof(cmd_buf) - 1) {
        cmd_buf[cmd_p++] = c;
        cmd_buf[cmd_p] = 0;
        term_putc(c, COL_TEXT_WHITE);
    }
}

void terminal_service_init(void) {
    static const wm_service_t terminal_srv = {
        WIN_TERMINAL,
        "Terminal",
        WM_SRV_FLAG_NONE,
        terminal_init,
        NULL,
        NULL,
        terminal_render,
        terminal_on_key
    };
    wm_register_service(&terminal_srv);
}
