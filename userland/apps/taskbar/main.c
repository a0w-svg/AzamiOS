/* ============================================================================
 * AzamiOS Desktop Environment — Taskbar (Panel)
 * File: userland/apps/taskbar/main.c   v3.0
 *
 * v3.0 Changes
 * ─────────────
 *  • Bug 3 fix: real clock via sys_clock_gettime (SYS_clock_gettime = 228)
 *    Autonomous timer via az_set_timer() fires AZ_WM_TIMER_TICK messages
 *    every second — clock advances independently of user input.
 *  • Panel height 44 → 52px for better proportions
 *  • Window buttons: pill-shaped (rounded rect radius 8), width 140px
 *  • Active window: mauve left pill + bottom accent line
 *  • Start button: pill-shape gradient with "⊞ Apps" label  
 *  • System tray: real HH:MM clock + date string + WiFi icon
 *  • Tray width expanded to 120px
 *  • Gradient separator lines (fade in/out) instead of dashed
 * ============================================================================ */

#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/string.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/de_log.h"

/* ── Configuration ─────────────────────────────────────────────────────────── */
#define SERVER_CHAN        1
#define TASKBAR_H         52      /* Panel pixel height (increased from 44)      */
#define DEFAULT_WIDTH   1280
#define DEFAULT_HEIGHT   800
#define TASKBAR_MAP_ADDR  ((void *)0x73000000)

/* Start button */
#define SB_X    6
#define SB_Y    6
#define SB_W   92
#define SB_H   40

/* Tray zone width (clock + date + wifi + sound + lock icon, right-aligned) */
#define TRAY_W  180
#define TRAY_M    8   /* tray right margin */

/* Window button geometry (pill-shaped) */
#define WB_W       140
#define WB_H        40
#define WB_GAP       5
#define WB_MAX        12  /* maximum visible window buttons before overflow */
#define WB_RADIUS     8   /* pill corner radius */

/* ── Quick Launch Dock ─────────────────────────────────────────────────────
 * Entries load from /etc/taskbar_dock.conf ("glyph|label|path|0xAARRGGBB"
 * per line) so the dock can be edited without recompiling; a compiled-in
 * default list (tb_load_default_dock(), further down) covers a missing or
 * unreadable file. Each entry also tries to load a real icon file
 * (/usr/share/icons/<name>.icn, matching the convention the launcher and
 * `make icons` already use) and falls back to the coloured glyph when one
 * isn't shipped for that app. */
#define TB_ICON_DIM     32
#define TB_ICON_PIXELS  (TB_ICON_DIM * TB_ICON_DIM)

typedef struct {
    char glyph;
    char label[16];
    char path[64];
    unsigned int color;
    unsigned int icon[TB_ICON_PIXELS];
    int has_icon;
} dock_app_t;

#define MAX_DOCK_APPS 24
static dock_app_t   g_dock_apps[MAX_DOCK_APPS];
static int          g_num_dock_apps = 0;

#define DOCK_X        (SB_X + SB_W + 12)
#define DOCK_BTN_W    30
#define DOCK_BTN_H    36
#define DOCK_GAP      4

/* ── Catppuccin Mocha palette (ARGB) ────────────────────────────────────────── */
#define C_BG          0xFF0A0A14  /* deeper than crust — premium dark panel      */
#define C_BG_TOP      0xFF2A2A3E  /* top border gradient highlight               */
#define C_SEPARATOR   0xFF252535  /* vertical separators                         */

/* Start button */
#define C_SB_IDLE     0xFF313244  /* surface0                                    */
#define C_SB_HOT      0xFF45475A  /* surface1 (hover)                            */
#define C_SB_BORDER   0xFFCBA6F7  /* mauve border on hover                       */
#define C_SB_TEXT     0xFFCBA6F7  /* mauve                                       */
#define C_SB_ICON     0xFFB4BEFE  /* lavender                                    */

/* Window buttons */
#define C_WB_BG       0xFF181822  /* just above base — slightly darker           */
#define C_WB_ACTIVE   0xFF2E2E42  /* active window bg                            */
#define C_WB_ACCENT   0xFFCBA6F7  /* mauve accent for focused pill + underline   */
#define C_WB_GLOW     0xFF3C2E54  /* subtle mauve glow tint for active btn       */
#define C_WB_TXT_ACT  0xFFCDD6F4  /* text                                        */
#define C_WB_TXT_IDL  0xFF585B70  /* overlay1 (dimmed)                           */

/* Tray */
#define C_CLOCK       0xFFA6E3A1  /* green                                       */
#define C_DATE        0xFF6C7086  /* overlay0 — secondary text                   */
#define C_TRAY_BG     0xFF141420  /* very dark tray background                   */
#define C_WIFI        0xFF89B4FA  /* blue                                        */

/* Overflow indicator */
#define C_OVERFLOW    0xFFF38BA8  /* red                                         */

/* ── Global drawing state ───────────────────────────────────────────────────── */
static unsigned int *g_px   = (unsigned int *)0;
static unsigned int  g_w    = DEFAULT_WIDTH;
static unsigned int  g_sh   = DEFAULT_HEIGHT;
static unsigned int  g_h    = TASKBAR_H;

/* Global IPC handles */
static int           g_srv  = SERVER_CHAN;
static int           g_cli  = -1;
static unsigned int  g_wid  = 0;

/* Start button hover state */
static unsigned char g_sb_hot = 0;

/* ── Animation state (driven by the 100ms AZ_WM_TIMER_TICK below) ────────── */
#define SB_PRESS_TICKS   3   /* Start-button click flash: 3 * 100ms = 300ms  */
#define SB_HOVER_STEP   64   /* hover blend moves this far toward its target
                               * per tick (0..256 range) — ~4 ticks to settle */
static unsigned int g_tick_count    = 0; /* increments every timer tick        */
static int          g_colon_on      = 1; /* clock ":" blink phase              */
static int          g_sb_hover_level = 0; /* 0..256 smoothed Start-button hover */
static int          g_sb_press_anim = 0; /* ticks left in the click flash       */
static int          g_dock_hovered  = -1; /* hovered Quick Launch Dock slot, -1 = none */
/* Per-slot smoothed hover (0..256) and click-flash countdown, animated in the
 * tick handler exactly like g_sb_hover_level/g_sb_press_anim above — the
 * dock used to just swap its background between two flat colours on the
 * frame the hovered slot changed, which read as dead next to the Start
 * button's eased fade and press flash right beside it. */
static int          g_dock_hover_level[MAX_DOCK_APPS];
static int          g_dock_press_anim[MAX_DOCK_APPS];

/* Window list */
typedef struct {
    unsigned int  wid;
    unsigned char active;
    unsigned char focused;
    char          title[64];
} tb_entry_t;

static tb_entry_t    g_wins[DE_TASKBAR_MAX_WINDOWS];
static unsigned int  g_launcher_wid = 0;
static unsigned int  g_win_count  = 0;
static unsigned int  g_focus_wid  = 0;
static int           g_overflow   = 0;

/* Volume state */
static int           g_vol_level = 80; /* 0..100 */
static int           g_vol_muted = 0;

/* Toast & Tooltip overlay state */
static char          g_toast_msg[48] = "";
static int           g_toast_ticks = 0;   /* countdown in 100ms ticks */
static char          g_hover_tooltip[48] = "";
static int           g_hover_x = 0;

