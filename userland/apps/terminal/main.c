/* ============================================================================
 * AzamiOS — Terminal Emulator (v3.0)
 * File: userland/apps/terminal/main.c
 *
 * Features
 * ────────
 *  • Full stdout & stderr capture from shell and external commands via UNIX pipes
 *  • ANSI SGR color code parser mapping ANSI colors to Catppuccin Mocha palette
 *  • Built-in command dispatcher (help, cd, pwd, clear, history, launch, exit)
 *  • Up / Down arrow key command history navigation
 *  • Tab auto-completion for binaries in /bin and /sbin
 *  • Page Up / Page Down / Mouse scrollback history navigation
 *  • Dynamic prompt reflecting user, host, and current working directory
 * ============================================================================ */

#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/string.h"
#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/dirent.h"
#include <stdbool.h>
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN   1
#define WIN_W        720
#define WIN_H        460
#define MAP_ADDR     ((void *)0x67000000)

#define TERM_COLS    86
#define TERM_ROWS    24
#define FONT_W        8
#define FONT_H       16
#define TERM_OX      10
#define TERM_OY      44

#define MAX_CMD      128
#define MAX_HISTORY   64
#define MAX_OUTPUT   500

static uk_window_t g_win;

/* ── Terminal buffer ─────────────────────────────────────────────────────────── */
static char g_lines[MAX_OUTPUT][TERM_COLS + 1];
static unsigned int g_line_colors[MAX_OUTPUT];
static int  g_line_count = 0;
static int  g_scroll     = 0;

static char g_input[MAX_CMD + 1];
static int  g_input_len  = 0;
static char g_cwd[128]   = "/";

static void draw_terminal(void);

/* History */
static char g_history[MAX_HISTORY][MAX_CMD + 1];
static int  g_hist_count = 0;
static int  g_hist_idx   = 0;

static unsigned int g_tick = 0;

static void update_cwd(void)
{
    if (sys_getcwd(g_cwd, sizeof(g_cwd)) <= 0) {
        strncpy(g_cwd, "/", sizeof(g_cwd) - 1);
    }
}

/* ── Output a line to terminal buffer ─────────────────────────────────────────── */
static void term_print(const char *s, unsigned int col)
{
    if (g_line_count >= MAX_OUTPUT) {
        memmove(&g_lines[0], &g_lines[1], sizeof(g_lines[0]) * (MAX_OUTPUT - 1));
        memmove(&g_line_colors[0], &g_line_colors[1], sizeof(unsigned int) * (MAX_OUTPUT - 1));
        g_line_count = MAX_OUTPUT - 1;
    }
    int n = g_line_count++;
    int i;
    for (i = 0; s[i] && i < TERM_COLS; i++) g_lines[n][i] = s[i];
    g_lines[n][i] = '\0';
    g_line_colors[n] = col ? col : UK_TEXT;

    /* Auto-scroll to bottom */
    if (g_line_count > (TERM_ROWS - 2)) {
        g_scroll = g_line_count - (TERM_ROWS - 2);
    } else {
        g_scroll = 0;
    }
}

/* ── ANSI SGR Color Decoder ─────────────────────────────────────────────────── */
static unsigned int parse_ansi_color(const char *code, unsigned int default_col)
{
    if (!code || !*code) return default_col;
    if (strstr(code, "31") || strstr(code, "91")) return UK_RED;
    if (strstr(code, "32") || strstr(code, "92")) return UK_GREEN;
    if (strstr(code, "33") || strstr(code, "93")) return UK_YELLOW;
    if (strstr(code, "34") || strstr(code, "94")) return UK_BLUE;
    if (strstr(code, "35") || strstr(code, "95")) return UK_MAUVE;
    if (strstr(code, "36") || strstr(code, "96")) return UK_TEAL;
    if (strstr(code, "37") || strstr(code, "97")) return UK_TEXT;
    if (strstr(code, "90")) return UK_OVERLAY0;
    if (strstr(code, "0")) return UK_TEXT;
    return default_col;
}

