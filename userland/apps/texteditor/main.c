/* ============================================================================
 * AzamiOS — Text Editor (v3.0)
 * File: userland/apps/texteditor/main.c
 *
 * Features:
 *  • Catppuccin Mocha themed code editor
 *  • Real-time C/Script Syntax Highlighting (Keywords, Strings, Comments, Numbers)
 *  • Line Number Gutter & Active Line Highlight
 *  • Interactive Search & Find in File (Ctrl+F, F3 next match)
 *  • Save buffer (Ctrl+S / F2) & Dynamic Status Bar
 *  • High-capacity text buffer (256 lines × 200 chars)
 * ============================================================================ */

#include <stdbool.h>
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
#define WIN_W       700
#define WIN_H       480
#define MAP_ADDR    ((void *)0x65000000)

#define ROWS         25     /* visible text rows */
#define COLS         80     /* visible text columns */
#define FONT_W        8     /* pixels per character */
#define FONT_H       16     /* pixels per line */
#define GUTTER_W     48     /* width of line number gutter */
#define TEXT_OX      (GUTTER_W + 8) /* left margin in px */
#define TEXT_OY      44     /* top margin in px */
#define MAX_LINES   256
#define MAX_LINELEN 200

static uk_window_t g_win;

/* ── Buffer ─────────────────────────────────────────────────────────────────── */
static char g_lines[MAX_LINES][MAX_LINELEN + 1];
static int  g_line_count  = 0;
static int  g_cursor_row  = 0;
static int  g_cursor_col  = 0;
static int  g_scroll      = 0;          /* top visible line */
static unsigned int g_tick = 0;         /* for cursor blink */
static int  g_dirty       = 0;          /* unsaved changes indicator */

/* ── Search / Find & File Prompt State ─────────────────────────────────────── */
static int  g_find_mode = 0;            /* 1 = Find bar active */
static char g_find_query[64] = "";
static int  g_find_len = 0;
static int  g_find_match_count = 0;

static int  g_open_prompt_mode = 0;     /* 1 = Open File Prompt active */
static char g_open_prompt_path[128] = "";
static int  g_open_prompt_len = 0;

/* ── Welcome content ───────────────────────────────────────────────────────── */
static const char *g_welcome[] = {
    "/* ===========================================================================",
    " * AzamiOS — Text Editor (v3.0)",
    " * Catppuccin Mocha C/Script Syntax Highlighter & Editor",
    " * =========================================================================== */",
    "#include <stdio.h>",
    "#include <stdlib.h>",
    "#include <unistd.h>",
    "",
    "int main(int argc, char **argv) {",
    "    // Print system greeting and diagnostics",
    "    printf(\"Hello, AzamiOS v7.0 Microkernel!\\n\");",
    "    for (int i = 0; i < 4; i++) {",
    "        printf(\"Core %d: Online and active.\\n\", i);",
    "    }",
    "    return 0;",
    "}",
    "",
    "/* Shortcuts:",
    " *   • Ctrl+S or F2 : Save current file",
    " *   • Ctrl+F       : Find in file",
    " *   • Arrows / Nav : Cursor movement & Scrolling",
    " *   • Enter / Bksp : Edit buffer",
    " */",
};

static char g_file_path[256] = "/hdd/notes.txt";

static int linelen(int row)
{
    if (row < 0 || row >= MAX_LINES) return 0;
    int j = 0;
    while (g_lines[row][j] && j < MAX_LINELEN) j++;
    return j;
}

static void save_buffer(void)
{
    int fd = sys_open(g_file_path, 0x0241, 0644);
    if (fd < 0 && strcmp(g_file_path, "/hdd/notes.txt") == 0) {
        fd = sys_open("/notes.txt", 0x0241, 0644);
    }
    if (fd >= 0) {
        lseek(fd, 0, 0);
        char buf[8192];
        memset(buf, 0, sizeof(buf));
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
    }
}