static void tb_show_toast(const char *msg)
{
    if (!msg || msg[0] == '\0') {
        g_toast_ticks = 0;
        return;
    }
    strncpy(g_toast_msg, msg, sizeof(g_toast_msg) - 1);
    g_toast_msg[sizeof(g_toast_msg) - 1] = '\0';
    g_toast_ticks = 25; /* 2.5 seconds display */
}

/* ── Real-time clock (Bug 3 fix) ─────────────────────────────────────────── */
#include "../../libc/include/time.h"

/* Build "HH:MM" string */
static void tb_build_clock(char buf[6])
{
    time_t t = time(NULL);
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    snprintf(buf, 6, "%02d:%02d", tm_info.tm_hour, tm_info.tm_min);
}

/* Build "Mon Jan 14" date string */
static void tb_build_date(char *buf, int max)
{
    time_t t = time(NULL);
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    strftime(buf, max, "%a %b %e", &tm_info);
}

/* ── Drawing primitives ──────────────────────────────────────────────────────── */

static void tb_put_pixel(int x, int y, unsigned int col)
{
    if (x < 0 || y < 0 || (unsigned int)x >= g_w || (unsigned int)y >= g_h) return;
    g_px[(unsigned int)y * g_w + (unsigned int)x] = col;
}

static void tb_fill_rect(int rx, int ry, int rw, int rh, unsigned int col)
{
    int x, y;
    for (y = ry; y < ry + rh; y++)
        for (x = rx; x < rx + rw; x++)
            tb_put_pixel(x, y, col);
}

static void tb_hline(int x0, int y, int len, unsigned int col)
{
    int x;
    for (x = x0; x < x0 + len; x++) tb_put_pixel(x, y, col);
}

/* Blend src into dst at alpha/256 */
static unsigned int tb_blend(unsigned int dst, unsigned int src, unsigned int a)
{
    if (a > 256) a = 256;
    unsigned int sr=(src>>16)&0xFF, sg=(src>>8)&0xFF, sb=src&0xFF;
    unsigned int dr=(dst>>16)&0xFF, dg=(dst>>8)&0xFF, db=dst&0xFF;
    return 0xFF000000
         | (((sr*a + dr*(256-a))>>8) << 16)
         | (((sg*a + dg*(256-a))>>8) <<  8)
         |  ((sb*a + db*(256-a))>>8);
}

/* Rounded rectangle (pill shape) */
static void tb_fill_rounded(int rx, int ry, int rw, int rh, int r, unsigned int col)
{
    if (r <= 0 || rw <= 0 || rh <= 0) { tb_fill_rect(rx, ry, rw, rh, col); return; }
    if (r * 2 > rw) r = rw / 2;
    if (r * 2 > rh) r = rh / 2;
    /* Interior */
    tb_fill_rect(rx + r, ry, rw - 2 * r, rh, col);
    tb_fill_rect(rx, ry + r, r, rh - 2 * r, col);
    tb_fill_rect(rx + rw - r, ry + r, r, rh - 2 * r, col);
    /* Corners */
    int x, y;
    for (y = 0; y <= r; y++) {
        for (x = 0; x <= r; x++) {
            if (x * x + y * y <= r * r) {
                tb_put_pixel(rx + r - x, ry + r - y, col);
                tb_put_pixel(rx + rw - r + x - 1, ry + r - y, col);
                tb_put_pixel(rx + r - x, ry + rh - r + y - 1, col);
                tb_put_pixel(rx + rw - r + x - 1, ry + rh - r + y - 1, col);
            }
        }
    }
}

/* Gradient-fill a rectangle horizontally */
static void tb_fill_grad_h(int rx, int ry, int rw, int rh,
                             unsigned int lc, unsigned int rc)
{
    int x, y;
    for (x = rx; x < rx + rw; x++) {
        unsigned int t = (rw > 1) ? (unsigned int)((x - rx) * 255 / (rw - 1)) : 0;
        unsigned int col = tb_blend(lc, rc, t);
        for (y = ry; y < ry + rh; y++) tb_put_pixel(x, y, col);
    }
}

/* Text helpers */
static void tb_char(int x, int y, char c, unsigned int col)
{
    de_font_draw_char(g_px, g_w, g_w, g_h, x, y, c, col);
}

static void tb_str(int x, int y, const char *s, unsigned int col)
{
    int i;
    for (i = 0; s[i]; i++) tb_char(x + i * 8, y, s[i], col);
}

static void tb_str_clip(int x, int y, const char *s, unsigned int col, int max_w_px)
{
    int i;
    for (i = 0; s[i] && i * 8 + 8 <= max_w_px; i++)
        tb_char(x + i * 8, y, s[i], col);
}

static int tb_strlen(const char *s) { int i = 0; while (s[i]) i++; return i; }

/* ── Quick Launch Dock: config + icon loading ────────────────────────────── */

/* Extracts "terminal" out of "/bin/terminal.elf" — used to look up
 * /usr/share/icons/<name>.icn for a dock entry. */
static void tb_app_basename(const char *path, char *out, size_t out_len)
{
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    size_t n = strlen(name);
    if (n > 4 && strcmp(name + n - 4, ".elf") == 0) n -= 4;
    if (n >= out_len) n = out_len - 1;
    memcpy(out, name, n);
    out[n] = '\0';
}

static int tb_load_icon32(const char *app_name, unsigned int *out)
{
    char path[128];
    snprintf(path, sizeof(path), "/usr/share/icons/%s.icn", app_name);
    int fd = sys_open(path, 0, 0);
    if (fd < 0) return 0;
    int nr = sys_read(fd, out, TB_ICON_PIXELS * sizeof(unsigned int));
    sys_close(fd);
    return nr == (int)(TB_ICON_PIXELS * sizeof(unsigned int));
}

static void tb_draw_icon32(int x, int y, const unsigned int *icon)
{
    for (int py = 0; py < TB_ICON_DIM; py++) {
        for (int px = 0; px < TB_ICON_DIM; px++) {
            unsigned int c = icon[py * TB_ICON_DIM + px];
            unsigned char a = (unsigned char)(c >> 24);
            if (a == 0) continue;
            if (a == 255) { tb_put_pixel(x + px, y + py, c); continue; }
            int dx = x + px, dy = y + py;
            if (dx < 0 || dy < 0 || (unsigned int)dx >= g_w || (unsigned int)dy >= g_h) continue;
            unsigned int bg = g_px[(unsigned int)dy * g_w + (unsigned int)dx];
            tb_put_pixel(dx, dy, tb_blend(bg, c, a));
        }
    }
}

/* Compiled-in fallback, used only when /etc/taskbar_dock.conf is missing or
 * unreadable (e.g. a partial/corrupt install) so the dock is never just
 * empty. The shipped config (see userland/Makefile's `etc:` target) starts
 * out with this exact same list — edit the .conf to customise the dock. */
