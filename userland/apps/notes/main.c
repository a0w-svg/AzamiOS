/* ============================================================================
 * AzamiOS Desktop Environment — Pastel Sticky Notes (notes.elf v2.0)
 * File: userland/apps/notes/main.c
 *
 * Features:
 *  • Interactive Checklists: `- [ ]` and `[ ]` toggle to `[x]` on click/space
 *  • Live Note Templates: Todo Checklist, Meeting Notes, Quick Scratchpad
 *  • 6 Pastel themes with Catppuccin Mocha contrast
 *  • Font size toggle (Normal 16px / Compact 8px)
 *  • Real-time auto-saving to ~/.notes/
 *  • Multi-line text buffer with cursor blinking & smooth typing
 * ============================================================================ */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#include "../../libc/include/az/ipc.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN  1
#define WIN_W       340
#define WIN_H       340
#define MAP_ADDR    ((void *)0x6B000000)
#define HEADER_H     34
#define PADDING      14
#define MAX_TEXT   4096

typedef struct {
    const char  *name;
    unsigned int bg;
    unsigned int header_bg;
    unsigned int text_col;
    unsigned int dot_col;
} note_theme_t;

static const note_theme_t g_themes[] = {
    { "Yellow", 0xFFFFF9C4, 0xFFFFF176, 0xFF212121, 0xFFFBC02D },
    { "Peach",  0xFFFFE0B2, 0xFFFFCC80, 0xFF212121, 0xFFFB8C00 },
    { "Mauve",  0xFFE1BEE7, 0xFFCE93D8, 0xFF212121, 0xFF8E24AA },
    { "Teal",   0xFFB2DFDB, 0xFF80CBC4, 0xFF212121, 0xFF00897B },
    { "Mint",   0xFFC8E6C9, 0xFFA5D6A7, 0xFF212121, 0xFF43A047 },
    { "Rose",   0xFFF8BBD0, 0xFFF48FB1, 0xFF212121, 0xFFD81B60 },
};
#define NUM_THEMES (sizeof(g_themes) / sizeof(g_themes[0]))

static int  g_cur_theme = 0;
static char g_title[64] = "Sticky Note";
static char g_text[MAX_TEXT] =
    "- [x] Welcome to Sticky Notes v2.0!\n"
    "- [ ] Click any checkbox to toggle it\n"
    "- [ ] Switch pastel colors with dots above\n"
    "- [ ] Click [T] for Note Templates\n"
    "- [ ] Click [S] to toggle font size\n"
    "Auto-saved in ~/.notes/";

static int  g_cursor_pos = 0;
static int  g_cursor_blink = 1;
static char g_save_path[256] = "";
static bool g_small_font = false;
static bool g_show_templates = false;

static uk_window_t g_win;

/* ── Templates ────────────────────────────────────────────────────────────── */
typedef struct {
    const char *title;
    const char *content;
} note_template_t;

static const note_template_t g_note_templates[] = {
    { "Todo Checklist",
      "- [ ] High priority task\n"
      "- [ ] Review system logs\n"
      "- [ ] Test code changes\n"
      "- [ ] Deploy build\n" },
    { "Meeting Notes",
      "# Project Sync\n"
      "Attendees: \n"
      "Notes: \n"
      "- Action item 1\n"
      "- Action item 2\n" },
    { "Quick Memo",
      "Memo: \n\n"
      "Remember to check:\n"
      "1. Performance\n"
      "2. Memory leaks\n" },
    { "Code Scratch",
      "// Scratchpad\n"
      "int compute(int x) {\n"
      "    return x * 2 + 1;\n"
      "}\n" }
};
#define NUM_NOTE_TEMPLATES (sizeof(g_note_templates) / sizeof(g_note_templates[0]))

/* ── Save Note to Disk ────────────────────────────────────────────────────── */
static void save_note(void)
{
    if (!g_save_path[0]) return;
    mkdir("/home/azami/.notes", 0755);
    int fd = open(g_save_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, g_text, strlen(g_text));
        close(fd);
    }
}

