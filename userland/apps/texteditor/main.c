/* ============================================================================
 * AzamiOS — Text Editor
 * File: userland/apps/texteditor/main.c
 * ============================================================================ */
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"
#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/unistd.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN  1
#define WIN_W       640
#define WIN_H       420
#define MAP_ADDR    ((void *)0x65000000)

#define ROWS         22     /* visible text rows                  */
#define COLS         74     /* visible text columns               */
#define FONT_W        8     /* pixels per character               */
#define FONT_H       16     /* pixels per line                    */
#define TEXT_OX      12     /* left margin in px                  */
#define TEXT_OY      52     /* top margin (below toolbar) in px   */
#define MAX_LINES    64
#define MAX_LINELEN 160

static uk_window_t g_win;

/* ── Buffer ─────────────────────────────────────────────────────────────────── */
static char g_lines[MAX_LINES][MAX_LINELEN + 1];
static int  g_line_count  = 0;
static int  g_cursor_row  = 0;
static int  g_cursor_col  = 0;
static int  g_scroll      = 0;          /* top visible line              */
static unsigned int g_tick = 0;         /* for cursor blink              */
static int  g_dirty       = 0;          /* unsaved changes indicator     */

/* ── Pre-loaded welcome content ─────────────────────────────────────────────── */
static const char *g_welcome[] = {
    "  Welcome to AzamiOS Text Editor",
    "  ─────────────────────────────────────────────",
    "",
    "  AzamiOS v7.0 — x86_64 Microkernel OS",
    "  Written in C with no external dependencies.",
    "",
    "  This text editor supports:",
    "    • Arrow-key navigation (via key events)",
    "    • Backspace / Delete",
    "    • Character insertion",
    "    • Vertical scrolling",
    "",
    "  Architecture highlights:",
    "    • Monolithic display server (azwm)",
    "    • IPC-based window protocol",
    "    • Shared-memory pixel buffers",
    "    • Catppuccin Mocha design system",
    "",
    "  Start typing to edit this file.",
    "  Close this window via the [X] button.",
    "",
};

static char g_file_path[256] = "/hdd/notes.txt";

static int linelen(int row);

static void save_buffer(void)
{
    /* 0x0241 = O_CREAT (0x40) | O_WRONLY (0x01) | O_TRUNC (0x200) */
    int fd = sys_open(g_file_path, 0x0241, 0644);
    if (fd < 0 && strcmp(g_file_path, "/hdd/notes.txt") == 0) {
        fd = sys_open("/notes.txt", 0x0241, 0644);
    }
    if (fd >= 0) {
        lseek(fd, 0, 0);
        char buf[4096];
        __builtin_memset(buf, 0, sizeof(buf));
        int offset = 0;
        for (int i = 0; i < g_line_count; i++) {
            int len = linelen(i);
            if (offset + len + 1 < (int)sizeof(buf)) {
                for (int j = 0; j < len; j++) buf[offset++] = g_lines[i][j];
                buf[offset++] = '\n';
            }
        }
        sys_write(fd, buf, offset > 0 ? offset : 1);
        sys_close(fd);
        g_dirty = 0;
        uk_draw_text(&g_win, 16, 24, "Saved!                 ", UK_GREEN);
    } else {
        uk_draw_text(&g_win, 16, 24, "Save error (open failed)", UK_RED);
    }
}

static void init_buffer(void)
{
    int fd = sys_open(g_file_path, 0, 0);
    if (fd < 0 && strcmp(g_file_path, "/hdd/notes.txt") == 0) {
        fd = sys_open("/notes.txt", 0, 0);
    }
    if (fd >= 0) {
        char buf[4096];
        __builtin_memset(buf, 0, 4096);
        int n = sys_read(fd, buf, 4096);
        sys_close(fd);
        
        if (n > 0 && buf[0] != '\0') {
            int r = 0, c = 0;
            for (int i = 0; i < n && r < MAX_LINES; i++) {
                if (buf[i] == '\0') break; /* End of padded file */
                if (buf[i] == '\n') {
                    g_lines[r][c] = '\0';
                    r++;
                    c = 0;
                } else if (c < MAX_LINELEN) {
                    g_lines[r][c++] = buf[i];
                }
            }
            g_line_count = (r > 0 || c > 0) ? r + 1 : 0;
            if (g_line_count > 0 && c == 0) g_line_count--; /* Strip trailing empty line */
            return;
        }
    }

    /* Fallback if file doesn't exist or is empty */
    int n = (int)(sizeof(g_welcome) / sizeof(g_welcome[0]));
    if (n > MAX_LINES) n = MAX_LINES;
    g_line_count = n;
    int i;
    for (i = 0; i < n; i++) {
        int j;
        for (j = 0; g_welcome[i][j] && j < MAX_LINELEN; j++)
            g_lines[i][j] = g_welcome[i][j];
        g_lines[i][j] = '\0';
    }
}