static void tb_load_default_dock(void)
{
    static const struct { char glyph; const char *label; const char *path; unsigned int color; } defaults[] = {
        { 'T', "Terminal", "/bin/terminal.elf",    0xFFA6E3A1 },
        { 'F', "Files",    "/bin/filemanager.elf", 0xFFF9E2AF },
        { 'E', "Editor",   "/bin/texteditor.elf",  0xFF89B4FA },
        { 'C', "Calc",     "/bin/calculator.elf",  0xFFFAB387 },
        { 'P', "Paint",    "/bin/paint.elf",       0xFFCBA6F7 },
        { 'A', "Audio",    "/bin/audioplayer.elf", 0xFFF38BA8 },
        { 'S', "Settings", "/bin/settings.elf",    0xFF74C7EC },
        { 'M', "Sysmon",   "/bin/sysmon.elf",      0xFF94E2D5 },
        { 'X', "XClock",   "/bin/xclock.elf",      0xFF89DCEB },
        { 'Y', "XEyes",    "/bin/xeyes.elf",       0xFFF5C2E7 },
        { 'K', "XCalc",    "/bin/xcalc.elf",       0xFFF9E2AF },
        { 'G', "XDemo",    "/bin/xgui_demo.elf",   0xFFB4BEFE },
    };
    g_num_dock_apps = 0;
    for (unsigned int i = 0; i < sizeof(defaults) / sizeof(defaults[0]) && g_num_dock_apps < MAX_DOCK_APPS; i++) {
        dock_app_t *e = &g_dock_apps[g_num_dock_apps++];
        e->glyph = defaults[i].glyph;
        strncpy(e->label, defaults[i].label, sizeof(e->label) - 1);
        e->label[sizeof(e->label) - 1] = '\0';
        strncpy(e->path, defaults[i].path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
        e->color = defaults[i].color;
    }
}

/* Parses "/etc/taskbar_dock.conf": one entry per line, formatted
 * "glyph|label|path|0xAARRGGBB" ('#' starts a comment, blank lines skipped).
 * Falls back to tb_load_default_dock() if the file is missing, empty, or
 * every line fails to parse. */
static void tb_load_dock_config(void)
{
    int fd = sys_open("/etc/taskbar_dock.conf", 0, 0);
    if (fd < 0) { tb_load_default_dock(); return; }

    static char buf[4096];
    int n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) { tb_load_default_dock(); return; }
    buf[n] = '\0';

    g_num_dock_apps = 0;
    char *saveptr = NULL;
    char *line = strtok_r(buf, "\n", &saveptr);
    while (line && g_num_dock_apps < MAX_DOCK_APPS) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '\0' || *line == '#') { line = strtok_r(NULL, "\n", &saveptr); continue; }

        char *fsave = NULL;
        char *glyph_s = strtok_r(line,  "|", &fsave);
        char *label_s = strtok_r(NULL, "|", &fsave);
        char *path_s  = strtok_r(NULL, "|", &fsave);
        char *color_s = strtok_r(NULL, "|", &fsave);
        if (glyph_s && label_s && path_s && color_s) {
            dock_app_t *e = &g_dock_apps[g_num_dock_apps++];
            e->glyph = glyph_s[0];
            strncpy(e->label, label_s, sizeof(e->label) - 1);
            e->label[sizeof(e->label) - 1] = '\0';
            strncpy(e->path, path_s, sizeof(e->path) - 1);
            e->path[sizeof(e->path) - 1] = '\0';
            e->color = (unsigned int)strtoul(color_s, NULL, 0);
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    if (g_num_dock_apps == 0) tb_load_default_dock();
}

static void tb_init_dock(void)
{
    tb_load_dock_config();
    for (int i = 0; i < g_num_dock_apps; i++) {
        char name[32];
        tb_app_basename(g_dock_apps[i].path, name, sizeof(name));
        g_dock_apps[i].has_icon = tb_load_icon32(name, g_dock_apps[i].icon);
    }
}

/* Where the window-button strip starts: right after the dock, whatever its
 * (config-controlled) length turns out to be. */
static int tb_wb_origin(void)
{
    return DOCK_X + g_num_dock_apps * (DOCK_BTN_W + DOCK_GAP) + 8;
}

/* ── WiFi icon ──────────────────────────────────────────────────────────────── */
static void tb_draw_wifi(int bx, int by, unsigned int col)
{
    tb_put_pixel(bx+0, by+0, col); tb_put_pixel(bx+6, by+0, col);
    tb_put_pixel(bx+1, by+1, col); tb_put_pixel(bx+5, by+1, col);
    tb_put_pixel(bx+1, by+3, col); tb_put_pixel(bx+5, by+3, col);
    tb_put_pixel(bx+2, by+4, col); tb_put_pixel(bx+4, by+4, col);
    tb_put_pixel(bx+2, by+6, col); tb_put_pixel(bx+4, by+6, col);
    tb_put_pixel(bx+3, by+7, col);
    tb_put_pixel(bx+2, by+9, col);
    tb_put_pixel(bx+3, by+9, col);
    tb_put_pixel(bx+4, by+9, col);
}

/* ── Sound / Volume icon ────────────────────────────────────────────────────── */
static void tb_draw_sound(int bx, int by, unsigned int col)
{
    tb_fill_rect(bx, by+3, 3, 5, col);
    tb_put_pixel(bx+3, by+2, col);
    tb_put_pixel(bx+4, by+1, col);
    tb_put_pixel(bx+5, by+0, col);
    tb_fill_rect(bx+3, by+3, 3, 5, col);
    tb_put_pixel(bx+3, by+8, col);
    tb_put_pixel(bx+4, by+9, col);
    tb_put_pixel(bx+5, by+10, col);

    tb_put_pixel(bx+7, by+3, col);
    tb_put_pixel(bx+8, by+4, col);
    tb_put_pixel(bx+8, by+6, col);
    tb_put_pixel(bx+7, by+7, col);

    tb_put_pixel(bx+10, by+1, col);
    tb_put_pixel(bx+11, by+3, col);
    tb_put_pixel(bx+12, by+5, col);
    tb_put_pixel(bx+11, by+7, col);
    tb_put_pixel(bx+10, by+9, col);
}

/* ── Lock icon ──────────────────────────────────────────────────────────────── */
static void tb_draw_lock(int bx, int by, unsigned int col)
{
    /* Shackle */
    tb_fill_rect(bx + 3, by, 6, 5, col);
    tb_fill_rect(bx + 4, by + 1, 4, 4, C_TRAY_BG);
    /* Body */
    tb_fill_rect(bx + 1, by + 4, 10, 8, col);
    /* Keyhole */
    tb_put_pixel(bx + 5, by + 7, C_TRAY_BG);
    tb_put_pixel(bx + 6, by + 7, C_TRAY_BG);
    tb_put_pixel(bx + 5, by + 8, C_TRAY_BG);
    tb_put_pixel(bx + 6, by + 8, C_TRAY_BG);
    tb_put_pixel(bx + 5, by + 9, C_TRAY_BG);
}

/* Gradient vertical separator */
static void tb_separator(int x)
{
    int y;
    for (y = 8; y < (int)g_h - 8; y++) {
        unsigned int mid = (unsigned int)(g_h / 2);
        unsigned int dist = (unsigned int)(y < (int)mid ? mid - (unsigned int)y : (unsigned int)y - mid);
        unsigned int a = 80 - (dist * 80 / mid);
        unsigned int col = tb_blend(C_BG, C_SEPARATOR, a);
        tb_put_pixel(x, y, col);
    }
}

/* Invalidate */
static void taskbar_invalidate(void)
{
    az_wm_msg_t inv;
    memset(&inv, 0, sizeof(inv));
    inv.type = AZ_WM_INVALIDATE;
    inv.wid  = g_wid;
    az_channel_send(g_srv, (az_ipc_msg_t *)&inv);
}

/* Invalidate just a sub-rectangle of the panel — see draw_clock_only()
 * below, the one place that needs this instead of a full taskbar_invalidate(). */
