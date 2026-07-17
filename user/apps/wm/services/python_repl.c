/**
 * python_repl.c — GUI Python Interactive Subsystem Window Service for AzamiOS
 */
#include "../wm.h"

#define MAX_PY_LINES 24
#define MAX_PY_COLS  60

static char s_lines[MAX_PY_LINES][MAX_PY_COLS + 1];
static int  s_cur_line = 0;
static char s_input[MAX_PY_COLS + 1];
static int  s_input_len = 0;

static void py_print(const char *txt) {
    if (s_cur_line >= MAX_PY_LINES) {
        for (int i = 0; i < MAX_PY_LINES - 1; i++) {
            memcpy(s_lines[i], s_lines[i + 1], MAX_PY_COLS + 1);
        }
        s_cur_line = MAX_PY_LINES - 1;
    }
    wm_strlcpy(s_lines[s_cur_line], txt, MAX_PY_COLS + 1);
    s_cur_line++;
}

static int py_eval(const char *e) {
    while (*e == ' ') e++;
    if (*e == '\0') return 0;
    const char *p = strchr(e, '+');
    if (p) {
        char left[32]; int l = p - e; if (l > 31) l = 31;
        memcpy(left, e, l); left[l] = '\0';
        return py_eval(left) + py_eval(p + 1);
    }
    p = strchr(e, '-');
    if (p && p != e) {
        char left[32]; int l = p - e; if (l > 31) l = 31;
        memcpy(left, e, l); left[l] = '\0';
        return py_eval(left) - py_eval(p + 1);
    }
    p = strchr(e, '*');
    if (p) {
        char left[32]; int l = p - e; if (l > 31) l = 31;
        memcpy(left, e, l); left[l] = '\0';
        return py_eval(left) * py_eval(p + 1);
    }
    return atoi(e);
}

static void py_exec(const char *cmd) {
    char pbuf[80];
    snprintf(pbuf, sizeof(pbuf), ">>> %s", cmd);
    py_print(pbuf);

    if (strncmp(cmd, "print(", 6) == 0) {
        const char *arg = cmd + 6;
        char out[60];
        int l = strlen(arg);
        if (l > 0 && arg[l-1] == ')') {
            if (arg[0] == '"' || arg[0] == '\'') {
                int end = l - 2;
                if (end > 58) end = 58;
                memcpy(out, arg + 1, end); out[end] = '\0';
                py_print(out);
            } else {
                char abuf[32]; int al = l - 1; if (al > 31) al = 31;
                memcpy(abuf, arg, al); abuf[al] = '\0';
                snprintf(out, sizeof(out), "%d", py_eval(abuf));
                py_print(out);
            }
        }
    } else if (strcmp(cmd, "help()") == 0) {
        py_print("AzamiOS Python: supports arithmetic (1+2*3),");
        py_print("print(\"hello\"), and expression evaluation.");
    } else if (strcmp(cmd, "clear") == 0) {
        s_cur_line = 0;
    } else if (strlen(cmd) > 0) {
        char out[40];
        snprintf(out, sizeof(out), "%d", py_eval(cmd));
        py_print(out);
    }
}

static void python_on_init(window_t *w) {
    if (!w) return;
    wm_resize_window(w, 520, 380);
    s_cur_line = 0;
    s_input_len = 0;
    s_input[0] = '\0';
    py_print("AzamiOS MicroPython 3.11 Interactive GUI");
    py_print("Type 'help()' or expressions like '25 * 4'");
}

static void python_on_open(window_t *w) { python_on_init(w); }

static void python_on_key(window_t *w, int c, rtc_time_t *t, uint32_t frame_cnt) {
    (void)w; (void)t; (void)frame_cnt;
    if (c == '\r' || c == '\n') {
        if (s_input_len > 0) {
            py_exec(s_input);
            s_input_len = 0;
            s_input[0] = '\0';
        }
    } else if (c == '\b' || c == 127) {
        if (s_input_len > 0) s_input[--s_input_len] = '\0';
    } else if (c >= 32 && c <= 126) {
        if (s_input_len < MAX_PY_COLS - 1) {
            s_input[s_input_len++] = (char)c;
            s_input[s_input_len] = '\0';
        }
    }
}

static void python_on_render(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    (void)t; (void)frame_cnt;
    if (!w) return;
    int bx = w->x + 8;
    int by = w->y + TITLEBAR_H + 8;

    draw_rect(bx, by, w->w - 16, w->h - TITLEBAR_H - 16, 0x000F172A);

    int y = by + 6;
    for (int i = 0; i < s_cur_line; i++) {
        draw_text(bx + 8, y, s_lines[i], COL_TEXT_GREEN, 0x000F172A);
        y += 14;
    }

    /* Prompt line */
    char prompt[80];
    snprintf(prompt, sizeof(prompt), ">>> %s%s", s_input, (blink < 20) ? "_" : " ");
    draw_text(bx + 8, y + 4, prompt, COL_TEXT_YELLOW, 0x000F172A);
}

void python_repl_service_init(void) {
    static const wm_service_t py_srv = {
        WIN_PYTHON, "Python REPL", 0,
        python_on_init, python_on_open, NULL, python_on_render, python_on_key
    };
    wm_register_service(&py_srv);
}
