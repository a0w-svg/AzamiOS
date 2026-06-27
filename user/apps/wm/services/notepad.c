/**
 * notepad.c — AzamiOS Text Editor Service
 *
 * EDUCATIONAL MEMORY SAFETY EXPLANATIONS:
 * 1. Bounded Text Grid Traversal:
 *    Text input loops verify cursor row (`note_r`) and column (`note_c`) boundaries
 *    against `NOTE_ROWS` and `NOTE_COLS` prior to insertion. Carriage returns and
 *    backspaces clamp index pointers within `[0, NOTE_ROWS-1]` preventing stack corruption.
 * 2. Static Redraw Optimization:
 *    Notepad specifies `WM_SRV_FLAG_NONE`, signaling the core compositor that redraws
 *    are only necessary upon user keyboard/mouse events or global UI refreshes.
 */

#include "../wm.h"

/* ── Notepad state ───────────────────────────────────────────────────────── */
static char note_buf[NOTE_ROWS][NOTE_COLS];
static int  note_r = 0, note_c = 0;

static void note_clear(void) {
    for (int r = 0; r < NOTE_ROWS; r++)
        for (int c = 0; c < NOTE_COLS; c++)
            note_buf[r][c] = ' ';
    note_r = 0; note_c = 0;
}

static void note_putc(char ch) {
    if (ch == '\n' || ch == '\r') {
        note_c = 0; note_r++;
        if (note_r >= NOTE_ROWS) note_r = NOTE_ROWS - 1;
        return;
    }
    if (ch == '\b' || ch == 127) {
        if (note_c > 0) { note_c--; note_buf[note_r][note_c] = ' '; }
        else if (note_r > 0) { note_r--; note_c = NOTE_COLS - 1; }
        return;
    }
    if (note_c < NOTE_COLS && note_r < NOTE_ROWS) {
        note_buf[note_r][note_c] = ch;
        note_c++;
        if (note_c >= NOTE_COLS) { note_c = 0; note_r++; if (note_r >= NOTE_ROWS) note_r = NOTE_ROWS - 1; }
    }
}

static void notepad_init(window_t *w) {
    (void)w;
    note_clear();
}

static void notepad_open(window_t *w) {
    (void)w;
    note_clear();
}

static void notepad_render(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    (void)t; (void)frame_cnt;
    if (!w) return;
    int bx = w->x + 1;
    int by = w->y + TITLEBAR_H;
    int bw = w->w - 2;
    int bh = w->h - TITLEBAR_H - 1;

    draw_rect(bx, by, bw, bh, COL_NOTE_BG);
    for (int r = 0; r < NOTE_ROWS; r++) {
        int ly = by + 4 + r * 10 + 9;
        if (ly < by + bh - 2)
            draw_rect(bx + 4, ly, bw - 8, 1, COL_NOTE_LINE);
    }
    for (int r = 0; r < NOTE_ROWS; r++) {
        char line[NOTE_COLS + 1];
        int lp = 0;
        for (int c = 0; c < NOTE_COLS; c++)
            line[lp++] = note_buf[r][c];
        line[lp] = 0;
        draw_text(bx + 6, by + 4 + r * 10, line, COL_TEXT_DARK, COL_NOTE_BG);
    }
    if (g_focus == w->id && blink < 20) {
        draw_rect(bx + 6 + note_c * 8, by + 12 + note_r * 10, 7, 2, COL_TEXT_BLUE);
    }
}

static void notepad_on_key(window_t *w, int c, rtc_time_t *t, uint32_t frame_cnt) {
    (void)t; (void)frame_cnt;
    if (!w || !w->open || w->minimized) return;
    if (c == '\n' || c == '\r') {
        note_putc('\n');
    } else if (c == '\b' || c == 127) {
        note_putc('\b');
    } else if (c >= 32 && c <= 126) {
        note_putc(c);
    }
}

void notepad_service_init(void) {
    static const wm_service_t notepad_srv = {
        WIN_NOTEPAD,
        "Notepad",
        WM_SRV_FLAG_NONE,
        notepad_init,
        notepad_open,
        NULL,
        notepad_render,
        notepad_on_key
    };
    wm_register_service(&notepad_srv);
}