static void init_buffer(void)
{
    int fd = sys_open(g_file_path, 0, 0);
    if (fd < 0 && strcmp(g_file_path, "/hdd/notes.txt") == 0) {
        fd = sys_open("/notes.txt", 0, 0);
    }
    if (fd >= 0) {
        char buf[8192];
        memset(buf, 0, sizeof(buf));
        int n = sys_read(fd, buf, sizeof(buf) - 1);
        sys_close(fd);

        if (n > 0 && buf[0] != '\0') {
            int r = 0, c = 0;
            for (int i = 0; i < n && r < MAX_LINES; i++) {
                if (buf[i] == '\0') break;
                if (buf[i] == '\n') {
                    g_lines[r][c] = '\0';
                    r++;
                    c = 0;
                } else if (c < MAX_LINELEN) {
                    g_lines[r][c++] = buf[i];
                }
            }
            g_line_count = (r > 0 || c > 0) ? r + 1 : 0;
            if (g_line_count > 0 && c == 0) g_line_count--;
            return;
        }
    }

    int n = (int)(sizeof(g_welcome) / sizeof(g_welcome[0]));
    if (n > MAX_LINES) n = MAX_LINES;
    g_line_count = n;
    for (int i = 0; i < n; i++) {
        int j;
        for (j = 0; g_welcome[i][j] && j < MAX_LINELEN; j++)
            g_lines[i][j] = g_welcome[i][j];
        g_lines[i][j] = '\0';
    }
}

/* ── C Syntax Highlighting Colorizer ─────────────────────────────────────────── */
static const char *g_keywords[] = {
    "int", "void", "char", "unsigned", "long", "short", "float", "double",
    "struct", "union", "enum", "typedef", "static", "const", "extern", "volatile",
    "return", "if", "else", "while", "for", "do", "switch", "case", "default",
    "break", "continue", "goto", "sizeof", "include", "define", "ifdef", "ifndef",
    "endif", "true", "false", "NULL", "bool"
};
#define NUM_KEYWORDS ((int)(sizeof(g_keywords)/sizeof(g_keywords[0])))

static bool is_ident_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static void render_syntax_line(int line_idx, int py, int max_w)
{
    const char *s = g_lines[line_idx];
    int len = linelen(line_idx);
    int x = TEXT_OX;
    int i = 0;

    while (i < len && (x - TEXT_OX) < max_w) {
        /* Comment: // */
        if (s[i] == '/' && s[i+1] == '/') {
            while (i < len && (x - TEXT_OX) < max_w) {
                uk_draw_char(&g_win, x, py, s[i++], UK_OVERLAY1);
                x += FONT_W;
            }
            break;
        }

        /* Comment: slash-star */
        if (s[i] == '/' && s[i+1] == '*') {
            uk_draw_char(&g_win, x, py, s[i++], UK_OVERLAY1);
            x += FONT_W;
            uk_draw_char(&g_win, x, py, s[i++], UK_OVERLAY1);
            x += FONT_W;
            while (i < len && (x - TEXT_OX) < max_w) {
                uk_draw_char(&g_win, x, py, s[i], UK_OVERLAY1);
                x += FONT_W;
                if (s[i-1] == '*' && s[i] == '/') { i++; break; }
                i++;
            }
            continue;
        }

        /* String Literal: "..." or '...' */
        if (s[i] == '"' || s[i] == '\'') {
            char quote = s[i];
            uk_draw_char(&g_win, x, py, s[i++], UK_GREEN);
            x += FONT_W;
            while (i < len && (x - TEXT_OX) < max_w) {
                uk_draw_char(&g_win, x, py, s[i], UK_GREEN);
                x += FONT_W;
                if (s[i] == quote && s[i-1] != '\\') { i++; break; }
                i++;
            }
            continue;
        }

        /* Preprocessor directive: #... */
        if (s[i] == '#') {
            while (i < len && s[i] != ' ' && s[i] != '<' && s[i] != '"' && (x - TEXT_OX) < max_w) {
                uk_draw_char(&g_win, x, py, s[i++], UK_MAUVE);
                x += FONT_W;
            }
            continue;
        }

        /* Number literal */
        if (s[i] >= '0' && s[i] <= '9' && (i == 0 || !is_ident_char(s[i-1]))) {
            while (i < len && (is_ident_char(s[i]) || s[i] == '.') && (x - TEXT_OX) < max_w) {
                uk_draw_char(&g_win, x, py, s[i++], UK_PEACH);
                x += FONT_W;
            }
            continue;
        }

        /* Identifier / Keyword */
        if ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || s[i] == '_') {
            int start = i;
            while (i < len && is_ident_char(s[i])) i++;
            int klen = i - start;

            unsigned int col = UK_TEXT;
            for (int k = 0; k < NUM_KEYWORDS; k++) {
                if ((int)strlen(g_keywords[k]) == klen && strncmp(&s[start], g_keywords[k], klen) == 0) {
                    col = UK_MAUVE;
                    break;
                }
            }

            for (int j = start; j < i && (x - TEXT_OX) < max_w; j++) {
                uk_draw_char(&g_win, x, py, s[j], col);
                x += FONT_W;
            }
            continue;
        }

        /* Punctuation / Operator */
        unsigned int sym_col = UK_SKY;
        if (s[i] == '{' || s[i] == '}' || s[i] == '(' || s[i] == ')' || s[i] == '[' || s[i] == ']')
            sym_col = UK_LAVENDER;
        else if (s[i] == ';' || s[i] == ',' || s[i] == '.')
            sym_col = UK_SUBTEXT0;

        uk_draw_char(&g_win, x, py, s[i++], sym_col);
        x += FONT_W;
    }
}