static void taskbar_invalidate_rect(int x, int y, int w, int h)
{
    az_wm_msg_t inv;
    memset(&inv, 0, sizeof(inv));
    inv.type = AZ_WM_INVALIDATE;
    inv.wid  = g_wid;
    inv.invalidate.x = x;
    inv.invalidate.y = y;
    inv.invalidate.w = (unsigned int)w;
    inv.invalidate.h = (unsigned int)h;
    az_channel_send(g_srv, (az_ipc_msg_t *)&inv);
}

/*
 * draw_clock_only() — the AZ_WM_TIMER_TICK fast path for a tick whose only
 * job is the colon blink (see the handler below): repaints the whole clock
 * text — both digit pairs *and* the colon, not just the colon glyph —
 * because a minute rollover is only ever caught by whichever tick happens
 * to redraw this region, and on an idle panel the colon-blink tick is the
 * only one that ever fires. Publishes just that rect instead of the whole
 * panel.
 *
 * Clears the region to C_TRAY_BG before drawing the glyphs: de_font_draw_char()
 * only ever sets the pixels a glyph's own strokes cover and never clears
 * the ones around them, so drawing "0" straight over a previously-drawn
 * "5" without clearing first would leave whichever of "5"'s strokes "0"
 * doesn't share permanently stuck on screen — exactly the kind of bug this
 * fast path has to not introduce.
 */
static void draw_clock_only(void)
{
    int tray_start_x = (int)g_w - TRAY_W - TRAY_M;

    char clk[6];
    tb_build_clock(clk);
    int clk_len = tb_strlen(clk);
    int clk_x = tray_start_x + TRAY_W - clk_len * 8 - 8;
    int clk_y = SB_Y + 2;

    tb_fill_rect(clk_x, clk_y, clk_len * 8, 16, C_TRAY_BG);
    tb_char(clk_x,      clk_y, clk[0], C_CLOCK);
    tb_char(clk_x + 8,  clk_y, clk[1], C_CLOCK);
    tb_char(clk_x + 16, clk_y, ':',    g_colon_on ? C_CLOCK : tb_blend(C_TRAY_BG, C_CLOCK, 90));
    tb_char(clk_x + 24, clk_y, clk[3], C_CLOCK);
    tb_char(clk_x + 32, clk_y, clk[4], C_CLOCK);

    taskbar_invalidate_rect(clk_x, clk_y, clk_len * 8, 16);
}

/* ============================================================================
 * taskbar_draw — rebuild the panel pixel buffer
 * ============================================================================ */
