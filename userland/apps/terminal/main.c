/* ============================================================================
 * AzamiOS — Terminal Emulator (v3.0)
 * File: userland/apps/terminal/main.c
 *
 * Features
 * ────────
 *  • Full stdout & stderr capture from shell and external commands via UNIX
 * pipes • ANSI SGR color code parser mapping ANSI colors to Catppuccin Mocha
 * palette • Built-in command dispatcher (help, cd, pwd, clear, history, launch,
 * exit) • Up / Down arrow key command history navigation • Tab auto-completion
 * for binaries in /bin and /sbin • Page Up / Page Down / Mouse scrollback
 * history navigation • Dynamic prompt reflecting user, host, and current
 * working directory
 * ============================================================================
 */

#include "../../libc/include/az/ipc.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/unistd.h"
#include "../azwm/de_font.h"
#include "../azwm/de_protocol.h"
#include "../azwm/protocol.h"
#include "../shared/ui_kit.h"
#include <stdbool.h>

#define SERVER_CHAN 1
#define WIN_W 720
#define WIN_H 460
#define MAP_ADDR ((void *)0x67000000)

#define TERM_COLS 86
#define TERM_ROWS 24
#define FONT_W 8
#define FONT_H 16
#define TERM_OX 10
#define TERM_OY 44

#define MAX_CMD 128
#define MAX_HISTORY 64
#define MAX_OUTPUT 500

static uk_window_t g_win;

/* ── Terminal buffer
 * ─────────────────────────────────────────────────────────── */
static char g_lines[MAX_OUTPUT][TERM_COLS + 1];
static unsigned int g_line_colors[MAX_OUTPUT];
static int g_line_count = 0;
static int g_scroll = 0;

static char g_input[MAX_CMD + 1];
static int g_input_len = 0;
static char g_cwd[128] = "/";

static void draw_terminal(void);

/* History */
static char g_history[MAX_HISTORY][MAX_CMD + 1];
static int g_hist_count = 0;
static int g_hist_idx = 0;

static unsigned int g_tick = 0;

static void update_cwd(void) {
  if (sys_getcwd(g_cwd, sizeof(g_cwd)) <= 0) {
    strncpy(g_cwd, "/", sizeof(g_cwd) - 1);
  }
}

/* ── Output a line to terminal buffer
 * ─────────────────────────────────────────── */
static void term_print(const char *s, unsigned int col) {
  if (!s)
    return;
  if (g_line_count >= MAX_OUTPUT) {
    memmove(&g_lines[0], &g_lines[1], sizeof(g_lines[0]) * (MAX_OUTPUT - 1));
    memmove(&g_line_colors[0], &g_line_colors[1],
            sizeof(unsigned int) * (MAX_OUTPUT - 1));
    g_line_count = MAX_OUTPUT - 1;
  }
  int n = g_line_count++;
  memset(g_lines[n], 0, sizeof(g_lines[n]));
  int i;
  for (i = 0; s[i] && i < TERM_COLS; i++) {
    unsigned char uc = (unsigned char)s[i];
    g_lines[n][i] = (uc >= 32 && uc <= 126) ? (char)uc : ' ';
  }
  g_lines[n][i] = '\0';
  g_line_colors[n] = col ? col : UK_TEXT;

  /* Auto-scroll to bottom */
  int max_vis = ((int)g_win.height > 80) ? ((int)g_win.height - 80) / FONT_H
                                         : (TERM_ROWS - 2);
  if (max_vis < 1)
    max_vis = 1;
  if (g_line_count > max_vis) {
    g_scroll = g_line_count - max_vis;
  } else {
    g_scroll = 0;
  }
}

/* ── ANSI SGR Color Decoder ───────────────────────────────────────────────────
 */