static void draw_editor(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    /* Background */
    uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_BASE);

    /* ── Header Bar ──────────────────────────────────────────────────────── */
    uk_gradient_h(&g_win, 0, 0, (int)w, 40, UK_SURFACE0, UK_BASE);
    uk_fill_rect(&g_win, 0, 0, 4, 40, UK_BLUE);
    uk_draw_text(&g_win, 14, 6, "AzamiOS Code & Text Editor", UK_TEXT);

    char sub[128];
    snprintf(sub, sizeof(sub), "%s %s", g_file_path, g_dirty ? "[Modified]" : "[Saved]");
    uk_draw_text(&g_win, 14, 22, sub, g_dirty ? UK_YELLOW : UK_OVERLAY0);

    /* Pos info on right */
    char pos_str[48];
    snprintf(pos_str, sizeof(pos_str), "Ln %d, Col %d", g_cursor_row + 1, g_cursor_col + 1);
    uk_draw_text(&g_win, (int)w - (int)strlen(pos_str) * 8 - 14, 12, pos_str, UK_LAVENDER);
    uk_hline(&g_win, 0, 40, (int)w, UK_SURFACE1);

    /* ── Line Number Gutter ─────────────────────────────────────────────── */
    int text_h = (int)h - TEXT_OY - 24;
    uk_fill_rect(&g_win, 0, TEXT_OY, GUTTER_W, text_h, UK_MANTLE);
    uk_vline(&g_win, GUTTER_W, TEXT_OY, text_h, UK_SURFACE1);

    /* ── Text Lines ─────────────────────────────────────────────────────── */
    int max_visible = text_h / FONT_H;
    for (int r = 0; r < max_visible; r++) {
        int line = r + g_scroll;
        if (line >= g_line_count) break;

        int py = TEXT_OY + r * FONT_H;

        /* Line Number */
        char lnbuf[8];
        snprintf(lnbuf, sizeof(lnbuf), "%3d", line + 1);
        uk_draw_text(&g_win, 8, py, lnbuf, (line == g_cursor_row) ? UK_TEXT : UK_OVERLAY0);

        /* Active line highlight strip */
        if (line == g_cursor_row) {
            uk_fill_rect(&g_win, GUTTER_W + 1, py, (int)w - GUTTER_W - 1, FONT_H, UK_SURFACE0);
        }

        /* Syntax Highlighted Line */
        render_syntax_line(line, py, (int)w - TEXT_OX - 8);
    }

    /* ── Cursor ─────────────────────────────────────────────────────────── */
    if ((g_tick / 6) % 2 == 0) {
        int cur_r = g_cursor_row - g_scroll;
        if (cur_r >= 0 && cur_r < max_visible) {
            int cx = TEXT_OX + g_cursor_col * FONT_W;
            int cy = TEXT_OY + cur_r * FONT_H;
            uk_fill_rect(&g_win, cx, cy, 2, FONT_H, UK_MAUVE);
        }
    }

    /* ── Search / Find Bar Overlay ──────────────────────────────────────── */
    if (g_find_mode) {
        int fy = (int)h - 56;
        uk_fill_rect(&g_win, 0, fy, (int)w, 32, UK_SURFACE0);
        uk_hline(&g_win, 0, fy, (int)w, UK_MAUVE);
        uk_draw_text(&g_win, 12, fy + 8, "Find: ", UK_MAUVE);
        uk_draw_text(&g_win, 56, fy + 8, g_find_query, UK_TEXT);
        /* Blinking search cursor */
        int scx = 56 + g_find_len * 8;
        if ((g_tick / 6) % 2 == 0) {
            uk_fill_rect(&g_win, scx, fy + 8, 2, 14, UK_MAUVE);
        }
        char match_str[32];
        snprintf(match_str, sizeof(match_str), "[%d matches | Esc: Close]", g_find_match_count);
        uk_draw_text(&g_win, (int)w - (int)strlen(match_str) * 8 - 12, fy + 8, match_str, UK_OVERLAY0);
    }

    /* ── Open File Prompt Bar Overlay ─────────────────────────────────── */
    if (g_open_prompt_mode) {
        int oy = (int)h - 56;
        uk_fill_rect(&g_win, 0, oy, (int)w, 32, UK_SURFACE0);
        uk_hline(&g_win, 0, oy, (int)w, UK_BLUE);
        uk_draw_text(&g_win, 12, oy + 8, "Open File: ", UK_BLUE);
        uk_draw_text(&g_win, 100, oy + 8, g_open_prompt_path, UK_TEXT);
        int scx = 100 + g_open_prompt_len * 8;
        if ((g_tick / 6) % 2 == 0) {
            uk_fill_rect(&g_win, scx, oy + 8, 2, 14, UK_BLUE);
        }
        uk_draw_text(&g_win, (int)w - 180, oy + 8, "[Enter: Open | Esc: Cancel]", UK_OVERLAY0);
    }

    /* ── Bottom Status Bar ──────────────────────────────────────────────── */
    int sby = (int)h - 22;
    uk_fill_rect(&g_win, 0, sby, (int)w, 22, UK_CRUST);
    uk_hline(&g_win, 0, sby, (int)w, UK_SURFACE1);
    uk_draw_text(&g_win, 12, sby + 3, "^S:Save  ^F:Find  ^O:Open  ^N:New", UK_OVERLAY0);

    /* Word count & Byte telemetry */
    int total_words = 0;
    int total_chars = 0;
    for (int r = 0; r < g_line_count; r++) {
        int len = linelen(r);
        total_chars += len + 1;
        bool in_word = false;
        for (int c = 0; c < len; c++) {
            char ch = g_lines[r][c];
            if (ch != ' ' && ch != '\t') {
                if (!in_word) { total_words++; in_word = true; }
            } else {
                in_word = false;
            }
        }
    }

    char stat_r[64];
    snprintf(stat_r, sizeof(stat_r), "%d words | %d B | [C/C++] | %d lines", total_words, total_chars, g_line_count);
    uk_draw_text(&g_win, (int)w - (int)strlen(stat_r) * 8 - 12, sby + 3, stat_r, UK_SUBTEXT0);

    uk_invalidate(&g_win);
}