static void taskbar_draw(void)
{
    unsigned int i;
    if (!g_px) return;

    /* ── Background with subtle top gradient highlight ───────────────────── */
    tb_fill_rect(0, 0, (int)g_w, (int)g_h, C_BG);
    /* Top highlight: 2px gradient fade from C_BG_TOP to C_BG */
    for (i = 0; i < 2; i++) {
        unsigned int a = (i == 0) ? 120 : 60;
        unsigned int col = tb_blend(C_BG, C_BG_TOP, a);
        tb_hline(0, (int)i, (int)g_w, col);
    }

    /* ── Start button (pill-shaped with gradient) ────────────────────────── */
    /* Hover fades smoothly (g_sb_hover_level, animated in the tick handler)
     * rather than snapping between idle/hot, and border alpha rides the
     * same value so it fades in/out with it. */
    unsigned int sb_hover_a = (unsigned int)(g_sb_hover_level * 255 / 256);
    unsigned int sb_bg_l = tb_blend(C_SB_IDLE, C_SB_HOT, sb_hover_a);
    unsigned int sb_bg_r = tb_blend(sb_bg_l, 0xFF000000, 40);
    tb_fill_rounded(SB_X, SB_Y, SB_W, SB_H, 10, sb_bg_l);
    /* Gradient overlay for depth */
    tb_fill_grad_h(SB_X + 10, SB_Y, SB_W - 20, SB_H, sb_bg_l, sb_bg_r);

    /* Mauve border, fading in with hover */
    if (sb_hover_a > 0) {
        int bx, by;
        for (bx = SB_X + 2; bx < SB_X + SB_W - 2; bx++) {
            tb_put_pixel(bx, SB_Y,          tb_blend(sb_bg_l, C_SB_BORDER, sb_hover_a));
            tb_put_pixel(bx, SB_Y + SB_H - 1, tb_blend(sb_bg_l, C_SB_BORDER, sb_hover_a));
        }
        for (by = SB_Y + 2; by < SB_Y + SB_H - 2; by++) {
            tb_put_pixel(SB_X,          by, tb_blend(sb_bg_l, C_SB_BORDER, sb_hover_a));
            tb_put_pixel(SB_X + SB_W - 1, by, tb_blend(sb_bg_l, C_SB_BORDER, sb_hover_a));
        }
    }

    /* Click flash: a brief white overlay that fades out over SB_PRESS_TICKS
     * ticks, giving the Start button tactile feedback on click. */
    if (g_sb_press_anim > 0) {
        unsigned int flash_a = (unsigned int)(g_sb_press_anim * 130 / SB_PRESS_TICKS);
        int fx, fy;
        for (fy = SB_Y; fy < SB_Y + SB_H; fy++) {
            if (fy < 0 || (unsigned int)fy >= g_h) continue;
            for (fx = SB_X; fx < SB_X + SB_W; fx++) {
                if (fx < 0 || (unsigned int)fx >= g_w) continue;
                unsigned int idx = (unsigned int)fy * g_w + (unsigned int)fx;
                g_px[idx] = tb_blend(g_px[idx], 0xFFFFFFFF, flash_a);
            }
        }
    }

    /* ⊞ grid icon (4 small squares) */
    int ib = SB_X + 8;
    int iy_top = SB_Y + (SB_H - 14) / 2;
    tb_fill_rect(ib,     iy_top,     5, 5, C_SB_ICON);
    tb_fill_rect(ib + 7, iy_top,     5, 5, C_SB_ICON);
    tb_fill_rect(ib,     iy_top + 7, 5, 5, C_SB_ICON);
    tb_fill_rect(ib + 7, iy_top + 7, 5, 5, C_SB_ICON);

    /* "Apps" label */
    tb_str(SB_X + 26, SB_Y + (SB_H - 16) / 2, "Apps", C_SB_TEXT);

    /* Separator after start button */
    tb_separator(SB_X + SB_W + 6);

    /* ── Quick Launch Dock ─────────────────────────────────────────────────── */
    for (int i = 0; i < g_num_dock_apps; i++) {
        int dx = DOCK_X + i * (DOCK_BTN_W + DOCK_GAP);
        int dy = SB_Y + 2;

        /* Eased blend toward the hot colour, same curve as the Start
         * button, instead of an instant flat-colour swap. */
        unsigned int hover_a = (unsigned int)(g_dock_hover_level[i] * 255 / 256);
        unsigned int bg = tb_blend(C_WB_BG, C_WB_ACTIVE, hover_a);
        tb_fill_rounded(dx, dy, DOCK_BTN_W, DOCK_BTN_H, 6, bg);

        const dock_app_t *e = &g_dock_apps[i];
        if (e->has_icon) {
            tb_draw_icon32(dx + (DOCK_BTN_W - TB_ICON_DIM) / 2, dy + 1, e->icon);
        } else {
            /* No shipped icon file for this app — fall back to its
             * single-letter glyph in its assigned colour. */
            char glyph_s[2] = { e->glyph, '\0' };
            tb_str(dx + (DOCK_BTN_W - 8) / 2, dy + (DOCK_BTN_H - 16) / 2, glyph_s, e->color);
        }

        /* Active-app indicator: a thin accent bar under the icon that grows
         * in from the centre as the hover level rises — the same "something
         * is alive here" cue the Start button's border fade gives it. */
        if (hover_a > 0) {
            int full_w = DOCK_BTN_W - 10;
            int bar_w  = (full_w * (int)hover_a) / 255;
            if (bar_w > 0) {
                int bar_x = dx + (DOCK_BTN_W - bar_w) / 2;
                tb_fill_rect(bar_x, dy + DOCK_BTN_H - 3, bar_w, 2,
                            tb_blend(bg, e->color, hover_a));
            }
        }

        /* Click flash: brief white overlay fading out over SB_PRESS_TICKS
         * ticks, matching the Start button's tactile click feedback. */
        if (g_dock_press_anim[i] > 0) {
            unsigned int flash_a = (unsigned int)(g_dock_press_anim[i] * 130 / SB_PRESS_TICKS);
            for (int fy = dy; fy < dy + DOCK_BTN_H; fy++) {
                if (fy < 0 || (unsigned int)fy >= g_h) continue;
                for (int fx = dx; fx < dx + DOCK_BTN_W; fx++) {
                    if (fx < 0 || (unsigned int)fx >= g_w) continue;
                    unsigned int idx = (unsigned int)fy * g_w + (unsigned int)fx;
                    g_px[idx] = tb_blend(g_px[idx], 0xFFFFFFFF, flash_a);
                }
            }
        }
    }

    /* ── Window button strip (pill-shaped buttons) ───────────────────────── */
    int tray_start_x = (int)g_w - TRAY_W - TRAY_M;
    int strip_end_x  = tray_start_x - 8;
    int bx = tb_wb_origin();
    int visible = 0;
    g_overflow = 0;

    for (i = 0; i < DE_TASKBAR_MAX_WINDOWS && visible < WB_MAX; i++) {
        if (!g_wins[i].active) continue;
        if (bx + WB_W > strip_end_x) {
            g_overflow = 1;
            break;
        }

        unsigned char is_foc = (g_wins[i].wid == g_focus_wid);
        unsigned int bg  = is_foc ? C_WB_ACTIVE : C_WB_BG;
        unsigned int fg  = is_foc ? C_WB_TXT_ACT : C_WB_TXT_IDL;

        /* Pill background */
        tb_fill_rounded(bx, SB_Y, WB_W, WB_H, WB_RADIUS, bg);

        /* Active window: mauve glow tint + accent underline */
        if (is_foc) {
            /* Subtle glow tint over button */
            int gx, gy;
            for (gy = SB_Y; gy < SB_Y + WB_H; gy++) {
                for (gx = bx; gx < bx + WB_W; gx++) {
                    int px = gx, py = gy;
                    if (px < 0 || (unsigned int)px >= g_w) continue;
                    if (py < 0 || (unsigned int)py >= g_h) continue;
                    g_px[(unsigned int)py * g_w + (unsigned int)px] =
                        tb_blend(g_px[(unsigned int)py * g_w + (unsigned int)px],
                                 C_WB_GLOW, 60);
                }
            }
            /* Left mauve pill bar */
            tb_fill_rounded(bx, SB_Y + 6, 3, WB_H - 12, 1, C_WB_ACCENT);
            /* Bottom accent line */
            int ay;
            for (ay = bx + WB_RADIUS; ay < bx + WB_W - WB_RADIUS; ay++)
                tb_put_pixel(ay, SB_Y + WB_H - 2, C_WB_ACCENT);
        }

        /* Title text (clipped to pill interior, leaving room for left bar) */
        int text_x = bx + (is_foc ? 9 : 6);
        int text_y = SB_Y + (WB_H - 16) / 2;
        int max_px = WB_W - 12;
        tb_str_clip(text_x, text_y, g_wins[i].title, fg, max_px);

        bx += WB_W + WB_GAP;
        visible++;
    }

    /* Overflow indicator */
    if (g_overflow) {
        int rem = 0;
        for (i = 0; i < DE_TASKBAR_MAX_WINDOWS; i++)
            if (g_wins[i].active) rem++;
        rem -= visible;
        if (bx + 32 < strip_end_x) {
            char ovf[8] = {'+', '0', '\0', '\0', '\0', '\0', '\0', '\0'};
            if (rem < 10) {
                ovf[1] = '0' + (char)rem;
            } else {
                ovf[0] = '+'; ovf[1] = '0' + (char)(rem/10);
                ovf[2] = '0' + (char)(rem%10); ovf[3] = '\0';
            }
            tb_fill_rounded(bx, SB_Y, 32, WB_H, WB_RADIUS, C_WB_BG);
            tb_str(bx + 4, SB_Y + (WB_H - 16) / 2, ovf, C_OVERFLOW);
        }
    }

    /* Separator before tray */
    tb_separator(tray_start_x - 3);

    /* ── System tray ──────────────────────────────────────────────────────── */
    tb_fill_rect(tray_start_x, 0, TRAY_W, (int)g_h, C_TRAY_BG);

    /* WiFi icon */
    tb_draw_wifi(tray_start_x + 8, SB_Y + 12, C_WIFI);

    /* Sound / Volume icon */
    unsigned int snd_col = g_vol_muted ? 0xFFF38BA8 : 0xFFF9E2AF;
    tb_draw_sound(tray_start_x + 24, SB_Y + 12, snd_col);

    /* Mini Volume Level Bar (3 segment bars) */
    if (!g_vol_muted) {
        int bars = (g_vol_level * 3) / 100;
        if (bars < 1 && g_vol_level > 0) bars = 1;
        if (bars >= 1) tb_fill_rect(tray_start_x + 39, SB_Y + 18, 2, 4, 0xFFA6E3A1);
        if (bars >= 2) tb_fill_rect(tray_start_x + 42, SB_Y + 15, 2, 7, 0xFFA6E3A1);
        if (bars >= 3) tb_fill_rect(tray_start_x + 45, SB_Y + 12, 2, 10, 0xFFA6E3A1);
    } else {
        /* X mark on sound icon */
        tb_put_pixel(tray_start_x + 39, SB_Y + 14, 0xFFF38BA8);
        tb_put_pixel(tray_start_x + 41, SB_Y + 16, 0xFFF38BA8);
        tb_put_pixel(tray_start_x + 39, SB_Y + 16, 0xFFF38BA8);
        tb_put_pixel(tray_start_x + 41, SB_Y + 14, 0xFFF38BA8);
    }

    /* Lock icon */
    tb_draw_lock(tray_start_x + 52, SB_Y + 12, 0xFFCBA6F7);

    /* Clock text "HH:MM" (large, right-aligned). The ":" is drawn separately
     * so it can blink once a second like a real clock, without shifting the
     * digits around it. */
    char clk[6];
    tb_build_clock(clk);
    int clk_len = tb_strlen(clk);
    int clk_x = tray_start_x + TRAY_W - clk_len * 8 - 8;
    int clk_y = SB_Y + 2;
    tb_char(clk_x,      clk_y, clk[0], C_CLOCK);
    tb_char(clk_x + 8,  clk_y, clk[1], C_CLOCK);
    tb_char(clk_x + 16, clk_y, ':',    g_colon_on ? C_CLOCK : tb_blend(C_TRAY_BG, C_CLOCK, 90));
    tb_char(clk_x + 24, clk_y, clk[3], C_CLOCK);
    tb_char(clk_x + 32, clk_y, clk[4], C_CLOCK);

    /* Date string "Mon Jan 14" (small, below clock) */
    char date_buf[20];
    tb_build_date(date_buf, 20);
    int date_len = tb_strlen(date_buf);
    int date_x = tray_start_x + TRAY_W - date_len * 8 - 8;
    int date_y = clk_y + 18;
    if (date_y + 16 < (int)g_h)
        tb_str(date_x, date_y, date_buf, C_DATE);

    /* Toast notification / overlay (Volume, WiFi, etc.) */
    if (g_toast_ticks > 0 && g_toast_msg[0]) {
        int tlen = tb_strlen(g_toast_msg);
        int tw = tlen * 8 + 24;
        int tx = tray_start_x - tw - 12;
        if (tx < (int)g_w / 2) tx = (int)g_w / 2;
        int ty = 8;
        int th = 36;
        tb_fill_rounded(tx, ty, tw, th, 8, 0xFF181825);
        tb_fill_rect(tx + 2, ty + 2, 3, th - 4, 0xFFCBA6F7);
        tb_str(tx + 12, ty + 10, g_toast_msg, 0xFFCDD6F4);
    } else if (g_hover_tooltip[0] != '\0') {
        int tlen = tb_strlen(g_hover_tooltip);
        int tw = tlen * 8 + 16;
        int tx = g_hover_x - tw / 2;
        if (tx < 10) tx = 10;
        if (tx + tw > (int)g_w - 10) tx = (int)g_w - tw - 10;
        int ty = 10;
        int th = 32;
        tb_fill_rounded(tx, ty, tw, th, 6, 0xFF1E1E2E);
        tb_str(tx + 8, ty + 8, g_hover_tooltip, 0xFFBAC2DE);
    }

    taskbar_invalidate();
}