/* Print raw output stream with ANSI escape sequence filtering & color tracking */
static void term_print_stream_chunk(const char *buf, size_t len)
{
    static char line[TERM_COLS + 1];
    static int  li = 0;
    static unsigned int cur_col = UK_TEXT;

    for (size_t k = 0; k < len; k++) {
        char c = buf[k];

        if (c == '\033' && k + 1 < len && buf[k + 1] == '[') {
            /* Parse ANSI CSI sequence \033[...m */
            char seq[32];
            int si = 0;
            k += 2;
            while (k < len && buf[k] != 'm' && buf[k] != 'H' && buf[k] != 'J' && buf[k] != 'K' && si < 31) {
                seq[si++] = buf[k++];
            }
            seq[si] = '\0';
            if (k < len && buf[k] == 'm') {
                cur_col = parse_ansi_color(seq, UK_TEXT);
            }
            continue;
        }

        if (c == '\n' || li >= TERM_COLS) {
            line[li] = '\0';
            term_print(line, cur_col);
            li = 0;
        } else if (c != '\r' && (unsigned char)c >= 0x20) {
            line[li++] = c;
        }
    }
    if (li > 0) {
        line[li] = '\0';
        term_print(line, cur_col);
        li = 0;
    }
}

/* ── History Management ──────────────────────────────────────────────────────── */
static void add_history(const char *cmd)
{
    if (!cmd || !*cmd) return;
    if (g_hist_count > 0 && strcmp(g_history[g_hist_count - 1], cmd) == 0) return;

    if (g_hist_count < MAX_HISTORY) {
        strncpy(g_history[g_hist_count++], cmd, MAX_CMD);
    } else {
        for (int i = 1; i < MAX_HISTORY; i++) {
            strcpy(g_history[i - 1], g_history[i]);
        }
        strncpy(g_history[MAX_HISTORY - 1], cmd, MAX_CMD);
    }
    g_hist_idx = g_hist_count;
}

#include "../../libc/include/dirent.h"

/* ── Tab Auto-Completion ─────────────────────────────────────────────────────── */
static void do_tab_completion(void)
{
    if (g_input_len == 0) return;

    char prefix[MAX_CMD];
    strncpy(prefix, g_input, sizeof(prefix) - 1);
    prefix[sizeof(prefix) - 1] = '\0';
    int plen = strlen(prefix);

    DIR *dir = opendir("/bin");
    if (!dir) return;

    int match_count = 0;
    char match[64];
    match[0] = '\0';

    struct dirent *d;
    while ((d = readdir(dir)) != NULL) {
        char clean_name[64];
        strncpy(clean_name, d->d_name, sizeof(clean_name) - 1);
        clean_name[sizeof(clean_name) - 1] = '\0';
        char *dot = strstr(clean_name, ".elf");
        if (dot) *dot = '\0';

        if (strncmp(clean_name, prefix, plen) == 0) {
            match_count++;
            strncpy(match, clean_name, sizeof(match) - 1);
            match[sizeof(match) - 1] = '\0';
        }
    }
    closedir(dir);

    if (match_count == 1 && match[0] != '\0') {
        strncpy(g_input, match, MAX_CMD);
        g_input_len = strlen(g_input);
    }
}

/* ── Built-in Help Display ───────────────────────────────────────────────────── */
static void show_help(void)
{
    term_print("AzamiOS Terminal — Available System Commands & Utilities:", UK_MAUVE);
    term_print("  System & Info : help, uname, fetch, date, uptime, df, free, ps, top", UK_TEXT);
    term_print("  File & Dir    : ls, cat, head, tail, grep, wc, touch, mkdir, rm, cp, mv", UK_TEXT);
    term_print("  Navigation    : cd <path>, pwd, tree", UK_TEXT);
    term_print("  Text & Utils  : clear, history, echo, hexdump, base64, md5sum, cut, sort, uniq", UK_TEXT);
    term_print("  Network       : ping, ifconfig, netstat", UK_TEXT);
    term_print("  Hardware      : lspci, dmesg", UK_TEXT);
    term_print("  Desktop Apps  : launch <app> (e.g. calculator, filemanager, settings, paint)", UK_SAPPHIRE);
    term_print("  Session       : exit, reboot, poweroff", UK_TEXT);
    term_print("Tip: Use Up/Down arrows for history, Tab for auto-complete.", UK_OVERLAY0);
}

