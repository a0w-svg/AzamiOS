/**
 * wm.c  –  AzamiOS 640x480 True Color Window Manager & 60+ FPS Desktop Environment
 */
#include "../libc/include/stdio.h"
#include "../libc/include/stdlib.h"
#include "../libc/include/string.h"
#include "../libc/include/time.h"
#include "../libc/include/gui.h"

#define TERM_COLS 48
#define TERM_ROWS 26

#define COL_WALLPAPER  0x000F172A
#define COL_TASKBAR    0x001E293B
#define COL_START_BTN  0x002563EB
#define COL_START_ACT  0x00475569
#define COL_WIN_FRAME  0x00F8FAFC
#define COL_WIN_SHAD   0x00020617
#define COL_TITLE_ACT  0x001D4ED8
#define COL_TITLE_INA  0x0064748B
#define COL_TEXT_WHITE 0x00FFFFFF
#define COL_TEXT_DARK  0x000F172A
#define COL_TEXT_GREEN 0x0010B981
#define COL_TEXT_GOLD  0x00F59E0B
#define COL_TEXT_CYAN  0x0038BDF8
#define COL_TEXT_RED   0x00EF4444
#define COL_TERM_BG    0x00090D16

static char term_buf[TERM_ROWS][TERM_COLS];
static uint32_t term_color[TERM_ROWS][TERM_COLS];
static int term_r = 0;
static int term_c = 0;
static char cmd_buf[64];
static int cmd_p = 0;

typedef struct {
    int id;
    char title[32];
    int x, y, w, h;
    bool open;
    int type; /* 0=Welcome, 1=Terminal, 2=System Monitor */
} window_t;

#define MAX_WINS 3
static window_t g_wins[MAX_WINS];
static int g_focus = 0;

static void term_clear(void) {
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            term_buf[r][c] = ' ';
            term_color[r][c] = COL_TEXT_WHITE;
        }
    }
    term_r = 0;
    term_c = 0;
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
        term_c = 0;
        term_r++;
        if (term_r >= TERM_ROWS) term_scroll();
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
    term_color[term_r][term_c] = col;
    term_c++;
    if (term_c >= TERM_COLS) {
        term_c = 0;
        term_r++;
        if (term_r >= TERM_ROWS) term_scroll();
    }
}

static void term_print(const char *str, uint32_t col) {
    while (*str) term_putc(*str++, col);
}

static void format_time_str(char *buf, time_t *t) {
    char tmp[16];
    int p = 0;
    itoa(t->hour, tmp, 10);
    if (t->hour < 10) buf[p++] = '0';
    for(int i=0; tmp[i]; i++) buf[p++] = tmp[i];
    buf[p++] = ':';
    itoa(t->minute, tmp, 10);
    if (t->minute < 10) buf[p++] = '0';
    for(int i=0; tmp[i]; i++) buf[p++] = tmp[i];
    buf[p++] = ':';
    itoa(t->second, tmp, 10);
    if (t->second < 10) buf[p++] = '0';
    for(int i=0; tmp[i]; i++) buf[p++] = tmp[i];
    buf[p] = 0;
}

static void init_wins(void) {
    g_wins[0] = (window_t){0, "AzamiOS Welcome", 120, 80, 380, 220, true, 0};
    g_wins[1] = (window_t){1, "Terminal Emul", 180, 50, 412, 250, false, 1};
    g_wins[2] = (window_t){2, "System Monitor", 150, 110, 340, 200, false, 2};
}