/* ============================================================================
 * Window list helpers
 * ============================================================================ */
static void tb_add_win(unsigned int wid, unsigned int owner_pid, const char *title)
{
    unsigned int i;
    (void)owner_pid;
    if (!title || title[0] == '\0') return;
    for (i = 0; i < DE_TASKBAR_MAX_WINDOWS; i++)
        if (g_wins[i].active && g_wins[i].wid == wid) return;
    for (i = 0; i < DE_TASKBAR_MAX_WINDOWS; i++) {
        if (!g_wins[i].active) {
            g_wins[i].wid    = wid;
            g_wins[i].active = 1;
            g_wins[i].focused= 0;
            unsigned int j;
            for (j = 0; j < 63 && title[j]; j++) g_wins[i].title[j] = title[j];
            g_wins[i].title[j] = '\0';
            g_win_count++;
            return;
        }
    }
}

static void tb_del_win(unsigned int wid)
{
    unsigned int i;
    for (i = 0; i < DE_TASKBAR_MAX_WINDOWS; i++) {
        if (g_wins[i].active && g_wins[i].wid == wid) {
            g_wins[i].active = 0;
            g_wins[i].wid    = 0;
            if (g_win_count > 0) g_win_count--;
            if (g_focus_wid == wid) g_focus_wid = 0;
            return;
        }
    }
}

/* ============================================================================
 * Mouse event handler
 * ============================================================================ */
static unsigned char g_prev_tb_btns = 0;