/* ── Execute Command ─────────────────────────────────────────────────────────── */
static void execute_command(const char *raw_cmd)
{
    /* Echo prompt + command */
    char echo_line[TERM_COLS + 1];
    snprintf(echo_line, sizeof(echo_line), "%s$ %s", g_cwd, raw_cmd);
    term_print(echo_line, UK_GREEN);

    while (*raw_cmd == ' ' || *raw_cmd == '\t') raw_cmd++;
    if (!*raw_cmd) return;

    add_history(raw_cmd);

    char cmd_copy[MAX_CMD + 1];
    strncpy(cmd_copy, raw_cmd, MAX_CMD);
    cmd_copy[MAX_CMD] = '\0';

    /* Parse command name & args */
    char *argv[16];
    int argc = 0;
    char *p = cmd_copy;
    while (*p && argc < 15) {
        while (*p == ' ' || *p == '\t') *p++ = '\0';
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    argv[argc] = NULL;
    if (argc == 0) return;

    /* Built-in: clear */
    if (strcmp(argv[0], "clear") == 0) {
        g_line_count = 0;
        g_scroll     = 0;
        return;
    }

    /* Built-in: exit */
    if (strcmp(argv[0], "exit") == 0) {
        sys_exit(0);
        return;
    }

    /* Built-in: help */
    if (strcmp(argv[0], "help") == 0) {
        show_help();
        return;
    }

    /* Built-in: pwd */
    if (strcmp(argv[0], "pwd") == 0) {
        update_cwd();
        term_print(g_cwd, UK_TEXT);
        return;
    }

    /* Built-in: cd */
    if (strcmp(argv[0], "cd") == 0) {
        const char *dest = (argc > 1) ? argv[1] : "/";
        if (sys_chdir(dest) < 0) {
            char err[64];
            snprintf(err, sizeof(err), "cd: %s: No such file or directory", dest);
            term_print(err, UK_RED);
        } else {
            update_cwd();
        }
        return;
    }

    /* Built-in: history */
    if (strcmp(argv[0], "history") == 0) {
        for (int i = 0; i < g_hist_count; i++) {
            char hline[TERM_COLS];
            snprintf(hline, sizeof(hline), "%3d  %s", i + 1, g_history[i]);
            term_print(hline, UK_SUBTEXT0);
        }
        return;
    }

    /* Built-in: launch */
    if (strcmp(argv[0], "launch") == 0) {
        if (argc < 2) {
            term_print("Usage: launch <app_name> (e.g. launch settings, launch calculator)", UK_YELLOW);
        } else {
            char app_path[64];
            if (argv[1][0] == '/') {
                snprintf(app_path, sizeof(app_path), "%s", argv[1]);
            } else {
                snprintf(app_path, sizeof(app_path), "/bin/%s%s", argv[1],
                         (strstr(argv[1], ".elf") ? "" : ".elf"));
            }
            char notify[80];
            snprintf(notify, sizeof(notify), "Launching '%s'...", app_path);
            term_print(notify, UK_SAPPHIRE);
            uk_launch_app(&g_win, app_path);
        }
        return;
    }

    /* Execute command via UNIX Pipe and subshell */
    int pipefd[2];
    if (sys_pipe(pipefd) < 0) {
        term_print("terminal: pipe creation failed", UK_RED);
        return;
    }

    int pid = sys_fork();
    if (pid == 0) {
        /* Child: redirect stdout and stderr to write end of pipe */
        sys_close(pipefd[0]);
        if (pipefd[1] != 1) {
            sys_dup2(pipefd[1], 1);
            sys_close(pipefd[1]);
        }
        sys_dup2(1, 2);

        char *sh_argv[4];
        sh_argv[0] = "/bin/sh.elf";
        sh_argv[1] = "-c";
        sh_argv[2] = (char *)raw_cmd;
        sh_argv[3] = NULL;

        char *const envp[] = { "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/", "USER=root", "HOME=/root", "TERM=azami", NULL };
        sys_execve("/bin/sh.elf", sh_argv, envp);
        sys_execve("/sh.elf", sh_argv, envp);
        sys_exit(127);
    } else if (pid > 0) {
        /* Parent: Read output stream from child */
        sys_close(pipefd[1]);

        char fbuf[512];
        ssize_t n;
        while ((n = sys_read(pipefd[0], fbuf, sizeof(fbuf))) > 0) {
            term_print_stream_chunk(fbuf, (size_t)n);
            draw_terminal();
        }
        sys_close(pipefd[0]);

        int status = 0;
        sys_wait4(pid, &status, 0);
        draw_terminal();
    } else {
        term_print("terminal: process spawn failed", UK_RED);
        sys_close(pipefd[0]);
        sys_close(pipefd[1]);
    }
}

/* ── Draw Terminal Window ────────────────────────────────────────────────────── */
static void draw_terminal(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    /* Dark background */
    uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_CRUST);

    /* Header Bar */
    uk_fill_rect(&g_win, 0, 0, (int)w, 36, UK_SURFACE0);
    uk_fill_rect(&g_win, 0, 0, 4, 36, UK_GREEN);
    uk_draw_text(&g_win, 14, 6,  "AzamiOS Terminal", UK_TEXT);

    char subhdr[64];
    snprintf(subhdr, sizeof(subhdr), "root@azamios:%s — type 'help' for commands", g_cwd);
    uk_draw_text(&g_win, 14, 20, subhdr, UK_OVERLAY0);

    /* Window Control Dots */
    uk_fill_circle(&g_win, (int)w - 56, 18, 6, UK_RED);
    uk_fill_circle(&g_win, (int)w - 38, 18, 6, UK_YELLOW);
    uk_fill_circle(&g_win, (int)w - 20, 18, 6, UK_GREEN);
    uk_hline(&g_win, 0, 36, (int)w, UK_SURFACE1);

    /* Output text area */
    int max_visible = (int)(h - 80) / FONT_H;
    if (max_visible < 1) max_visible = 1;

    for (int r = 0; r < max_visible; r++) {
        int idx = r + g_scroll;
        if (idx >= g_line_count) break;
        int py = TERM_OY + r * FONT_H;
        uk_draw_text(&g_win, TERM_OX, py, g_lines[idx], g_line_colors[idx]);
    }

    /* Input prompt bar */
    int input_y = (int)h - FONT_H - 8;
    uk_fill_rect(&g_win, 0, input_y - 3, (int)w, FONT_H + 11, UK_SURFACE0);
    uk_hline(&g_win, 0, input_y - 3, (int)w, UK_SURFACE1);

    /* Prompt indicator */
    char prompt[32];
    snprintf(prompt, sizeof(prompt), "%s$ ", (strcmp(g_cwd, "/") == 0) ? "/" : (g_cwd + strlen(g_cwd) - (strlen(g_cwd) > 12 ? 12 : strlen(g_cwd))));
    int plen = strlen(prompt);
    uk_draw_text(&g_win, TERM_OX, input_y, prompt, UK_GREEN);
    uk_draw_text(&g_win, TERM_OX + plen * FONT_W, input_y, g_input, UK_TEXT);

    /* Cursor blink */
    if ((g_tick / 5) % 2 == 0) {
        int cx = TERM_OX + plen * FONT_W + g_input_len * FONT_W;
        uk_fill_rect(&g_win, cx, input_y, 2, FONT_H, UK_GREEN);
    }

    uk_invalidate(&g_win);
}