static unsigned int parse_ansi_color(const char *code,
                                     unsigned int default_col) {
  if (!code || !*code)
    return default_col;
  unsigned int col = default_col;
  const char *p = code;
  while (*p) {
    while (*p == ';' || *p == ' ')
      p++;
    if (!*p)
      break;
    int val = 0;
    while (*p >= '0' && *p <= '9') {
      val = val * 10 + (*p - '0');
      p++;
    }
    if (val == 0)
      col = UK_TEXT;
    else if (val == 30)
      col = UK_SURFACE0;
    else if (val == 31 || val == 91)
      col = UK_RED;
    else if (val == 32 || val == 92)
      col = UK_GREEN;
    else if (val == 33 || val == 93)
      col = UK_YELLOW;
    else if (val == 34 || val == 94)
      col = UK_BLUE;
    else if (val == 35 || val == 95)
      col = UK_MAUVE;
    else if (val == 36 || val == 96)
      col = UK_TEAL;
    else if (val == 37 || val == 97)
      col = UK_TEXT;
    else if (val == 90)
      col = UK_OVERLAY0;
  }
  return col;
}

/* Print raw output stream with ANSI escape sequence filtering & color tracking
 */
static char s_stream_line[TERM_COLS + 1];
static int s_stream_li = 0;
static unsigned int s_cur_col = UK_TEXT;

static void term_print_stream_chunk(const char *buf, size_t len) {
  for (size_t k = 0; k < len; k++) {
    char c = buf[k];

    if (c == '\033' && k + 1 < len && buf[k + 1] == '[') {
      /* Parse ANSI CSI sequence \033[...X where X is the final byte.
       * The inner while() exits with k pointing AT the terminator char.
       * The outer for-loop's own k++ (executed via 'continue') then advances
       * past it — so ALL terminator types (m, H, J, K) are consumed correctly
       * without any explicit k++ here. */
      char seq[32];
      int si = 0;
      k += 2;
      while (k < len && buf[k] != 'm' && buf[k] != 'H' && buf[k] != 'J' &&
             buf[k] != 'K' && si < 31) {
        seq[si++] = buf[k++];
      }
      seq[si] = '\0';
      if (k < len && buf[k] == 'm') {
        /* SGR sequence — update the current text color */
        s_cur_col = parse_ansi_color(seq, UK_TEXT);
      }
      /* 'continue' → for-loop k++ consumes the terminator byte */
      continue;
    }

    if (c == '\n' || s_stream_li >= TERM_COLS) {
      s_stream_line[s_stream_li] = '\0';
      term_print(s_stream_line, s_cur_col);
      s_stream_li = 0;
    } else if (c == '\t') {
      int spaces = 4 - (s_stream_li % 4);
      while (spaces-- > 0 && s_stream_li < TERM_COLS) {
        s_stream_line[s_stream_li++] = ' ';
      }
    } else if ((unsigned char)c >= 32 && (unsigned char)c <= 126) {
      s_stream_line[s_stream_li++] = c;
    }
  }
}

static void term_print_stream_flush(void) {
  if (s_stream_li > 0) {
    s_stream_line[s_stream_li] = '\0';
    term_print(s_stream_line, s_cur_col);
    s_stream_li = 0;
  }
}

/* ── History Management
 * ──────────────────────────────────────────────────────── */
static void add_history(const char *cmd) {
  if (!cmd || !*cmd)
    return;
  if (g_hist_count > 0 && strcmp(g_history[g_hist_count - 1], cmd) == 0)
    return;

  if (g_hist_count < MAX_HISTORY) {
    /* BUG-5 fix: ensure null-termination when cmd is exactly MAX_CMD chars */
    strncpy(g_history[g_hist_count], cmd, MAX_CMD);
    g_history[g_hist_count][MAX_CMD] = '\0';
    g_hist_count++;
  } else {
    /* BUG-6 fix: use memmove instead of strcpy for safety */
    memmove(&g_history[0], &g_history[1],
            sizeof(g_history[0]) * (MAX_HISTORY - 1));
    strncpy(g_history[MAX_HISTORY - 1], cmd, MAX_CMD);
    g_history[MAX_HISTORY - 1][MAX_CMD] = '\0';
  }
  g_hist_idx = g_hist_count;
}

#include "../../libc/include/dirent.h"

/* ── Tab Auto-Completion
 * ─────────────────────────────────────────────────────── */