static void tb_handle_mouse(short abs_x, short abs_y, unsigned char btns)
{
    int lx = (int)abs_x;
    int ly = (int)abs_y;
    unsigned char lclick_down = (btns & AZ_MOUSE_BTN_LEFT) != 0;
    unsigned char lclick = lclick_down && !(g_prev_tb_btns & AZ_MOUSE_BTN_LEFT);
    g_prev_tb_btns = btns;

    /* Start button hover */
    unsigned char was_hot = g_sb_hot;
    g_sb_hot = (lx >= SB_X && lx < SB_X + SB_W && ly >= SB_Y && ly < SB_Y + SB_H) ? 1 : 0;
    if (g_sb_hot != was_hot) { taskbar_draw(); }

    /* Quick Launch Dock hover */
    int dock_hit = -1;
    for (int i = 0; i < g_num_dock_apps; i++) {
        int btn_x = DOCK_X + i * (DOCK_BTN_W + DOCK_GAP);
        int btn_y = SB_Y + 2;
        if (lx >= btn_x && lx < btn_x + DOCK_BTN_W && ly >= btn_y && ly < btn_y + DOCK_BTN_H) {
            dock_hit = i;
            break;
        }
    }
    if (dock_hit != g_dock_hovered) { g_dock_hovered = dock_hit; taskbar_draw(); }

    int tray_start_x = (int)g_w - TRAY_W - TRAY_M;

    /* Tray & Dock tooltips on hover */
    const char *new_tooltip = "";
    int hover_x = lx;
    if (g_sb_hot) {
        new_tooltip = "Applications";
        hover_x = SB_X + SB_W / 2;
    } else if (dock_hit >= 0 && dock_hit < g_num_dock_apps) {
        new_tooltip = g_dock_apps[dock_hit].label;
        hover_x = DOCK_X + dock_hit * (DOCK_BTN_W + DOCK_GAP) + DOCK_BTN_W / 2;
    } else if (lx >= tray_start_x && lx < (int)g_w) {
        if (lx < tray_start_x + 20) {
            new_tooltip = "Network (eth0)";
            hover_x = tray_start_x + 10;
        } else if (lx < tray_start_x + 50) {
            new_tooltip = g_vol_muted ? "Volume: Muted" : "Volume Control";
            hover_x = tray_start_x + 35;
        } else if (lx < tray_start_x + 68) {
            new_tooltip = "Lock Screen";
            hover_x = tray_start_x + 58;
        } else {
            new_tooltip = "Clock & Calendar";
            hover_x = tray_start_x + 120;
        }
    }
    if (strcmp(g_hover_tooltip, new_tooltip) != 0) {
        strncpy(g_hover_tooltip, new_tooltip, sizeof(g_hover_tooltip) - 1);
        g_hover_tooltip[sizeof(g_hover_tooltip) - 1] = '\0';
        g_hover_x = hover_x;
        taskbar_draw();
    }

    /* Left-click on Start → toggle launcher */
    if (lclick && g_sb_hot) {
        g_sb_press_anim = SB_PRESS_TICKS;
        if (g_launcher_wid == 0) {
            az_wm_msg_t lmsg;
            memset(&lmsg, 0, sizeof(lmsg));
            lmsg.type = AZ_WM_LAUNCH_APP;
            az_wm_launch_payload_t *pl = AZ_WM_MSG_LAUNCH(&lmsg);
            const char *path = "/bin/launcher.elf";
            unsigned int j;
            for (j = 0; j < AZ_WM_LAUNCH_PATH_MAX - 1 && path[j]; j++)
                pl->path[j] = path[j];
            pl->path[j] = '\0';
            az_channel_send(g_srv, (az_ipc_msg_t *)&lmsg);
        } else {
            az_wm_msg_t cmsg;
            memset(&cmsg, 0, sizeof(cmsg));
            cmsg.type = AZ_WM_DESTROY_WINDOW;
            cmsg.wid = g_launcher_wid;
            az_channel_send(g_srv, (az_ipc_msg_t *)&cmsg);
        }
        taskbar_draw();
        return;
    }

    /* Left-click on Quick Launch Dock icons */
    if (lclick) {
        for (int i = 0; i < g_num_dock_apps; i++) {
            int btn_x = DOCK_X + i * (DOCK_BTN_W + DOCK_GAP);
            int btn_y = SB_Y + 2;
            if (lx >= btn_x && lx < btn_x + DOCK_BTN_W && ly >= btn_y && ly < btn_y + DOCK_BTN_H) {
                g_dock_press_anim[i] = SB_PRESS_TICKS;
                az_wm_msg_t lmsg;
                memset(&lmsg, 0, sizeof(lmsg));
                lmsg.type = AZ_WM_LAUNCH_APP;
                az_wm_launch_payload_t *pl = AZ_WM_MSG_LAUNCH(&lmsg);
                const char *path = g_dock_apps[i].path;
                unsigned int j;
                for (j = 0; j < AZ_WM_LAUNCH_PATH_MAX - 1 && path[j]; j++)
                    pl->path[j] = path[j];
                pl->path[j] = '\0';
                az_channel_send(g_srv, (az_ipc_msg_t *)&lmsg);
                taskbar_draw();
                return;
            }
        }
    }

    /* Left-click on Tray WiFi icon → launch Settings (Network) */
    if (lclick && lx >= tray_start_x && lx < tray_start_x + 20) {
        az_wm_msg_t lmsg;
        memset(&lmsg, 0, sizeof(lmsg));
        lmsg.type = AZ_WM_LAUNCH_APP;
        az_wm_launch_payload_t *pl = AZ_WM_MSG_LAUNCH(&lmsg);
        const char *path = "/bin/settings.elf";
        unsigned int j;
        for (j = 0; j < AZ_WM_LAUNCH_PATH_MAX - 1 && path[j]; j++)
            pl->path[j] = path[j];
        pl->path[j] = '\0';
        az_channel_send(g_srv, (az_ipc_msg_t *)&lmsg);
        tb_show_toast("Network: eth0 (10.0.2.15)");
        taskbar_draw();
        return;
    }

    /* Left-click on Tray Sound / Volume area → cycle volume / toggle mute */
    if (lclick && lx >= tray_start_x + 20 && lx < tray_start_x + 50) {
        if (g_vol_muted) {
            g_vol_muted = 0;
            char msg[32];
            snprintf(msg, sizeof(msg), "Volume: %d%%", g_vol_level);
            tb_show_toast(msg);
        } else if (g_vol_level >= 100) {
            g_vol_muted = 1;
            g_vol_level = 0;
            tb_show_toast("Audio Muted");
        } else {
            g_vol_level = (g_vol_level + 25);
            if (g_vol_level > 100) g_vol_level = 100;
            char msg[32];
            snprintf(msg, sizeof(msg), "Volume: %d%%", g_vol_level);
            tb_show_toast(msg);
        }
        taskbar_draw();
        return;
    }

    /* Left-click on Lock icon → launch /sbin/lockscreen.elf */
    if (lclick && lx >= tray_start_x + 48 && lx < tray_start_x + 68) {
        az_wm_msg_t lmsg;
        memset(&lmsg, 0, sizeof(lmsg));
        lmsg.type = AZ_WM_LAUNCH_APP;
        az_wm_launch_payload_t *pl = AZ_WM_MSG_LAUNCH(&lmsg);
        const char *path = "/sbin/lockscreen.elf";
        unsigned int j;
        for (j = 0; j < AZ_WM_LAUNCH_PATH_MAX - 1 && path[j]; j++)
            pl->path[j] = path[j];
        pl->path[j] = '\0';
        az_channel_send(g_srv, (az_ipc_msg_t *)&lmsg);
        taskbar_draw();
        return;
    }

    /* Left-click on Tray Clock / Calendar area → launch Clock & Calendar widget */
    if (lclick && lx >= tray_start_x + 68 && lx < (int)g_w) {
        az_wm_msg_t lmsg;
        memset(&lmsg, 0, sizeof(lmsg));
        lmsg.type = AZ_WM_LAUNCH_APP;
        az_wm_launch_payload_t *pl = AZ_WM_MSG_LAUNCH(&lmsg);
        const char *path = "/bin/clock.elf";
        unsigned int j;
        for (j = 0; j < AZ_WM_LAUNCH_PATH_MAX - 1 && path[j]; j++)
            pl->path[j] = path[j];
        pl->path[j] = '\0';
        az_channel_send(g_srv, (az_ipc_msg_t *)&lmsg);
        taskbar_draw();
        return;
    }

    /* Left-click on a window button */
    if (lclick) {
        int wbx = tb_wb_origin();
        int tray_end = (int)g_w - TRAY_W - TRAY_M;
        unsigned int i;
        for (i = 0; i < DE_TASKBAR_MAX_WINDOWS; i++) {
            if (!g_wins[i].active) continue;
            if (wbx + WB_W > tray_end) break;
            if (lx >= wbx && lx < wbx + WB_W && ly >= SB_Y && ly < SB_Y + WB_H) {
                if (g_focus_wid == g_wins[i].wid) {
                    az_wm_msg_t min_msg;
                    memset(&min_msg, 0, sizeof(min_msg));
                    min_msg.type = AZ_WM_MINIMIZE_WINDOW;
                    min_msg.wid = g_wins[i].wid;
                    az_channel_send(g_srv, (az_ipc_msg_t *)&min_msg);
                } else {
                    g_focus_wid = g_wins[i].wid;
                    az_wm_msg_t rest_msg;
                    memset(&rest_msg, 0, sizeof(rest_msg));
                    rest_msg.type = AZ_WM_RESTORE_WINDOW;
                    rest_msg.wid = g_wins[i].wid;
                    az_channel_send(g_srv, (az_ipc_msg_t *)&rest_msg);
                }
                taskbar_draw();
                return;
            }
            wbx += WB_W + WB_GAP;
        }
    }
}