/* ── Key Event Handler ───────────────────────────────────────────────────────── */
static void handle_key(unsigned char keycode, unsigned char scancode, unsigned char pressed, unsigned short modifiers)
{
    if (!pressed) return;

    /* Enter */
    if (keycode == '\n' || keycode == '\r' || scancode == 28) {
        g_input[g_input_len] = '\0';
        execute_command(g_input);
        g_input_len = 0;
        g_input[0] = '\0';
        return;
    }

    /* Backspace */
    if (keycode == '\b' || keycode == 127 || scancode == 14) {
        if (g_input_len > 0) {
            g_input[--g_input_len] = '\0';
        }
        return;
    }

    /* Tab: Auto-complete */
    if (keycode == '\t' || scancode == 15) {
        do_tab_completion();
        return;
    }

    /* Up Arrow: History backward */
    if (scancode == 72 || scancode == 0x48) {
        if (g_hist_idx > 0 && g_hist_count > 0) {
            g_hist_idx--;
            strncpy(g_input, g_history[g_hist_idx], MAX_CMD);
            g_input_len = strlen(g_input);
        }
        return;
    }

    /* Down Arrow: History forward */
    if (scancode == 80 || scancode == 0x50) {
        if (g_hist_idx < g_hist_count - 1) {
            g_hist_idx++;
            strncpy(g_input, g_history[g_hist_idx], MAX_CMD);
            g_input_len = strlen(g_input);
        } else {
            g_hist_idx = g_hist_count;
            g_input[0] = '\0';
            g_input_len = 0;
        }
        return;
    }

    /* Page Up: Scroll back */
    if (scancode == 73 || scancode == 0x49) {
        if (g_scroll > 5) g_scroll -= 5;
        else g_scroll = 0;
        return;
    }

    /* Page Down: Scroll forward */
    if (scancode == 81 || scancode == 0x51) {
        if (g_scroll + 5 < g_line_count) g_scroll += 5;
        return;
    }

    /* Direct ASCII printable */
    char c = 0;
    if (keycode >= 32 && keycode <= 126) {
        c = (char)keycode;
    } else {
        /* Fallback scancode translation */
        int shift = (modifiers & 1) != 0;
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
    }

    if (c && g_input_len < MAX_CMD) {
        g_input[g_input_len++] = c;
        g_input[g_input_len] = '\0';
    }
}