static int linelen(int row)
{
    int j = 0;
    while (g_lines[row][j]) j++;
    return j;
}

static void draw_editor(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    /* Background */
    uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_BASE);

    /* ── Toolbar ────────────────────────────────────────────────────────── */
    uk_gradient_h(&g_win, 0, 0, (int)w, 44, UK_SURFACE0, UK_BASE);
    uk_fill_rect(&g_win, 0, 0, 4, 44, UK_BLUE);
    uk_draw_text(&g_win, 16, 6, "Text Editor", UK_TEXT);
    uk_draw_text(&g_win, 16, 24,
                 g_dirty ? "Modified — unsaved" : "AzamiOS Editor v1.0",
                 g_dirty ? UK_YELLOW : UK_OVERLAY0);

    /* File info on right */
    char lstr[16];
    const char *lpfx = "Ln ";
    int li = 0; int j;
    for (j=0;lpfx[j];j++) lstr[li++]=lpfx[j];
    char lnum[6];
    unsigned int ln = (unsigned int)(g_cursor_row + 1);
    int ld = 0; unsigned int lt = ln;
    while(lt){ld++;lt/=10;}
    lt = ln;
    for(int k=ld-1;k>=0;k--){lnum[k]='0'+(char)(lt%10);lt/=10;}
    lnum[ld]='\0';
    for(j=0;lnum[j];j++) lstr[li++]=lnum[j];
    lstr[li++]=' '; lstr[li++]='C'; lstr[li++]='o'; lstr[li++]='l';
    lstr[li++]=' ';
    unsigned int cn = (unsigned int)(g_cursor_col + 1);
    ld=0; lt=cn;
    while(lt){ld++;lt/=10;}
    lt=cn;
    for(int k=ld-1;k>=0;k--){lnum[k]='0'+(char)(lt%10);lt/=10;}
    lnum[ld]='\0';
    for(j=0;lnum[j];j++) lstr[li++]=lnum[j];
    lstr[li]='\0';
    uk_draw_text(&g_win, (int)w - li * 8 - 8, 14, lstr, UK_OVERLAY0);

    uk_hline(&g_win, 0, 44, (int)w, UK_SURFACE1);

    /* ── Line number gutter ─────────────────────────────────────────────── */
    uk_fill_rect(&g_win, 0, TEXT_OY, 44, (int)h - TEXT_OY, UK_MANTLE);
    uk_vline(&g_win, 44, TEXT_OY, (int)h - TEXT_OY, UK_SURFACE1);

    /* ── Text area ──────────────────────────────────────────────────────── */
    int r;
    for (r = 0; r < ROWS; r++) {
        int line = r + g_scroll;
        if (line >= g_line_count) break;

        int py = TEXT_OY + r * FONT_H;

        /* Line number */
        char lnbuf[5];
        unsigned int lv = (unsigned int)(line + 1);
        int ld2 = 0; unsigned int lt2 = lv;
        while(lt2){ld2++;lt2/=10;}
        lt2=lv;
        for(int k=ld2-1;k>=0;k--){lnbuf[k]='0'+(char)(lt2%10);lt2/=10;}
        lnbuf[ld2]='\0';
        uk_draw_text(&g_win, 4 + (3 - ld2) * 8, py, lnbuf, UK_OVERLAY0);

        /* Highlight current line */
        if (line == g_cursor_row)
            uk_fill_rect(&g_win, 45, py, (int)w - 45, FONT_H, UK_SURFACE0);

        /* Line text */
        uk_draw_text_clip(&g_win, TEXT_OX + 44, py,
                          g_lines[line], UK_TEXT, (int)w - TEXT_OX - 44 - 4);
    }

    /* ── Cursor (blinking block) ────────────────────────────────────────── */
    if ((g_tick / 8) % 2 == 0) {
        int cur_r = g_cursor_row - g_scroll;
        if (cur_r >= 0 && cur_r < ROWS) {
            int cx = TEXT_OX + 44 + g_cursor_col * FONT_W;
            int cy = TEXT_OY + cur_r * FONT_H;
            uk_fill_rect(&g_win, cx, cy, 2, FONT_H, UK_MAUVE);
        }
    }

    /* ── Status bar ─────────────────────────────────────────────────────── */
    uk_fill_rect(&g_win, 0, (int)h - 20, (int)w, 20, UK_SURFACE0);
    uk_hline(&g_win, 0, (int)h - 20, (int)w, UK_SURFACE1);
    uk_draw_text(&g_win, 8, (int)h - 14,
                 "Arrows: nav  |  Bksp: del  |  F2: save  |  Type to insert",
                 UK_OVERLAY0);

    uk_invalidate(&g_win);
}