/* ============================================================================
 * _start
 * ============================================================================ */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    unsigned int i;
    de_log("[taskbar] v3.0 — panel starting");


    /* Zero window table */
    for (i = 0; i < DE_TASKBAR_MAX_WINDOWS; i++) {
        g_wins[i].active = 0; g_wins[i].wid = 0;
    }

    tb_init_dock();

    /* ── Screen geometry ──────────────────────────────────────────────────── */
    az_fb_info_t fb;
    unsigned int sw = DEFAULT_WIDTH, sh = DEFAULT_HEIGHT;
    if (az_fb_info(&fb) == 0) {
        if (fb.width  > 0) sw = fb.width;
        if (fb.height > 0) sh = fb.height;
    }
    g_w = sw;
    g_sh = sh;

    /* ── Create reply channel ─────────────────────────────────────────────── */
    g_cli = az_channel_create();
    if (g_cli < 0) { de_log("[taskbar] FATAL: channel_create"); return -1; }

    /* ── Request panel window ─────────────────────────────────────────────── */
    az_wm_msg_t req;
    memset(&req, 0, sizeof(req));
    req.type           = AZ_WM_CREATE_WINDOW;
    req.client_chan     = (unsigned int)g_cli;
    req.create.x       = 0;
    req.create.y       = (int)(sh - TASKBAR_H);
    req.create.w       = sw;
    req.create.h       = TASKBAR_H;
    req.create.title[0]= '\0';

    if (az_channel_send(g_srv, (az_ipc_msg_t *)&req) < 0) {
        de_log("[taskbar] FATAL: channel_send"); return -1;
    }

    /* ── Wait for window creation ack ─────────────────────────────────────── */
    az_wm_msg_t resp;
    for (;;) {
        int r = az_channel_recv(g_cli, (az_ipc_msg_t *)&resp);
        if (r < 0) { de_log("[taskbar] FATAL: recv failed"); return -1; }
        if (resp.type == AZ_WM_WINDOW_CREATED) {
            g_wid = resp.created.assigned_wid;
            break;
        }
    }

    if (az_shmem_map((int)resp.created.shmem_id, TASKBAR_MAP_ADDR) < 0) {
        de_log("[taskbar] FATAL: shmem_map"); return -1;
    }
    g_px = (unsigned int *)TASKBAR_MAP_ADDR;

    /* ── Register: z-order TOP, strut bottom, broadcast subscriber ─────────── */
    az_wm_msg_t zmsg;
    memset(&zmsg, 0, sizeof(zmsg));
    zmsg.type = AZ_WM_SET_ZORDER_HINT;
    az_wm_zorder_payload_t *zpl = AZ_WM_MSG_ZORDER(&zmsg);
    zpl->wid = g_wid; zpl->band = AZ_WM_ZORDER_TOP;
    az_channel_send(g_srv, (az_ipc_msg_t *)&zmsg);

    az_wm_msg_t smsg;
    memset(&smsg, 0, sizeof(smsg));
    smsg.type = AZ_WM_SET_STRUT;
    az_wm_strut_payload_t *spl = AZ_WM_MSG_STRUT(&smsg);
    spl->wid = g_wid; spl->bottom = TASKBAR_H;
    az_channel_send(g_srv, (az_ipc_msg_t *)&smsg);

    az_wm_msg_t sub;
    memset(&sub, 0, sizeof(sub));
    sub.type = AZ_WM_SUBSCRIBE_EVENTS;
    az_wm_subscribe_payload_t *subpl = AZ_WM_MSG_SUBSCRIBE(&sub);
    subpl->subscriber_chan = (unsigned int)g_cli;
    az_channel_send(g_srv, (az_ipc_msg_t *)&sub);

    /* ── Register a 100ms timer: drives the once-a-second clock (Bug 3 fix)
     * as well as the colon blink and Start-button hover/press animations.
     * The clock itself only actually changes once a second — reformatting
     * it 10x more often than that costs nothing since it's a cheap
     * localtime_r() + snprintf() over a handful of bytes. */
    az_set_timer(g_cli, 100, 0);

    taskbar_draw();
    de_log("[taskbar] Entering event loop.");

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_cli, (az_ipc_msg_t *)&msg);
        if (r < 0) {
            de_log("[taskbar] IPC channel disconnected, exiting.");
            break;
        }

        switch (msg.type) {

        case AZ_WM_MOUSE_EVENT:
            tb_handle_mouse(msg.mouse.abs_x, msg.mouse.abs_y, msg.mouse.buttons);
            break;

        case AZ_WM_TIMER_TICK: {
            /* This 100ms tick used to call taskbar_draw() — a full repaint
             * of the whole bar: dock, systray, clock, start button — on
             * every single firing, even the (overwhelming, on an idle
             * desktop with the pointer elsewhere) majority where nothing
             * animating actually moved and the colon didn't blink. `colon_dirty`
             * and `other_dirty` are tracked separately (instead of one
             * combined flag) so the very common "only the colon blinked"
             * tick can take draw_clock_only()'s narrow repaint below
             * instead of a full taskbar_draw() — anything else changing
             * still falls back to the full repaint. Each level's "did this
             * actually change" check runs against its pre-step value,
             * mirroring launcher's hover-fade gate, so the tick that lands
             * a fade exactly on its target still counts as dirty and
             * paints that settled frame (see the compositor's identical
             * fix for what goes wrong if it didn't). */
            g_tick_count++;
            bool colon_dirty = (g_tick_count % 5 == 0); /* colon blink, 500ms */
            if (colon_dirty) g_colon_on = !g_colon_on;

            bool other_dirty = false;

            int sb_target = g_sb_hot ? 256 : 0;
            if (g_sb_hover_level != sb_target) {
                other_dirty = true;
                if (g_sb_hover_level < sb_target) {
                    g_sb_hover_level += SB_HOVER_STEP;
                    if (g_sb_hover_level > sb_target) g_sb_hover_level = sb_target;
                } else {
                    g_sb_hover_level -= SB_HOVER_STEP;
                    if (g_sb_hover_level < sb_target) g_sb_hover_level = sb_target;
                }
            }

            if (g_sb_press_anim > 0) { other_dirty = true; g_sb_press_anim--; }

            for (int i = 0; i < g_num_dock_apps; i++) {
                int dock_target = (i == g_dock_hovered) ? 256 : 0;
                if (g_dock_hover_level[i] != dock_target) {
                    other_dirty = true;
                    if (g_dock_hover_level[i] < dock_target) {
                        g_dock_hover_level[i] += SB_HOVER_STEP;
                        if (g_dock_hover_level[i] > dock_target) g_dock_hover_level[i] = dock_target;
                    } else {
                        g_dock_hover_level[i] -= SB_HOVER_STEP;
                        if (g_dock_hover_level[i] < dock_target) g_dock_hover_level[i] = dock_target;
                    }
                }
                if (g_dock_press_anim[i] > 0) { other_dirty = true; g_dock_press_anim[i]--; }
            }

            if (g_toast_ticks > 0) { other_dirty = true; g_toast_ticks--; }

            if (other_dirty) {
                taskbar_draw();
            } else if (colon_dirty) {
                draw_clock_only();
            }
            break;
        }

        case AZ_WM_EVT_WINDOW_CREATED: {
            az_wm_evt_created_payload_t *p = AZ_WM_MSG_EVT_CREATED(&msg);
            if (p->title[0] != '\0') {
                /* Check for launcher title */
                const char *lname = "AzamiOS App Launcher";
                int match = 1;
                for (int k = 0; lname[k]; k++) {
                    if (p->title[k] != lname[k]) { match = 0; break; }
                }
                if (match) {
                    g_launcher_wid = p->wid;
                } else if (p->wid != g_wid) {
                    tb_add_win(p->wid, p->owner_pid, p->title);
                }
            }
            taskbar_draw();
            break;
        }

        case AZ_WM_EVT_WINDOW_DESTROYED: {
            az_wm_evt_destroyed_payload_t *p = AZ_WM_MSG_EVT_DESTROYED(&msg);
            if (p->wid == g_launcher_wid) {
                g_launcher_wid = 0;
            } else {
                tb_del_win(p->wid);
            }
            taskbar_draw();
            break;
        }

        case AZ_WM_EVT_FOCUS_CHANGED: {
            az_wm_evt_focus_payload_t *p = AZ_WM_MSG_EVT_FOCUS(&msg);
            g_focus_wid = p->new_wid;
            taskbar_draw();
            break;
        }

        case AZ_WM_EVT_WINDOW_TITLE_CHANGED: {
            az_wm_evt_title_payload_t *p = AZ_WM_MSG_EVT_TITLE(&msg);
            for (unsigned int i = 0; i < DE_TASKBAR_MAX_WINDOWS; i++) {
                if (g_wins[i].active && g_wins[i].wid == p->wid) {
                    unsigned int j;
                    for (j = 0; j < 63 && p->title[j]; j++) {
                        g_wins[i].title[j] = p->title[j];
                    }
                    g_wins[i].title[j] = '\0';
                    taskbar_draw();
                    break;
                }
            }
            break;
        }

        case AZ_WM_FOCUS_CHANGE:
            /* Panel never needs focus */
            break;

        default:
            break;
        }
    }

    sys_exit(0);
}