/* ── Main ────────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    de_log("[terminal] Starting...");
    update_cwd();

    /* Welcome message */
    term_print("AzamiOS POSIX Terminal v3.0 (x86_64 SMP)", UK_MAUVE);
    term_print("Type 'help' for commands, or run binaries directly (ls, fetch, uname, etc.)", UK_OVERLAY0);
    term_print("", 0);

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0 && fb.width > 0 && fb.height > 0) {
        sw = fb.width;
        sh = fb.height;
    }

    int ret = uk_window_connect(&g_win, "Terminal",
                                (int)(sw / 2) - WIN_W / 2,
                                (int)(sh / 2) - WIN_H / 2,
                                WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN);
    if (ret < 0) {
        de_log("[terminal] FATAL: Failed to connect window");
        return -1;
    }

    /* Register timer for cursor blinking (every 100ms) */
    az_set_timer(g_win.client_chan, 100, 0);

    draw_terminal();

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) {
            break;
        }

        g_tick++;
        bool needs_redraw = true;

        if (msg.type == AZ_WM_KEY_EVENT) {
            handle_key(msg.key.keycode, msg.key.scancode, msg.key.pressed, msg.key.modifiers);
        } else if (msg.type == AZ_WM_TIMER_TICK) {
            /* Blink cursor */
            needs_redraw = true;
        } else if (msg.type == AZ_WM_MOUSE_EVENT) {
            if (msg.mouse.buttons == 0) {
                needs_redraw = false;
            }
        }

        if (needs_redraw) {
            draw_terminal();
        }
    }

    sys_exit(0);
}