/* ── Load Note from Disk ──────────────────────────────────────────────────── */
static void load_note(const char *path)
{
    int fd = open(path, O_RDONLY, 0);
    if (fd >= 0) {
        ssize_t n = read(fd, g_text, sizeof(g_text) - 1);
        close(fd);
        if (n >= 0) {
            g_text[n] = '\0';
            g_cursor_pos = (int)n;
        }
    }
}

/* ── Toggle Checklist Box at Click Coordinate ─────────────────────────────── */
static bool try_toggle_checklist(int click_x, int click_y)
{
    if (click_y < HEADER_H + PADDING) return false;

    int cur_x = PADDING;
    int cur_y = HEADER_H + PADDING;
    int font_w = 8;
    int line_h = g_small_font ? 12 : 18;

    int text_len = (int)strlen(g_text);
    int line_start = 0;

    for (int i = 0; i <= text_len; i++) {
        if (g_text[i] == '\n' || g_text[i] == '\0') {
            /* Check if this line is a checklist item */
            int cb_offset = -1;
            if (strncmp(g_text + line_start, "- [ ] ", 6) == 0 || strncmp(g_text + line_start, "- [x] ", 6) == 0) {
                cb_offset = line_start + 3;
            } else if (strncmp(g_text + line_start, "[ ] ", 4) == 0 || strncmp(g_text + line_start, "[x] ", 4) == 0) {
                cb_offset = line_start + 1;
            }

            if (cb_offset >= 0) {
                /* Checkbox is at cur_x .. cur_x + 16, cur_y .. cur_y + line_h */
                if (click_y >= cur_y - 2 && click_y <= cur_y + line_h &&
                    click_x >= PADDING - 2 && click_x <= PADDING + 28) {
                    if (g_text[cb_offset] == ' ') g_text[cb_offset] = 'x';
                    else g_text[cb_offset] = ' ';
                    save_note();
                    return true;
                }
            }

            cur_x = PADDING;
            cur_y += line_h;
            line_start = i + 1;
        } else {
            cur_x += font_w;
            if (cur_x > WIN_W - PADDING) {
                cur_x = PADDING;
                cur_y += line_h;
            }
        }
    }
    return false;
}

