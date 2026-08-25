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

/* Tray zone width (clock + date + wifi icon, right-aligned) */
#define TRAY_W  160
#define TRAY_M    8   /* tray right margin */

/* Window button geometry (pill-shaped) */
#define WB_W       140
#define WB_H        40
#define WB_GAP       5
#define WB_MAX        12  /* maximum visible window buttons before overflow */
#define WB_RADIUS     8   /* pill corner radius */

/* Quick Launch Dock */
typedef struct {
    char glyph;
    const char *label;
    const char *path;
    unsigned int color;
} dock_app_t;

static const dock_app_t g_dock_apps[] = {
    { 'T', "Terminal",    "/bin/terminal.elf",    0xFFA6E3A1 }, /* Green */
    { 'F', "Files",       "/bin/filemanager.elf", 0xFFF9E2AF }, /* Yellow */
    { 'E', "Editor",      "/bin/texteditor.elf",  0xFF89B4FA }, /* Blue */
    { 'C', "Calc",        "/bin/calculator.elf",  0xFFFAB387 }, /* Peach */
    { 'P', "Paint",       "/bin/paint.elf",       0xFFCBA6F7 }, /* Mauve */
    { 'A', "Audio",       "/bin/audioplayer.elf", 0xFFF38BA8 }, /* Flamingo/Pink */
    { 'S', "Settings",    "/bin/settings.elf",    0xFF74C7EC }, /* Sapphire */
    { 'M', "Sysmon",      "/bin/sysmon.elf",      0xFF94E2D5 }, /* Teal */
    { 'X', "XClock",      "/bin/xclock.elf",      0xFF89DCEB }, /* Sky */
    { 'Y', "XEyes",       "/bin/xeyes.elf",       0xFFF5C2E7 }, /* Pink */
    { 'K', "XCalc",       "/bin/xcalc.elf",       0xFFF9E2AF }, /* Yellow */
    { 'G', "XDemo",       "/bin/xgui_demo.elf",   0xFFB4BEFE }, /* Lavender */
};
#define NUM_DOCK_APPS ((int)(sizeof(g_dock_apps) / sizeof(g_dock_apps[0])))
#define DOCK_X        (SB_X + SB_W + 12)
#define DOCK_BTN_W    30
#define DOCK_BTN_H    36
#define DOCK_GAP      4
#define WB_ORIGIN     (DOCK_X + NUM_DOCK_APPS * (DOCK_BTN_W + DOCK_GAP) + 8)

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
    unsigned int sb_bg_l = g_sb_hot ? C_SB_HOT : C_SB_IDLE;
    unsigned int sb_bg_r = tb_blend(sb_bg_l, 0xFF000000, 40);
    tb_fill_rounded(SB_X, SB_Y, SB_W, SB_H, 10, sb_bg_l);
    /* Gradient overlay for depth */
    tb_fill_grad_h(SB_X + 10, SB_Y, SB_W - 20, SB_H, sb_bg_l, sb_bg_r);

    /* Mauve border on hover */
    if (g_sb_hot) {
        /* Top+bottom border pixels of pill */
        int bx, by;
        for (bx = SB_X + 2; bx < SB_X + SB_W - 2; bx++) {
            tb_put_pixel(bx, SB_Y,          C_SB_BORDER);
            tb_put_pixel(bx, SB_Y + SB_H - 1, C_SB_BORDER);
        }
        for (by = SB_Y + 2; by < SB_Y + SB_H - 2; by++) {
            tb_put_pixel(SB_X,          by, C_SB_BORDER);
            tb_put_pixel(SB_X + SB_W - 1, by, C_SB_BORDER);
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

    /* ── Window button strip (pill-shaped buttons) ───────────────────────── */
    int tray_start_x = (int)g_w - TRAY_W - TRAY_M;
    int strip_end_x  = tray_start_x - 8;
    int bx = WB_ORIGIN;
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

    /* Clock text "HH:MM" (large, right-aligned) */
    char clk[6];
    tb_build_clock(clk);
    int clk_len = tb_strlen(clk);
    int clk_x = tray_start_x + TRAY_W - clk_len * 8 - 8;
    int clk_y = SB_Y + 2;
    tb_str(clk_x, clk_y, clk, C_CLOCK);

    /* Date string "Mon Jan 14" (small, below clock) */
    char date_buf[20];
    tb_build_date(date_buf, 20);
    int date_len = tb_strlen(date_buf);
    int date_x = tray_start_x + TRAY_W - date_len * 8 - 8;
    int date_y = clk_y + 18;
    if (date_y + 16 < (int)g_h)
        tb_str(date_x, date_y, date_buf, C_DATE);

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

    /* Left-click on Start → toggle launcher */
    if (lclick && g_sb_hot) {
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
        for (int i = 0; i < NUM_DOCK_APPS; i++) {
            int btn_x = DOCK_X + i * (DOCK_BTN_W + DOCK_GAP);
            int btn_y = SB_Y + 2;
            if (lx >= btn_x && lx < btn_x + DOCK_BTN_W && ly >= btn_y && ly < btn_y + DOCK_BTN_H) {
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

    /* Left-click on Tray Sound / Volume area → cycle volume / toggle mute */
    int tray_start_x = (int)g_w - TRAY_W - TRAY_M;
    if (lclick && lx >= tray_start_x + 20 && lx < tray_start_x + 50) {
        if (g_vol_muted) {
            g_vol_muted = 0;
        } else if (g_vol_level >= 100) {
            g_vol_muted = 1;
            g_vol_level = 0;
        } else {
            g_vol_level = (g_vol_level + 25);
            if (g_vol_level > 100) g_vol_level = 100;
        }
        taskbar_draw();
        return;
    }

    /* Left-click on Tray Clock / Calendar area → launch Clock & Calendar widget */
    if (lclick && lx >= tray_start_x + 50 && lx < (int)g_w) {
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
        int wbx = WB_ORIGIN;
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

    /* ── Register 1-second timer for autonomous clock update (Bug 3 fix) ─── */
    az_set_timer(g_cli, 1000, 0);  /* 1000ms, repeating */

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

        case AZ_WM_TIMER_TICK:
            /* Real-time clock update — fires once per second autonomously */
            taskbar_draw();
            break;

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

        case AZ_WM_FOCUS_CHANGE:
            /* Panel never needs focus */
            break;

        default:
            break;
        }
    }

    sys_exit(0);
}