static void insert_char(char c)
{
    if (g_cursor_row >= MAX_LINES) return;
    int len = linelen(g_cursor_row);
    if (len >= MAX_LINELEN) return;
    /* Shift right */
    int j;
    for (j = len; j >= g_cursor_col; j--)
        g_lines[g_cursor_row][j + 1] = g_lines[g_cursor_row][j];
    g_lines[g_cursor_row][g_cursor_col] = c;
    g_cursor_col++;
    g_dirty = 1;
}

static void backspace(void)
{
    if (g_cursor_col > 0) {
        int len = linelen(g_cursor_row);
        int j;
        for (j = g_cursor_col - 1; j < len; j++)
            g_lines[g_cursor_row][j] = g_lines[g_cursor_row][j + 1];
        g_cursor_col--;
        g_dirty = 1;
    } else if (g_cursor_row > 0) {
        /* Join with previous line */
        int prev_len = linelen(g_cursor_row - 1);
        int cur_len  = linelen(g_cursor_row);
        if (prev_len + cur_len <= MAX_LINELEN) {
            int j;
            for (j = 0; j <= cur_len; j++)
                g_lines[g_cursor_row - 1][prev_len + j] = g_lines[g_cursor_row][j];
            /* Shift lines up */
            for (j = g_cursor_row; j < g_line_count - 1; j++) {
                int k;
                for (k = 0; k <= linelen(j + 1); k++)
                    g_lines[j][k] = g_lines[j+1][k];
            }
            g_line_count--;
            g_cursor_row--;
            g_cursor_col = prev_len;
            if (g_cursor_row < g_scroll) g_scroll = g_cursor_row;
        }
        g_dirty = 1;
    }
}