static void insert_char(char c)
{
    if (g_cursor_row >= MAX_LINES) return;
    int len = linelen(g_cursor_row);
    if (len >= MAX_LINELEN) return;

    for (int j = len; j >= g_cursor_col; j--)
        g_lines[g_cursor_row][j + 1] = g_lines[g_cursor_row][j];
    g_lines[g_cursor_row][g_cursor_col] = c;
    g_cursor_col++;
    g_dirty = 1;
}

static void backspace(void)
{
    if (g_cursor_col > 0) {
        int len = linelen(g_cursor_row);
        for (int j = g_cursor_col - 1; j < len; j++)
            g_lines[g_cursor_row][j] = g_lines[g_cursor_row][j + 1];
        g_cursor_col--;
        g_dirty = 1;
    } else if (g_cursor_row > 0) {
        int prev_len = linelen(g_cursor_row - 1);
        int cur_len  = linelen(g_cursor_row);
        if (prev_len + cur_len <= MAX_LINELEN) {
            for (int j = 0; j <= cur_len; j++)
                g_lines[g_cursor_row - 1][prev_len + j] = g_lines[g_cursor_row][j];
            for (int j = g_cursor_row; j < g_line_count - 1; j++) {
                for (int k = 0; k <= linelen(j + 1); k++)
                    g_lines[j][k] = g_lines[j + 1][k];
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

    /* Handle Search Bar Input when Find Mode is active */
    if (g_find_mode) {
        if (keycode == 27 || scancode == 1) { /* Escape */
            g_find_mode = 0;
            return;
        }
        if (keycode == '\b' || scancode == 14) {
            if (g_find_len > 0) {
                g_find_query[--g_find_len] = '\0';
            }
            return;
        }
        if (keycode == '\n' || keycode == '\r' || scancode == 28) {
            /* Jump to next match */
            if (g_find_len > 0) {
                for (int r = g_cursor_row; r < g_line_count; r++) {
                    char *m = strstr(g_lines[r], g_find_query);
                    if (m && (r > g_cursor_row || (int)(m - g_lines[r]) > g_cursor_col)) {
                        g_cursor_row = r;
                        g_cursor_col = (int)(m - g_lines[r]);
                        if (g_cursor_row >= g_scroll + ROWS) g_scroll = g_cursor_row - ROWS + 1;
                        if (g_cursor_row < g_scroll) g_scroll = g_cursor_row;
                        return;
                    }
                }
                /* Wrap around */
                for (int r = 0; r <= g_cursor_row; r++) {
                    char *m = strstr(g_lines[r], g_find_query);
                    if (m) {
                        g_cursor_row = r;
                        g_cursor_col = (int)(m - g_lines[r]);
                        if (g_cursor_row >= g_scroll + ROWS) g_scroll = g_cursor_row - ROWS + 1;
                        if (g_cursor_row < g_scroll) g_scroll = g_cursor_row;
                        return;
                    }
                }
            }
            return;
        }
        if (keycode >= 32 && keycode <= 126 && g_find_len < 60) {
            g_find_query[g_find_len++] = keycode;
            g_find_query[g_find_len] = '\0';
            /* Count matches */
            g_find_match_count = 0;
            for (int r = 0; r < g_line_count; r++) {
                if (strstr(g_lines[r], g_find_query)) g_find_match_count++;
            }
            return;
        }
    }

    /* Handle Open File Prompt Bar Input */
    if (g_open_prompt_mode) {
        if (keycode == 27 || scancode == 1) { /* Escape */
            g_open_prompt_mode = 0;
            return;
        }
        if (keycode == '\b' || scancode == 14) {
            if (g_open_prompt_len > 0) g_open_prompt_path[--g_open_prompt_len] = '\0';
            return;
        }
        if (keycode == '\n' || keycode == '\r' || scancode == 28) {
            if (g_open_prompt_len > 0) {
                strncpy(g_file_path, g_open_prompt_path, sizeof(g_file_path) - 1);
                g_file_path[sizeof(g_file_path) - 1] = '\0';
                init_buffer();
                g_cursor_row = 0;
                g_cursor_col = 0;
                g_scroll = 0;
                g_dirty = 0;
            }
            g_open_prompt_mode = 0;
            return;
        }
        if (keycode >= 32 && keycode <= 126 && g_open_prompt_len < 120) {
            g_open_prompt_path[g_open_prompt_len++] = keycode;
            g_open_prompt_path[g_open_prompt_len] = '\0';
            return;
        }
    }

    /* Ctrl+O: Open File Prompt */
    if ((modifiers & 2) && (keycode == 'o' || keycode == 'O' || keycode == 15)) {
        g_open_prompt_mode = !g_open_prompt_mode;
        g_open_prompt_len = (int)strlen(g_file_path);
        strncpy(g_open_prompt_path, g_file_path, sizeof(g_open_prompt_path) - 1);
        g_find_mode = 0;
        return;
    }

    /* Ctrl+N: New File */
    if ((modifiers & 2) && (keycode == 'n' || keycode == 'N' || keycode == 14)) {
        g_line_count = 1;
        g_lines[0][0] = '\0';
        g_cursor_row = 0;
        g_cursor_col = 0;
        g_scroll = 0;
        g_dirty = 0;
        strncpy(g_file_path, "/hdd/untitled.txt", sizeof(g_file_path) - 1);
        return;
    }

    /* Ctrl+F: Open Find Mode */
    if ((modifiers & 2) && (keycode == 'f' || keycode == 'F' || keycode == 6)) {
        g_find_mode = !g_find_mode;
        g_find_len = 0;
        g_find_query[0] = '\0';
        g_find_match_count = 0;
        g_open_prompt_mode = 0;
        return;
    }

    /* Save: Ctrl+S or F2 */
    if (scancode == 60 || ((modifiers & 2) && (keycode == 's' || keycode == 'S' || keycode == 19))) {
        save_buffer();
        return;
    }

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

    /* Backspace */
    if (keycode == '\b' || keycode == 127 || scancode == 14) {
        backspace();
        return;
    }

    /* Enter */
    if (keycode == '\n' || keycode == '\r' || scancode == 28) {
        if (g_line_count < MAX_LINES) {
            for (int j = g_line_count; j > g_cursor_row + 1; j--) {
                for (int k = 0; k <= linelen(j - 1); k++)
                    g_lines[j][k] = g_lines[j - 1][k];
            }
            int split = g_cursor_col;
            g_lines[g_cursor_row + 1][0] = '\0';
            for (int k = split; g_lines[g_cursor_row][k]; k++) {
                g_lines[g_cursor_row + 1][k - split] = g_lines[g_cursor_row][k];
                g_lines[g_cursor_row + 1][k - split + 1] = '\0';
            }
            g_lines[g_cursor_row][split] = '\0';
            g_line_count++;
            g_cursor_row++;
            g_cursor_col = 0;
            if (g_cursor_row >= g_scroll + ROWS) g_scroll++;
            g_dirty = 1;
        }
        return;
    }

    /* Tab: insert 4 spaces */
    if (keycode == '\t' || scancode == 15) {
        for (int t = 0; t < 4; t++) insert_char(' ');
        return;
    }

    /* Printable ASCII */
    if (keycode >= 32 && keycode <= 126) {
        insert_char((char)keycode);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv[1] && argv[1][0]) {
        strncpy(g_file_path, argv[1], sizeof(g_file_path) - 1);
        g_file_path[sizeof(g_file_path) - 1] = '\0';
    }

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0 && fb.width > 0 && fb.height > 0) {
        sw = fb.width;
        sh = fb.height;
    }

    int ret = uk_window_connect(&g_win, "Text Editor",
                                (int)(sw / 2) - WIN_W / 2,
                                (int)(sh / 2) - WIN_H / 2,
                                WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN);
    if (ret < 0) return -1;

    init_buffer();
    draw_editor();

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) break;

        if (msg.type == AZ_WM_KEY_EVENT) {
            handle_key(msg.key.keycode, msg.key.scancode, msg.key.pressed, msg.key.modifiers);
            draw_editor();
        } else if (msg.type == AZ_WM_MOUSE_EVENT) {
            if (msg.mouse.buttons & 1) {
                int mx = msg.mouse.abs_x;
                int my = msg.mouse.abs_y;
                if (mx >= TEXT_OX && my >= TEXT_OY && my < (int)g_win.height - 24) {
                    int clicked_r = (my - TEXT_OY) / FONT_H + g_scroll;
                    int clicked_c = (mx - TEXT_OX) / FONT_W;
                    if (clicked_r < g_line_count) {
                        g_cursor_row = clicked_r;
                        int max_c = linelen(g_cursor_row);
                        g_cursor_col = (clicked_c <= max_c) ? clicked_c : max_c;
                        draw_editor();
                    }
                }
            }
        }
    }

    return 0;
}