void _start(void) {
    printf("AzamiOS 640x480 True Color Window Manager starting...\n");
    init_graphics();
    init_wins();
    term_clear();
    term_print("AzamiOS High-FPS True Color Shell v2.0\n", COL_TEXT_CYAN);
    term_print("Type 'help' or 'ls'\n", COL_TEXT_WHITE);
    term_print("azami:~$ ", COL_TEXT_GREEN);
    cmd_buf[0] = 0;

    bool dragging = false;
    int drag_x = 0, drag_y = 0;
    bool prev_left = false;
    bool start_menu = false;
    uint32_t frame_cnt = 0;
    int blink = 0;

    for (;;) {
        frame_cnt++;
        time_t t;
        rtc_get_time(&t);
        mouse_state_t ms;
        get_mouse_state(&ms);

        /* Handle Mouse Input */
        if (ms.left_btn && !prev_left) {
            bool handled = false;

            /* 1. Taskbar Start Button Click (X: 4..74, Y: 462..478) */
            if (ms.x >= 4 && ms.x <= 74 && ms.y >= 462 && ms.y <= 478) {
                start_menu = !start_menu;
                handled = true;
            }

            /* 2. Start Menu Overlay Clicks */
            if (start_menu && !handled) {
                if (ms.x >= 4 && ms.x <= 134 && ms.y >= 330 && ms.y <= 458) {
                    if (ms.y >= 355 && ms.y <= 380) { /* Terminal */
                        g_wins[1].open = true;
                        g_focus = 1;
                        start_menu = false;
                    } else if (ms.y >= 385 && ms.y <= 410) { /* Sys Monitor */
                        g_wins[2].open = true;
                        g_focus = 2;
                        start_menu = false;
                    } else if (ms.y >= 420 && ms.y <= 450) { /* Shutdown */
                        exit(0);
                    }
                    handled = true;
                }
            }

            /* 3. Desktop Icons Click */
            if (!start_menu && !handled) {
                if (ms.x >= 20 && ms.x <= 60 && ms.y >= 20 && ms.y <= 70) { /* System Icon */
                    g_wins[2].open = true;
                    g_focus = 2;
                    handled = true;
                } else if (ms.x >= 20 && ms.x <= 60 && ms.y >= 85 && ms.y <= 135) { /* Shell Icon */
                    g_wins[1].open = true;
                    g_focus = 1;
                    handled = true;
                }
            }

            /* 4. Windows Click (Z-index top down) */
            if (!handled) {
                int check_order[3] = { (g_focus+1)%3, (g_focus+2)%3, g_focus };
                for (int i = 2; i >= 0; i--) {
                    int idx = check_order[i];
                    window_t *w = &g_wins[idx];
                    if (!w->open) continue;

                    /* Close Button [X] */
                    if (ms.x >= w->x + w->w - 22 && ms.x <= w->x + w->w - 4 &&
                        ms.y >= w->y + 4 && ms.y <= w->y + 18) {
                        w->open = false;
                        handled = true;
                        break;
                    }

                    /* Titlebar Drag */
                    if (ms.x >= w->x && ms.x <= w->x + w->w &&
                        ms.y >= w->y && ms.y <= w->y + 22) {
                        g_focus = idx;
                        dragging = true;
                        drag_x = ms.x - w->x;
                        drag_y = ms.y - w->y;
                        handled = true;
                        break;
                    }

                    /* Body Focus Click */
                    if (ms.x >= w->x && ms.x <= w->x + w->w &&
                        ms.y >= w->y && ms.y <= w->y + w->h) {
                        g_focus = idx;
                        handled = true;
                        break;
                    }
                }
            }
        }

        if (!ms.left_btn) dragging = false;
        if (dragging && g_focus >= 0 && g_focus < MAX_WINS) {
            g_wins[g_focus].x = ms.x - drag_x;
            g_wins[g_focus].y = ms.y - drag_y;
        }
        prev_left = ms.left_btn;

        /* Handle Non-blocking Keyboard for Terminal */
        while (has_char()) {
            int c = getchar();
            if (g_focus == 1 && g_wins[1].open) {
                if (c == '\n' || c == '\r') {
                    term_putc('\n', COL_TEXT_WHITE);
                    cmd_buf[cmd_p] = 0;

                    if (strcmp(cmd_buf, "help") == 0) {
                        term_print("Commands: ls time clear whoami fps exit\n", COL_TEXT_GOLD);
                    } else if (strcmp(cmd_buf, "ls") == 0) {
                        term_print("bin/ initrd.tar README.TXT wm shell\n", COL_TEXT_CYAN);
                    } else if (strcmp(cmd_buf, "time") == 0) {
                        char tb[16];
                        format_time_str(tb, &t);
                        term_print("System RTC Clock: ", COL_TEXT_CYAN);
                        term_print(tb, COL_TEXT_WHITE);
                        term_print("\n", COL_TEXT_WHITE);
                    } else if (strcmp(cmd_buf, "whoami") == 0) {
                        term_print("root@AzamiOS (Ring 3 True Color GUI)\n", COL_TEXT_GREEN);
                    } else if (strcmp(cmd_buf, "fps") == 0) {
                        char fb[32];
                        itoa(frame_cnt, fb, 10);
                        term_print("Total Rendered Frames: ", COL_TEXT_GOLD);
                        term_print(fb, COL_TEXT_WHITE);
                        term_print(" (Unlocked 60+ FPS)\n", COL_TEXT_GREEN);
                    } else if (strcmp(cmd_buf, "clear") == 0) {
                        term_clear();
                    } else if (strcmp(cmd_buf, "exit") == 0) {
                        g_wins[1].open = false;
                    } else if (cmd_buf[0] != 0) {
                        term_print("unknown command: ", COL_TEXT_RED);
                        term_print(cmd_buf, COL_TEXT_WHITE);
                        term_print("\n", COL_TEXT_WHITE);
                    }

                    cmd_p = 0;
                    cmd_buf[0] = 0;
                    term_print("azami:~$ ", COL_TEXT_GREEN);
                } else if (c == '\b' || c == 127) {
                    if (cmd_p > 0) {
                        cmd_p--;
                        cmd_buf[cmd_p] = 0;
                        term_putc('\b', COL_TEXT_WHITE);
                    }
                } else if (c >= 32 && c <= 126 && cmd_p < 50) {
                    cmd_buf[cmd_p++] = c;
                    cmd_buf[cmd_p] = 0;
                    term_putc(c, COL_TEXT_WHITE);
                }
            }
        }

        /* ─── Render Frame (640x480) ─────────────────────────────────────── */
        /* 1. Wallpaper Canvas */
        draw_rect(0, 0, 640, 460, COL_WALLPAPER);

        /* 2. Desktop Icons */
        /* Icon 1: System */
        draw_rect(20, 20, 40, 32, COL_TEXT_WHITE);
        draw_rect(24, 24, 32, 24, COL_START_BTN);
        draw_text(16, 56, "System", COL_TEXT_WHITE, COL_WALLPAPER);

        /* Icon 2: Shell */
        draw_rect(20, 85, 40, 32, COL_TERM_BG);
        draw_text(26, 96, ">_", COL_TEXT_GREEN, COL_TERM_BG);
        draw_text(20, 122, "Shell", COL_TEXT_WHITE, COL_WALLPAPER);

        /* 3. Render Windows (Z-index back to front) */
        int draw_order[3] = { (g_focus+1)%3, (g_focus+2)%3, g_focus };
        for (int i = 0; i < 3; i++) {
            int idx = draw_order[i];
            window_t *w = &g_wins[idx];
            if (!w->open) continue;

            bool is_active = (g_focus == idx);
            uint32_t bar_col = is_active ? COL_TITLE_ACT : COL_TITLE_INA;

            /* Window Drop Shadow */
            draw_rect(w->x + 6, w->y + 6, w->w, w->h, COL_WIN_SHAD);
            /* Window Main Silver Canvas */
            draw_rect(w->x, w->y, w->w, w->h, COL_WIN_FRAME);
            /* Titlebar */
            draw_rect(w->x, w->y, w->w, 22, bar_col);
            draw_text(w->x + 8, w->y + 7, w->title, COL_TEXT_WHITE, bar_col);
            /* Close Button [X] */
            draw_rect(w->x + w->w - 22, w->y + 4, 18, 14, COL_TEXT_RED);
            draw_text(w->x + w->w - 17, w->y + 7, "X", COL_TEXT_WHITE, COL_TEXT_RED);

            /* Window Body Client Area */
            if (w->type == 0) { /* Welcome Window */
                draw_text(w->x+24, w->y+45, "Welcome to AzamiOS True Color v2.0", COL_TEXT_DARK, COL_WIN_FRAME);
                draw_text(w->x+24, w->y+70, "High Resolution : 640x480 True Color (32 BPP)", COL_START_BTN, COL_WIN_FRAME);
                draw_text(w->x+24, w->y+95, "Display Driver  : Bochs VBE Linear Framebuffer", COL_TEXT_DARK, COL_WIN_FRAME);
                draw_text(w->x+24, w->y+120,"Rendering Engine: Unlocked 60+ FPS Assembly REP MOVSD", COL_TEXT_DARK, COL_WIN_FRAME);
                draw_text(w->x+24, w->y+145,"Multitasking    : Preemptive Round-Robin SMP Cores", COL_TEXT_DARK, COL_WIN_FRAME);
                draw_text(w->x+24, w->y+180,"[ Double click desktop icons or START menu ]", COL_TITLE_INA, COL_WIN_FRAME);
            } else if (w->type == 1) { /* Terminal Window */
                draw_rect(w->x+4, w->y+24, w->w-8, w->h-28, COL_TERM_BG);
                for (int r = 0; r < TERM_ROWS; r++) {
                    char wb[TERM_COLS + 1];
                    int wp = 0;
                    uint32_t cc = term_color[r][0];
                    int st = 0;
                    for (int c = 0; c < TERM_COLS; c++) {
                        if (term_color[r][c] != cc) {
                            if (wp > 0) {
                                wb[wp] = 0;
                                draw_text(w->x+8 + st*8, w->y+28 + r*8, wb, cc, COL_TERM_BG);
                                wp = 0;
                            }
                            cc = term_color[r][c];
                            st = c;
                        }
                        wb[wp++] = term_buf[r][c];
                    }
                    if (wp > 0) {
                        wb[wp] = 0;
                        draw_text(w->x+8 + st*8, w->y+28 + r*8, wb, cc, COL_TERM_BG);
                    }
                }
                blink = (blink + 1) % 40;
                if (is_active && blink < 20) {
                    draw_rect(w->x + 8 + term_c*8, w->y + 34 + term_r*8, 8, 2, COL_TEXT_GREEN);
                }
            } else if (w->type == 2) { /* System Monitor */
                draw_text(w->x+20, w->y+45, "AzamiOS Hardware & Graphics Monitor", COL_TEXT_DARK, COL_WIN_FRAME);
                draw_text(w->x+20, w->y+70, "Graphics Card: Bochs VBE True Color Adapter", COL_START_BTN, COL_WIN_FRAME);
                draw_text(w->x+20, w->y+90, "Resolution   : 640 x 480 @ 32 BPP (16.7M Colors)", COL_TEXT_DARK, COL_WIN_FRAME);
                draw_text(w->x+20, w->y+110,"Memory Paging: 1.2 MB Linear Framebuffer Mapped", COL_TEXT_DARK, COL_WIN_FRAME);
                draw_text(w->x+20, w->y+130,"CPU Topology : 4-Core SMP Preemptive Time-sliced", COL_TEXT_DARK, COL_WIN_FRAME);
                char fb[32];
                itoa(frame_cnt, fb, 10);
                draw_text(w->x+20, w->y+160,"Frames Rendered: ", COL_TITLE_ACT, COL_WIN_FRAME);
                draw_text(w->x+150, w->y+160, fb, COL_TEXT_DARK, COL_WIN_FRAME);
            }
        }

        /* 4. Start Menu Overlay */
        if (start_menu) {
            draw_rect(4, 330, 140, 128, COL_TASKBAR);
            draw_rect(4, 330, 140, 2, COL_START_BTN);
            draw_rect(8, 336, 132, 20, COL_START_BTN);
            draw_text(16, 342, "AzamiOS v2.0", COL_TEXT_WHITE, COL_START_BTN);
            draw_text(16, 368, "> Terminal Shell", COL_TEXT_CYAN, COL_TASKBAR);
            draw_text(16, 394, "# System Monitor", COL_TEXT_GOLD, COL_TASKBAR);
            draw_text(16, 430, "X Shutdown OS", COL_TEXT_RED, COL_TASKBAR);
        }

        /* 5. Taskbar */
        draw_rect(0, 460, 640, 20, COL_TASKBAR);
        draw_rect(0, 460, 640, 2, COL_START_BTN);
        draw_rect(4, 463, 70, 15, start_menu ? COL_START_ACT : COL_START_BTN);
        draw_text(16, 467, "START", COL_TEXT_WHITE, start_menu ? COL_START_ACT : COL_START_BTN);

        char time_buf[16];
        format_time_str(time_buf, &t);
        draw_text(570, 467, time_buf, COL_TEXT_WHITE, COL_TASKBAR);

        gfx_flip();

        /* Unlocked high speed rendering yielding just enough for clean irq rotation */
        for (volatile int k = 0; k < 150; k++);
    }

    exit(0);
}