static void handle_key(unsigned char keycode, unsigned char scancode, unsigned char pressed, unsigned short modifiers)
{
    if (!pressed) return;
    int shift = (modifiers & 1) != 0;

    /* Up Arrow */
    if (scancode == 72 || keycode == 140) {
        if (g_cursor_row > 0) {
            g_cursor_row--;
            if (g_cursor_col > linelen(g_cursor_row)) g_cursor_col = linelen(g_cursor_row);
            if (g_cursor_row < g_scroll) g_scroll--;
        }
        return;
    }

    /* Down Arrow */
    if (scancode == 80 || keycode == 141) {
        if (g_cursor_row < g_line_count - 1) {
            g_cursor_row++;
            if (g_cursor_col > linelen(g_cursor_row)) g_cursor_col = linelen(g_cursor_row);
            if (g_cursor_row >= g_scroll + ROWS) g_scroll++;
        }
        return;
    }

    /* Left Arrow */
    if (scancode == 75 || keycode == 142) {
        if (g_cursor_col > 0) g_cursor_col--;
        else if (g_cursor_row > 0) { g_cursor_row--; g_cursor_col = linelen(g_cursor_row); }
        return;
    }

    /* Right Arrow */
    if (scancode == 77 || keycode == 143) {
        if (g_cursor_col < linelen(g_cursor_row)) g_cursor_col++;
        else if (g_cursor_row < g_line_count - 1) { g_cursor_row++; g_cursor_col = 0; }
        return;
    }

    /* Save: F2 or Ctrl+S */
    if (scancode == 60 || ((modifiers & 2) && (keycode == 's' || keycode == 'S' || keycode == 19))) {
        save_buffer();
        return;
    }

    /* Backspace */
    if (keycode == '\b' || keycode == 127 || scancode == 14) {
        backspace();
        return;
    }

    /* Enter: insert new line */
    if (keycode == '\n' || keycode == '\r' || scancode == 28) {
        if (g_line_count < MAX_LINES) {
            int j;
            for (j = g_line_count; j > g_cursor_row + 1; j--) {
                int k;
                for (k = 0; k <= linelen(j-1); k++) g_lines[j][k] = g_lines[j-1][k];
            }
            /* Split current line */
            int split = g_cursor_col;
            g_lines[g_cursor_row + 1][0] = '\0';
            int k;
            for (k = split; g_lines[g_cursor_row][k]; k++)
                g_lines[g_cursor_row + 1][k - split] = g_lines[g_cursor_row][k];
            g_lines[g_cursor_row + 1][k - split] = '\0';
            g_lines[g_cursor_row][split] = '\0';
            g_line_count++;
            g_cursor_row++;
            g_cursor_col = 0;
            if (g_cursor_row >= g_scroll + ROWS) g_scroll++;
        }
        g_dirty = 1;
        return;
    }

    /* Direct ASCII printable */
    if (keycode >= 32 && keycode <= 126) {
        insert_char((char)keycode);
        return;
    }

    /* Fallback scancode translation */
    char c = 0;
    if (scancode >= 2 && scancode <= 13) {
        const char r1_off[] = "1234567890-=";
        const char r1_on[]  = "!@#$%^&*()_+";
        c = shift ? r1_on[scancode - 2] : r1_off[scancode - 2];
    } else if (scancode >= 16 && scancode <= 27) {
        const char r2_off[] = "qwertyuiop[]";
        const char r2_on[]  = "QWERTYUIOP{}";
        c = shift ? r2_on[scancode - 16] : r2_off[scancode - 16];
    } else if (scancode >= 30 && scancode <= 40) {
        const char r3_off[] = "asdfghjkl;'";
        const char r3_on[]  = "ASDFGHJKL:\"";
        c = shift ? r3_on[scancode - 30] : r3_off[scancode - 30];
    } else if (scancode >= 44 && scancode <= 53) {
        const char r4_off[] = "zxcvbnm,./";
        const char r4_on[]  = "ZXCVBNM<>?";
        c = shift ? r4_on[scancode - 44] : r4_off[scancode - 44];
    } else if (scancode == 41) {
        c = shift ? '~' : '`';
    } else if (scancode == 43) {
        c = shift ? '|' : '\\';
    } else if (scancode == 57) {
        c = ' ';
    }
    if (c) insert_char(c);
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        strncpy(g_file_path, argv[1], sizeof(g_file_path) - 1);
        g_file_path[sizeof(g_file_path) - 1] = '\0';
    }

    de_log("[texteditor] Starting...");

    init_buffer();

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0) { sw = fb.width; sh = fb.height; }

    int ret = uk_window_connect(&g_win, "Text Editor",
                                (int)(sw / 2) - WIN_W / 2,
                                (int)(sh / 2) - WIN_H / 2,
                                WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN);
    if (ret < 0) { de_log("[texteditor] FATAL"); return -1; }

    draw_editor();

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) {
            break;
        }

        g_tick++;

        if (msg.type == AZ_WM_KEY_EVENT) {
            handle_key(msg.key.keycode, msg.key.scancode, msg.key.pressed, msg.key.modifiers);
            draw_editor();
        } else if (msg.type == AZ_WM_MOUSE_EVENT) {
            /* Click to position cursor */
            if (msg.mouse.buttons & AZ_MOUSE_BTN_LEFT) {
                int mx = (int)msg.mouse.abs_x;
                int my = (int)msg.mouse.abs_y;
                if (mx > TEXT_OX + 44 && my > TEXT_OY) {
                    int new_row = (my - TEXT_OY) / FONT_H + g_scroll;
                    int new_col = (mx - TEXT_OX - 44) / FONT_W;
                    if (new_row >= 0 && new_row < g_line_count) {
                        g_cursor_row = new_row;
                        int ll = linelen(new_row);
                        g_cursor_col = (new_col > ll) ? ll : new_col;
                        draw_editor();
                    }
                }
            }
        } else if (msg.type == AZ_WM_TIMER_TICK) {
            /* Redraw for cursor blink on timer tick */
            draw_editor();
        }
    }
    sys_exit(0);
}