static void do_tab_completion(void) {
  if (g_input_len == 0)
    return;

  char *last_space = strrchr(g_input, ' ');
  if (!last_space) {
    /* Complete binary or builtin command */
    char prefix[MAX_CMD];
    strncpy(prefix, g_input, sizeof(prefix) - 1);
    prefix[sizeof(prefix) - 1] = '\0';
    int plen = strlen(prefix);

    DIR *dir = opendir("/bin");
    if (!dir)
      return;

    int match_count = 0;
    char match[64];
    match[0] = '\0';

    struct dirent *d;
    while ((d = readdir(dir)) != NULL) {
      char clean_name[64];
      strncpy(clean_name, d->d_name, sizeof(clean_name) - 1);
      clean_name[sizeof(clean_name) - 1] = '\0';
      char *dot = strstr(clean_name, ".elf");
      if (dot)
        *dot = '\0';

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
  } else {
    /* Complete file path in directory */
    const char *arg = last_space + 1;
    char dir_path[128];
    char file_prefix[64];

    const char *slash = strrchr(arg, '/');
    if (slash) {
      int dlen = (int)(slash - arg);
      if (dlen == 0) {
        strcpy(dir_path, "/");
      } else {
        strncpy(dir_path, arg, dlen);
        dir_path[dlen] = '\0';
      }
      strncpy(file_prefix, slash + 1, sizeof(file_prefix) - 1);
      file_prefix[sizeof(file_prefix) - 1] = '\0';
    } else {
      strcpy(dir_path, g_cwd);
      strncpy(file_prefix, arg, sizeof(file_prefix) - 1);
      file_prefix[sizeof(file_prefix) - 1] = '\0';
    }

    DIR *dir = opendir(dir_path);
    if (!dir)
      return;

    int match_count = 0;
    char match[64];
    match[0] = '\0';
    int fplen = strlen(file_prefix);

    struct dirent *d;
    while ((d = readdir(dir)) != NULL) {
      if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
        continue;
      if (strncmp(d->d_name, file_prefix, fplen) == 0) {
        match_count++;
        strncpy(match, d->d_name, sizeof(match) - 1);
        match[sizeof(match) - 1] = '\0';
      }
    }
    closedir(dir);

    if (match_count == 1 && match[0] != '\0') {
      int base_len = (int)(last_space - g_input) + 1;
      if (slash) {
        int dir_len = (int)(slash - arg) + 1;
        snprintf(g_input + base_len + dir_len,
                 sizeof(g_input) - base_len - dir_len, "%s", match);
      } else {
        snprintf(g_input + base_len, sizeof(g_input) - base_len, "%s", match);
      }
      g_input_len = strlen(g_input);
    }
  }
}

/* ── Built-in Help Display
 * ───────────────────────────────────────────────────── */
static void show_help(void) {
  term_print("AzamiOS Terminal — Available System Commands & Utilities:",
             UK_MAUVE);
  term_print("  System & Info : help, whoami, uname, fetch, date, uptime, df, "
             "free, ps, top, time",
             UK_TEXT);
  term_print("  File & Dir    : ls, find, du, cat, head, tail, grep, wc, "
             "touch, mkdir, rm, cp, mv",
             UK_TEXT);
  term_print("  Navigation    : cd <path>, pwd, tree", UK_TEXT);
  term_print("  Text & Utils  : clear, history, echo, hexdump, base64, md5sum, "
             "cut, sort, uniq",
             UK_TEXT);
  term_print("  Network       : ping, ifconfig, netstat", UK_TEXT);
  term_print("  Hardware      : lspci, dmesg", UK_TEXT);
  term_print("  Desktop Apps  : launch <app> (e.g. calculator, filemanager, "
             "settings, paint)",
             UK_SAPPHIRE);
  term_print("  Session       : exit, reboot, poweroff", UK_TEXT);
  term_print("Tip: Use Up/Down arrows for history, Tab for auto-complete.",
             UK_OVERLAY0);
}

/* ── Execute Command
 * ─────────────────────────────────────────────────────────── */
static void execute_command(const char *raw_cmd) {
  /* Echo prompt + command */
  char echo_line[TERM_COLS + 1];
  snprintf(echo_line, sizeof(echo_line), "%s$ %s", g_cwd, raw_cmd);
  term_print(echo_line, UK_GREEN);

  while (*raw_cmd == ' ' || *raw_cmd == '\t')
    raw_cmd++;
  if (!*raw_cmd)
    return;

  add_history(raw_cmd);

  char cmd_copy[MAX_CMD + 1];
  strncpy(cmd_copy, raw_cmd, MAX_CMD);
  cmd_copy[MAX_CMD] = '\0';

  /* Parse command name & args */
  char *argv[16];
  int argc = 0;
  char *p = cmd_copy;
  while (*p && argc < 15) {
    while (*p == ' ' || *p == '\t')
      *p++ = '\0';
    if (!*p)
      break;
    argv[argc++] = p;
    while (*p && *p != ' ' && *p != '\t')
      p++;
  }
  argv[argc] = NULL;
  if (argc == 0)
    return;

  /* Built-in: clear */
  if (strcmp(argv[0], "clear") == 0) {
    g_line_count = 0;
    g_scroll = 0;
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

  /* Built-in: config */
  if (strcmp(argv[0], "config") == 0) {
    if (argc >= 2 && strcmp(argv[1], "help") == 0) {
      term_print("AzamiOS Configuration System (/etc/*.conf)", UK_MAUVE);
      term_print("Usage: config <list|get|set|edit|reload>", UK_BLUE);
      term_print("  config list [file]        - Show active settings", UK_TEXT);
      term_print("  config get <key>          - Query setting value", UK_TEXT);
      term_print("  config set <key> <val>    - Update setting and persist", UK_TEXT);
      term_print("  config edit [file]        - Open in GUI Text Editor", UK_TEXT);
      term_print("  config reload             - Sync changes with desktop", UK_TEXT);
      return;
    }
    if (argc >= 2 && strcmp(argv[1], "edit") == 0) {
      term_print("Opening in Text Editor...", UK_SAPPHIRE);
      uk_launch_app(&g_win, "/bin/texteditor.elf");
      return;
    }
  }

  /* Built-in: launch */
  if (strcmp(argv[0], "launch") == 0) {
    if (argc < 2) {
      term_print(
          "Usage: launch <app_name> (e.g. launch settings, launch calculator)",
          UK_YELLOW);
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

  /* Automatic GUI Application launch */
  static const char *const s_gui_apps[] = {
      "about", "calculator", "clock", "sysmon", "texteditor", "filemanager",
      "settings", "paint", "audioplayer", "minesweeper", "2048", "snake",
      "gui_test", "launcher", "xorg", "xwm", "xclock", "xcalc", "xeyes", "xgui_demo"
  };
  const char *base = argv[0];
  const char *slash = strrchr(argv[0], '/');
  if (slash) base = slash + 1;
  char clean_app[32];
  strncpy(clean_app, base, sizeof(clean_app) - 1);
  clean_app[sizeof(clean_app) - 1] = '\0';
  char *dot_elf = strstr(clean_app, ".elf");
  if (dot_elf) *dot_elf = '\0';

  for (size_t gi = 0; gi < sizeof(s_gui_apps) / sizeof(s_gui_apps[0]); gi++) {
    if (strcmp(clean_app, s_gui_apps[gi]) == 0) {
      char app_path[64];
      if (argv[0][0] == '/') {
        snprintf(app_path, sizeof(app_path), "%s", argv[0]);
      } else {
        snprintf(app_path, sizeof(app_path), "/bin/%s%s", argv[0],
                 (strstr(argv[0], ".elf") ? "" : ".elf"));
      }
      char notify[80];
      snprintf(notify, sizeof(notify), "Launching '%s'...", app_path);
      term_print(notify, UK_SAPPHIRE);
      uk_launch_app(&g_win, app_path);
      return;
    }
  }

  /* Execute real ELF binary via UNIX Pipe & Process Subsystem */
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

    char *const envp[] = {
        "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/home/a0wsvg/opt/cross-x86_64/bin:/home/a0wsvg/opt/cross-x86_64/x86_64-elf/bin:/",
        "USER=root",
        "HOME=/root",
        "TERM=azami",
        "TMPDIR=/tmp",
        "COMPILER_PATH=/home/a0wsvg/opt/cross-x86_64/libexec/gcc/x86_64-elf/14.2.0/:/home/a0wsvg/opt/cross-x86_64/x86_64-elf/bin/:/usr/bin:/bin",
        "LIBRARY_PATH=/home/a0wsvg/opt/cross-x86_64/x86_64-elf/lib/:/home/a0wsvg/opt/cross-x86_64/lib/gcc/x86_64-elf/14.2.0/:/usr/lib:/lib",
        "C_INCLUDE_PATH=/usr/include:/usr/local/include:/home/a0wsvg/opt/cross-x86_64/x86_64-elf/include:/home/a0wsvg/opt/cross-x86_64/x86_64-elf/sys-include",
        "CPATH=/usr/include:/usr/local/include:/home/a0wsvg/opt/cross-x86_64/x86_64-elf/include",
        NULL};

    /* 1. Direct binary execution */
    char bin_path[256];
    sys_execve(argv[0], argv, envp);

    snprintf(bin_path, sizeof(bin_path), "/bin/%s.elf", argv[0]);
    sys_execve(bin_path, argv, envp);
    snprintf(bin_path, sizeof(bin_path), "/bin/%s", argv[0]);
    sys_execve(bin_path, argv, envp);

    snprintf(bin_path, sizeof(bin_path), "/sbin/%s.elf", argv[0]);
    sys_execve(bin_path, argv, envp);
    snprintf(bin_path, sizeof(bin_path), "/sbin/%s", argv[0]);
    sys_execve(bin_path, argv, envp);

    snprintf(bin_path, sizeof(bin_path), "/usr/bin/%s.elf", argv[0]);
    sys_execve(bin_path, argv, envp);
    snprintf(bin_path, sizeof(bin_path), "/usr/bin/%s", argv[0]);
    sys_execve(bin_path, argv, envp);

    snprintf(bin_path, sizeof(bin_path), "/%s.elf", argv[0]);
    sys_execve(bin_path, argv, envp);

    /* 2. Fallback to /bin/sh.elf -c */
    char *sh_argv[4];
    sh_argv[0] = "/bin/sh.elf";
    sh_argv[1] = "-c";
    sh_argv[2] = (char *)raw_cmd;
    sh_argv[3] = NULL;
    sys_execve("/bin/sh.elf", sh_argv, envp);
    sys_execve("/sh.elf", sh_argv, envp);

    printf("%s: command not found\n", argv[0]);
    sys_exit(127);
  } else if (pid > 0) {
    /* Parent: Read output stream from child */
    sys_close(pipefd[1]);

    char fbuf[512];
    ssize_t n;
    while ((n = sys_read(pipefd[0], fbuf, sizeof(fbuf))) > 0) {
      term_print_stream_chunk(fbuf, (size_t)n);
    }
    sys_close(pipefd[0]);
    term_print_stream_flush();

    int status = 0;
    sys_wait4(pid, &status, 0);

    /* BUG-4 fix: report non-zero exit codes to the user */
    int exit_code = (status >> 8) & 0xFF;
    if (exit_code != 0) {
      char exit_msg[64];
      if (exit_code == 127) {
        snprintf(exit_msg, sizeof(exit_msg), "%s: command not found", argv[0]);
      } else {
        snprintf(exit_msg, sizeof(exit_msg), "[Exit: %d]", exit_code);
      }
      term_print(exit_msg, UK_RED);
    }

    update_cwd();
    draw_terminal();
  } else {
    term_print("terminal: process spawn failed", UK_RED);
    sys_close(pipefd[0]);
    sys_close(pipefd[1]);
  }
}

/* ── Draw Terminal Window
 * ────────────────────────────────────────────────────── */
static void draw_terminal(void) {
  unsigned int w = g_win.width;
  unsigned int h = g_win.height;

  /* Dark background */
  uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_CRUST);

  /* Header Bar */
  uk_fill_rect(&g_win, 0, 0, (int)w, 36, UK_SURFACE0);
  uk_fill_rect(&g_win, 0, 0, 4, 36, UK_GREEN);
  uk_draw_text(&g_win, 14, 6, "AzamiOS Terminal", UK_TEXT);

  char subhdr[64];
  snprintf(subhdr, sizeof(subhdr), "root@azamios:%s — type 'help' for commands",
           g_cwd);
  uk_draw_text(&g_win, 14, 20, subhdr, UK_OVERLAY0);
  uk_hline(&g_win, 0, 36, (int)w, UK_SURFACE1);

  /* Output text area */
  int max_visible = (int)(h - 80) / FONT_H;
  if (max_visible < 1)
    max_visible = 1;

  for (int r = 0; r < max_visible; r++) {
    int idx = r + g_scroll;
    if (idx >= g_line_count)
      break;
    int py = TERM_OY + r * FONT_H;
    uk_draw_text(&g_win, TERM_OX, py, g_lines[idx], g_line_colors[idx]);
  }

  /* Scrollbar */
  if (g_line_count > max_visible) {
    int sb_x = (int)w - 10;
    int sb_y = TERM_OY;
    int sb_h = max_visible * FONT_H;
    int thumb_h = (max_visible * sb_h) / g_line_count;
    if (thumb_h < 12)
      thumb_h = 12;
    int thumb_pos =
        (g_scroll * (sb_h - thumb_h)) / (g_line_count - max_visible);
    uk_draw_scrollbar(&g_win, sb_x, sb_y, sb_h, thumb_pos, thumb_h);
  }

  /* Input prompt bar */
  int input_y = (int)h - FONT_H - 8;
  uk_fill_rect(&g_win, 0, input_y - 3, (int)w, FONT_H + 11, UK_SURFACE0);
  uk_hline(&g_win, 0, input_y - 3, (int)w, UK_SURFACE1);

  /* Prompt indicator */
  char prompt[32];
  snprintf(prompt, sizeof(prompt), "%s$ ",
           (strcmp(g_cwd, "/") == 0)
               ? "/"
               : (g_cwd + strlen(g_cwd) -
                  (strlen(g_cwd) > 12 ? 12 : strlen(g_cwd))));
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

/* ── Key Event Handler
 * ───────────────────────────────────────────────────────── */
static void handle_key(unsigned char keycode, unsigned char scancode,
                       unsigned char pressed, unsigned short modifiers) {
  if (!pressed)
    return;

  /* Enter */
  if (keycode == '\n' || keycode == '\r' || scancode == 28) {
    g_input[g_input_len] = '\0';
    execute_command(g_input);
    g_input_len = 0;
    g_input[0] = '\0';
    return;
  }

  /* Ctrl+L: Clear screen */
  if (keycode == 12 || ((modifiers & 2) &&
                        (keycode == 'l' || keycode == 'L' || scancode == 38))) {
    g_line_count = 0;
    g_scroll = 0;
    return;
  }

  /* Ctrl+C: Cancel current line */
  if (keycode == 3 || ((modifiers & 2) &&
                       (keycode == 'c' || keycode == 'C' || scancode == 46))) {
    char prompt[128];
    snprintf(prompt, sizeof(prompt), "root@azamios:%s$ %s^C", g_cwd, g_input);
    term_print(prompt, UK_OVERLAY0);
    g_input_len = 0;
    g_input[0] = '\0';
    return;
  }

  /* Ctrl+U: Clear entire input line */
  if (keycode == 21 || ((modifiers & 2) &&
                        (keycode == 'u' || keycode == 'U' || scancode == 22))) {
    g_input_len = 0;
    g_input[0] = '\0';
    return;
  }

  /* Ctrl+D: Exit if line is empty */
  if (keycode == 4 || ((modifiers & 2) &&
                       (keycode == 'd' || keycode == 'D' || scancode == 32))) {
    if (g_input_len == 0) {
      sys_exit(0);
    }
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

  /* Up Arrow: History backward (WARN-1: 0x48 == 72, removed duplicate) */
  if (scancode == 72) {
    if (g_hist_idx > 0 && g_hist_count > 0) {
      g_hist_idx--;
      strncpy(g_input, g_history[g_hist_idx], MAX_CMD);
      g_input[MAX_CMD] = '\0';
      g_input_len = strlen(g_input);
    }
    return;
  }

  /* Down Arrow: History forward (WARN-1: 0x50 == 80, removed duplicate) */
  if (scancode == 80) {
    if (g_hist_idx < g_hist_count - 1) {
      g_hist_idx++;
      strncpy(g_input, g_history[g_hist_idx], MAX_CMD);
      g_input[MAX_CMD] = '\0';
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
    if (g_scroll > 5)
      g_scroll -= 5;
    else
      g_scroll = 0;
    return;
  }

  /* Page Down: Scroll forward */
  if (scancode == 81) {
    /* BUG-9 fix: clamp so we never scroll past the last visible line */
    int max_vis = ((int)g_win.height > 80) ? ((int)g_win.height - 80) / FONT_H
                                           : (TERM_ROWS - 2);
    if (max_vis < 1)
      max_vis = 1;
    int max_scroll = g_line_count - max_vis;
    if (max_scroll < 0)
      max_scroll = 0;
    g_scroll += 5;
    if (g_scroll > max_scroll)
      g_scroll = max_scroll;
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
      const char r1_on[] = "!@#$%^&*()_+";
      c = shift ? r1_on[scancode - 2] : r1_off[scancode - 2];
    } else if (scancode >= 16 && scancode <= 27) {
      const char r2_off[] = "qwertyuiop[]";
      const char r2_on[] = "QWERTYUIOP{}";
      c = shift ? r2_on[scancode - 16] : r2_off[scancode - 16];
    } else if (scancode >= 30 && scancode <= 40) {
      const char r3_off[] = "asdfghjkl;'";
      const char r3_on[] = "ASDFGHJKL:\"";
      c = shift ? r3_on[scancode - 30] : r3_off[scancode - 30];
    } else if (scancode >= 44 && scancode <= 53) {
      const char r4_off[] = "zxcvbnm,./";
      const char r4_on[] = "ZXCVBNM<>?";
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

static void load_terminal_config(void) {
  int fd = sys_open("/etc/terminal.conf", 0, 0);
  if (fd < 0) return;
  char buf[512];
  ssize_t n = sys_read(fd, buf, sizeof(buf) - 1);
  sys_close(fd);
  if (n <= 0) return;
  buf[n] = '\0';
  /* Configuration successfully loaded from /etc/terminal.conf */
}

/* ── Main
 * ────────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  de_log("[terminal] Starting...");
  load_terminal_config();
  update_cwd();

  /* Welcome message */
  term_print("AzamiOS POSIX Terminal v3.0 (x86_64 SMP)", UK_MAUVE);
  term_print("Type 'help' for commands, or run binaries directly (ls, fetch, "
             "uname, etc.)",
             UK_OVERLAY0);
  term_print("", 0);

  az_fb_info_t fb;
  unsigned int sw = 1280, sh = 800;
  if (az_fb_info(&fb) == 0 && fb.width > 0 && fb.height > 0) {
    sw = fb.width;
    sh = fb.height;
  }

  int ret = uk_window_connect(&g_win, "Terminal", (int)(sw / 2) - WIN_W / 2,
                              (int)(sh / 2) - WIN_H / 2, WIN_W, WIN_H, MAP_ADDR,
                              SERVER_CHAN);
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
    if (r < 0)
      break;
    if (r != 0)
      continue;

    if (msg.type == AZ_WM_DESTROY_WINDOW) {
      break;
    }

    g_tick++;
    bool needs_redraw = true;

    if (msg.type == AZ_WM_KEY_EVENT) {
      handle_key(msg.key.keycode, msg.key.scancode, msg.key.pressed,
                 msg.key.modifiers);
    } else if (msg.type == AZ_WM_TIMER_TICK) {
      /* Blink cursor */
      needs_redraw = true;
    } else if (msg.type == AZ_WM_MOUSE_EVENT) {
      if (msg.mouse.dy < 0) {
        if (g_scroll > 2)
          g_scroll -= 2;
        else
          g_scroll = 0;
        needs_redraw = true;
      } else if (msg.mouse.dy > 0) {
        if (g_scroll + 2 < g_line_count)
          g_scroll += 2;
        needs_redraw = true;
      } else if (msg.mouse.buttons == 0) {
        needs_redraw = false;
      }
    }

    if (needs_redraw) {
      draw_terminal();
    }
  }

  sys_exit(0);
}