/* ── Render Note ──────────────────────────────────────────────────────────── */
static void render_note(uk_window_t *win)
{
    const note_theme_t *thm = &g_themes[g_cur_theme];

    /* 1. Note Body Background */
    uk_fill_rect(win, 0, 0, WIN_W, WIN_H, thm->bg);

    /* 2. Top Header Bar */
    uk_fill_rect(win, 0, 0, WIN_W, HEADER_H, thm->header_bg);
    uk_fill_rect(win, 0, HEADER_H - 1, WIN_W, 1, thm->dot_col);

    /* Header Buttons: [+] New, [T] Templates, [S] Size */
    uk_draw_button(win, 8, 5, 24, 24, "+", UK_BTN_NORMAL);
    uk_draw_button(win, 36, 5, 24, 24, "T", g_show_templates ? UK_BTN_PRESSED : UK_BTN_NORMAL);
    uk_draw_button(win, 64, 5, 24, 24, "S", g_small_font ? UK_BTN_PRESSED : UK_BTN_NORMAL);

    /* Title */
    uk_draw_text(win, 94, 9, g_title, thm->text_col);

    /* Color Swatches (Dots) on Right */
    int dot_x = WIN_W - (int)NUM_THEMES * 18 - 8;
    for (size_t i = 0; i < NUM_THEMES; i++) {
        int cx = dot_x + (int)i * 18 + 7;
        int cy = HEADER_H / 2;

        uk_fill_rect(win, cx - 6, cy - 6, 12, 12, g_themes[i].dot_col);
        if ((int)i == g_cur_theme) {
            uk_fill_rect(win, cx - 3, cy - 3, 6, 6, 0xFFFFFFFF);
        }
    }

    /* 3. Text Body with Checkbox Icons */
    int cur_x = PADDING;
    int cur_y = HEADER_H + PADDING;
    int line_h = g_small_font ? 12 : 18;
    int text_len = (int)strlen(g_text);

    int line_start = 0;
    for (int i = 0; i <= text_len; i++) {
        /* Draw blinking cursor */
        if (i == g_cursor_pos && g_cursor_blink) {
            uk_fill_rect(win, cur_x, cur_y - 1, 2, line_h - 2, thm->dot_col);
        }

        if (i == text_len) break;

        char c = g_text[i];
        if (c == '\n') {
            cur_x = PADDING;
            cur_y += line_h;
            line_start = i + 1;
            continue;
        }

        /* Check for checkbox start */
        if (i == line_start && (strncmp(g_text + i, "- [ ] ", 6) == 0 || strncmp(g_text + i, "- [x] ", 6) == 0)) {
            bool checked = (g_text[i + 3] == 'x');
            /* Draw visual checkbox */
            uk_fill_rect(win, cur_x, cur_y + 2, 11, 11, thm->header_bg);
            uk_fill_rect(win, cur_x, cur_y + 2, 11, 1, thm->dot_col);
            uk_fill_rect(win, cur_x, cur_y + 12, 11, 1, thm->dot_col);
            uk_fill_rect(win, cur_x, cur_y + 2, 1, 11, thm->dot_col);
            uk_fill_rect(win, cur_x + 10, cur_y + 2, 1, 11, thm->dot_col);

            if (checked) {
                uk_fill_rect(win, cur_x + 3, cur_y + 5, 5, 5, thm->dot_col);
            }
            cur_x += 16;
            i += 5;
            continue;
        }

        char str[2] = { c, '\0' };
        uk_draw_text(win, cur_x, cur_y, str, thm->text_col);
        cur_x += 8;

        if (cur_x > WIN_W - PADDING) {
            cur_x = PADDING;
            cur_y += line_h;
        }
    }

    /* 4. Templates Dropdown Menu */
    if (g_show_templates) {
        int tx = 20;
        int ty = HEADER_H + 4;
        int tw = 200;
        int th = (int)NUM_NOTE_TEMPLATES * 28 + 8;

        uk_fill_rect(win, tx, ty, tw, th, 0xFF181825);
        uk_fill_rect(win, tx, ty, tw, 1, 0xFFCBA6F7);
        uk_fill_rect(win, tx, ty + th - 1, tw, 1, 0xFFCBA6F7);
        uk_fill_rect(win, tx, ty, 1, th, 0xFFCBA6F7);
        uk_fill_rect(win, tx + tw - 1, ty, 1, th, 0xFFCBA6F7);

        for (size_t t = 0; t < NUM_NOTE_TEMPLATES; t++) {
            int iy = ty + 4 + (int)t * 28;
            uk_draw_text(win, tx + 10, iy + 6, g_note_templates[t].title, 0xFFCDD6F4);
            uk_fill_rect(win, tx + 6, iy + 24, tw - 12, 1, 0xFF313244);
        }
    }
}

/* ── Main ─────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    if (argc >= 2) {
        strncpy(g_save_path, argv[1], sizeof(g_save_path) - 1);
        load_note(g_save_path);
    } else {
        snprintf(g_save_path, sizeof(g_save_path), "/home/azami/.notes/note_%u.note", (unsigned int)getpid());
    }

    if (uk_window_connect(&g_win, "Notes", 240, 180, WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN) < 0) {
        fprintf(stderr, "Failed to create Notes window\n");
        return 1;
    }

    az_set_timer(g_win.client_chan, 500, 0);

    render_note(&g_win);
    uk_invalidate(&g_win);

    bool running = true;
    while (running) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) {
            save_note();
            break;
        }

        if (msg.type == AZ_WM_TIMER_TICK) {
            g_cursor_blink = !g_cursor_blink;
            render_note(&g_win);
            uk_invalidate(&g_win);
            continue;
        }

        if (msg.type == AZ_WM_MOUSE_EVENT) {
            int mx = msg.mouse.abs_x;
            int my = msg.mouse.abs_y;
            unsigned int btns = msg.mouse.buttons;

            if (btns & 1) {
                /* Check templates popup click */
                if (g_show_templates) {
                    int tx = 20;
                    int ty = HEADER_H + 4;
                    int tw = 200;
                    int th = (int)NUM_NOTE_TEMPLATES * 28 + 8;
                    if (mx >= tx && mx <= tx + tw && my >= ty && my <= ty + th) {
                        int sel = (my - (ty + 4)) / 28;
                        if (sel >= 0 && sel < (int)NUM_NOTE_TEMPLATES) {
                            strncpy(g_text, g_note_templates[sel].content, sizeof(g_text) - 1);
                            g_cursor_pos = (int)strlen(g_text);
                            save_note();
                        }
                    }
                    g_show_templates = false;
                    render_note(&g_win);
                    uk_invalidate(&g_win);
                    continue;
                }

                if (my <= HEADER_H) {
                    /* [+] New Note */
                    if (mx >= 8 && mx <= 32) {
                        char new_path[256];
                        snprintf(new_path, sizeof(new_path), "/home/azami/.notes/note_%u.note", (unsigned int)getpid() + 1);
                        char *const nav[] = { "/bin/notes.elf", new_path, NULL };
                        if (fork() == 0) {
                            execve("/bin/notes.elf", nav, NULL);
                            exit(0);
                        }
                        continue;
                    }
                    /* [T] Templates */
                    if (mx >= 36 && mx <= 60) {
                        g_show_templates = !g_show_templates;
                        render_note(&g_win);
                        uk_invalidate(&g_win);
                        continue;
                    }
                    /* [S] Size */
                    if (mx >= 64 && mx <= 88) {
                        g_small_font = !g_small_font;
                        render_note(&g_win);
                        uk_invalidate(&g_win);
                        continue;
                    }

                    /* Theme dots */
                    int dot_x = WIN_W - (int)NUM_THEMES * 18 - 8;
                    if (mx >= dot_x) {
                        int theme_idx = (mx - dot_x) / 18;
                        if (theme_idx >= 0 && theme_idx < (int)NUM_THEMES) {
                            g_cur_theme = theme_idx;
                            render_note(&g_win);
                            uk_invalidate(&g_win);
                        }
                    }
                } else {
                    /* Click in note body: try toggling checklist checkbox */
                    if (try_toggle_checklist(mx, my)) {
                        render_note(&g_win);
                        uk_invalidate(&g_win);
                    }
                }
            }
        } else if (msg.type == AZ_WM_KEY_EVENT) {
            if (msg.key.pressed) {
                int k = msg.key.keycode;
                int len = (int)strlen(g_text);

                if (k == 0x1B) { /* ESC */
                    save_note();
                    break;
                } else if (k == 0x08 || k == 0x0E) { /* Backspace */
                    if (g_cursor_pos > 0) {
                        memmove(g_text + g_cursor_pos - 1, g_text + g_cursor_pos, len - g_cursor_pos + 1);
                        g_cursor_pos--;
                        save_note();
                    }
                } else if (k == 0x0A || k == 0x0D || k == 0x1C) { /* Enter */
                    if (len < MAX_TEXT - 2) {
                        memmove(g_text + g_cursor_pos + 1, g_text + g_cursor_pos, len - g_cursor_pos + 1);
                        g_text[g_cursor_pos] = '\n';
                        g_cursor_pos++;
                        save_note();
                    }
                } else if (k == 0x4B || k == 0x5002) { /* Left */
                    if (g_cursor_pos > 0) g_cursor_pos--;
                } else if (k == 0x4D || k == 0x5003) { /* Right */
                    if (g_cursor_pos < len) g_cursor_pos++;
                } else if (k >= 0x20 && k <= 0x7E) {
                    if (len < MAX_TEXT - 2) {
                        memmove(g_text + g_cursor_pos + 1, g_text + g_cursor_pos, len - g_cursor_pos + 1);
                        g_text[g_cursor_pos] = (char)k;
                        g_cursor_pos++;
                        save_note();
                    }
                }

                g_cursor_blink = 1;
                render_note(&g_win);
                uk_invalidate(&g_win);
            }
        }
    }

    uk_window_destroy(&g_win);
    return 0;
}
