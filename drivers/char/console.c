/* ============================================================================
 * AzamiOS — Unified Console Implementation
 * File: drivers/console.c
 * ============================================================================ */

#include "console.h"
#include "uart.h"
#include "../../include/azami/defs.h"
#include "../../kernel/lib/string.h"
#include <stdarg.h>

#include "../../arch/x86_64/cpu/spinlock.h"

/* ── State ────────────────────────────────────────────────────────────────── */
static bool g_uart_ready = false;
static bool g_fb_ready   = false;
static spinlock_t g_console_lock = SPINLOCK_INIT;

/* Framebuffer state */
static u8  *g_fb_base   = NULL;
static u32  g_fb_width  = 0;
static u32  g_fb_height = 0;
static u32  g_fb_pitch  = 0;
static u8   g_fb_bpp    = 32;
static u32  g_fb_col    = 0;   /* current text column (pixels / char width) */
static u32  g_fb_row    = 0;   /* current text row    (pixels / char height) */

#include "console_font.h"

#define FONT_W  8
#define FONT_H  16
#define FG_COLOR 0x00E0E0E0  /* Light grey */
#define BG_COLOR 0x00000000  /* Black */

/*
 * ── Why the console keeps its own copy of the text ──────────────────────────
 * Video memory is mapped write-combining: writes stream out in bursts, reads
 * go straight to the bus one at a time and cost hundreds of times more.  A
 * console that scrolls by memmove()ing the framebuffer over itself therefore
 * reads back the entire screen for every line of output, which at 1280x800 is
 * megabytes of uncached reads per line — slow enough to see as the screen
 * lurching and flashing while the kernel logs.
 *
 * So the character grid lives in ordinary RAM.  Scrolling shifts that (cheap),
 * and the screen is repainted from it with nothing but sequential writes.  The
 * framebuffer is write-only from here on.
 */
#define CON_MAX_COLS 256
#define CON_MAX_ROWS 128

static char g_text[CON_MAX_ROWS][CON_MAX_COLS];
static u32  g_cols = 0;
static u32  g_rows = 0;

/* ── Init ─────────────────────────────────────────────────────────────────── */

void console_init_early(void)
{
    uart_init(UART_COM1);
    g_uart_ready = true;
}

void console_init_fb(void *fb_base, u32 width, u32 height, u32 pitch, u8 bpp)
{
    g_fb_base   = (u8 *)fb_base;
    g_fb_width  = width;
    g_fb_height = height;
    g_fb_pitch  = pitch;
    g_fb_bpp    = bpp;
    g_fb_col    = 0;
    g_fb_row    = 0;

    g_cols = width  / FONT_W;
    g_rows = height / FONT_H;
    if (g_cols > CON_MAX_COLS) g_cols = CON_MAX_COLS;
    if (g_rows > CON_MAX_ROWS) g_rows = CON_MAX_ROWS;

    for (u32 r = 0; r < g_rows; r++) {
        for (u32 c = 0; c < g_cols; c++) g_text[r][c] = ' ';
    }
    g_fb_ready = true;
}

void console_disable_fb(void)
{
    g_fb_ready = false;
}

/* ── Framebuffer character output ─────────────────────────────────────────── */

static inline const u8 *fb_glyph(char c)
{
    u8 idx = (u8)c;
    return (idx >= 0x20 && idx < 0x7F) ? g_vga_font[idx - 0x20]
                                       : g_vga_font[0];   /* fallback: space */
}

static inline void fb_store_pixel(u8 *p, u32 color)
{
    if (g_fb_bpp == 32) {
        *(u32 *)p = color | 0xFF000000u;
        return;
    }
    p[0] = (u8)(color);
    p[1] = (u8)(color >> 8);
    p[2] = (u8)(color >> 16);
}

/* Paint one character cell.  Writes only — never reads video memory. */
static void fb_draw_char(u32 col, u32 row, char c)
{
    u32 px = col * FONT_W;
    u32 py = row * FONT_H;
    if (px + FONT_W > g_fb_width || py + FONT_H > g_fb_height) return;

    const u8 *glyph = fb_glyph(c);
    u32 bytes_pp = g_fb_bpp / 8;

    for (u32 gy = 0; gy < FONT_H; gy++) {
        u8  bits = glyph[gy];
        u8 *dst  = g_fb_base + (py + gy) * g_fb_pitch + px * bytes_pp;

        if (g_fb_bpp == 32) {
            u32 *d32 = (u32 *)dst;
            for (u32 b = 0; b < FONT_W; b++) {
                d32[b] = (bits & (0x80 >> b)) ? (FG_COLOR | 0xFF000000u)
                                              : (BG_COLOR | 0xFF000000u);
            }
        } else {
            for (u32 b = 0; b < FONT_W; b++) {
                fb_store_pixel(dst + b * bytes_pp,
                               (bits & (0x80 >> b)) ? FG_COLOR : BG_COLOR);
            }
        }
    }
}

/* Repaint the whole screen from the text grid, one scanline at a time so the
 * writes go out in long sequential runs. */
static void fb_redraw_all(void)
{
    u32 bytes_pp = g_fb_bpp / 8;

    for (u32 r = 0; r < g_rows; r++) {
        for (u32 gy = 0; gy < FONT_H; gy++) {
            u32  y   = r * FONT_H + gy;
            u8  *row = g_fb_base + y * g_fb_pitch;

            if (g_fb_bpp == 32) {
                u32 *d = (u32 *)row;
                for (u32 c = 0; c < g_cols; c++) {
                    u8 bits = fb_glyph(g_text[r][c])[gy];
                    for (u32 b = 0; b < FONT_W; b++) {
                        *d++ = (bits & (0x80 >> b)) ? (FG_COLOR | 0xFF000000u)
                                                    : (BG_COLOR | 0xFF000000u);
                    }
                }
                /* The margin left over when the width is not a whole number
                 * of cells would otherwise keep whatever the bootloader put
                 * there, in a strip down the side of the text. */
                for (u32 x = g_cols * FONT_W; x < g_fb_width; x++) {
                    *d++ = BG_COLOR | 0xFF000000u;
                }
            } else {
                for (u32 c = 0; c < g_cols; c++) {
                    u8 bits = fb_glyph(g_text[r][c])[gy];
                    for (u32 b = 0; b < FONT_W; b++) {
                        fb_store_pixel(row + (c * FONT_W + b) * bytes_pp,
                                       (bits & (0x80 >> b)) ? FG_COLOR : BG_COLOR);
                    }
                }
                for (u32 x = g_cols * FONT_W; x < g_fb_width; x++) {
                    fb_store_pixel(row + x * bytes_pp, BG_COLOR);
                }
            }
        }
    }

    /* Rows below the last full character cell. */
    for (u32 y = g_rows * FONT_H; y < g_fb_height; y++) {
        u8 *row = g_fb_base + y * g_fb_pitch;
        if (g_fb_bpp == 32) {
            u32 *d = (u32 *)row;
            for (u32 x = 0; x < g_fb_width; x++) *d++ = BG_COLOR | 0xFF000000u;
        } else {
            for (u32 x = 0; x < g_fb_width; x++) {
                fb_store_pixel(row + x * bytes_pp, BG_COLOR);
            }
        }
    }
}

static void fb_scroll(void)
{
    if (g_rows == 0) return;

    /* Shift the text in RAM, then repaint.  The framebuffer is never read. */
    for (u32 r = 1; r < g_rows; r++) {
        memcpy(g_text[r - 1], g_text[r], g_cols);
    }
    for (u32 c = 0; c < g_cols; c++) g_text[g_rows - 1][c] = ' ';

    fb_redraw_all();
}

static void fb_putc(char c)
{
    if (!g_fb_ready || !g_fb_base || g_cols == 0 || g_rows == 0) return;

    if (c == '\n') {
        g_fb_col = 0;
        g_fb_row++;
    } else if (c == '\r') {
        g_fb_col = 0;
    } else if (c >= 0x20 && c < 0x7F) {
        g_text[g_fb_row][g_fb_col] = c;
        fb_draw_char(g_fb_col, g_fb_row, c);
        g_fb_col++;
        if (g_fb_col >= g_cols) { g_fb_col = 0; g_fb_row++; }
    }

    if (g_fb_row >= g_rows) {
        fb_scroll();
        g_fb_row = g_rows - 1;
    }
}

/* ── Public output ─────────────────────────────────────────────────────────── */

/* ── Kernel Log Ring Buffer (dmesg) ───────────────────────────────────────── */
#define KLOG_BUF_SIZE 65536
static char g_klog_buf[KLOG_BUF_SIZE];
static u64  g_klog_total = 0;

void kputc(char c)
{
    /* Store in kernel log ring buffer */
    g_klog_buf[g_klog_total % KLOG_BUF_SIZE] = c;
    g_klog_total++;

    if (g_uart_ready) uart_putc(UART_COM1, c);
    fb_putc(c);
}

u64 console_get_klog_size(void)
{
    return g_klog_total;
}

s64 console_read_klog(void *buf, size_t max_len, u64 *offset)
{
    if (!buf || max_len == 0 || !offset) return 0;

    irqflags_t irqf = spinlock_lock_irqsave(&g_console_lock);

    u64 total = g_klog_total;
    u64 start_avail = (total > KLOG_BUF_SIZE) ? (total - KLOG_BUF_SIZE) : 0;
    u64 cur_pos = *offset;

    if (cur_pos < start_avail) {
        cur_pos = start_avail;
    }

    if (cur_pos >= total) {
        spinlock_unlock_irqrestore(&g_console_lock, irqf);
        return 0;
    }

    size_t avail = (size_t)(total - cur_pos);
    size_t count = (avail < max_len) ? avail : max_len;
    char *out = (char *)buf;

    for (size_t i = 0; i < count; i++) {
        out[i] = g_klog_buf[(cur_pos + i) % KLOG_BUF_SIZE];
    }

    *offset = cur_pos + count;
    spinlock_unlock_irqrestore(&g_console_lock, irqf);
    return (s64)count;
}

void kputs(const char *s)
{
    irqflags_t irqf = spinlock_lock_irqsave(&g_console_lock);
    while (*s) kputc(*s++);
    kputc('\n');
    spinlock_unlock_irqrestore(&g_console_lock, irqf);
}

void console_write(const char *buf, size_t len)
{
    if (!buf || len == 0) return;
    irqflags_t irqf = spinlock_lock_irqsave(&g_console_lock);
    for (size_t i = 0; i < len; i++) {
        kputc(buf[i]);
    }
    spinlock_unlock_irqrestore(&g_console_lock, irqf);
}

static void kprintf_puts(const char *s, s32 prec, u32 width, bool left_align)
{
    if (!s) s = "(null)";
    u32 slen = 0;
    while (s[slen]) slen++;
    if (prec >= 0 && (u32)prec < slen) slen = (u32)prec;

    u32 pad_count = (width > slen) ? (width - slen) : 0;

    if (!left_align) {
        for (u32 i = 0; i < pad_count; i++) kputc(' ');
    }

    for (u32 i = 0; i < slen; i++) {
        kputc(s[i]);
    }

    if (left_align) {
        for (u32 i = 0; i < pad_count; i++) kputc(' ');
    }
}

static void kprintf_u64(u64 v, u32 base, bool upper, u32 min_width, bool left_align, char pad)
{
    static const char lo[] = "0123456789abcdef";
    static const char hi[] = "0123456789ABCDEF";
    const char *digits = upper ? hi : lo;
    char buf[22];
    int len = 0;
    if (v == 0) { buf[len++] = '0'; }
    while (v) { buf[len++] = digits[v % base]; v /= base; }
    int pad_count = (min_width > (u32)len) ? (int)(min_width - len) : 0;
    if (!left_align) {
        for (int i = 0; i < pad_count; i++) kputc(pad);
    }
    for (int i = len - 1; i >= 0; i--) kputc(buf[i]);
    if (left_align) {
        for (int i = 0; i < pad_count; i++) kputc(' ');
    }
}

static void kprintf_s64(s64 v, u32 min_width, bool left_align, char pad)
{
    bool neg = false;
    u64 uv;
    if (v < 0) {
        neg = true;
        uv = (u64)(-(v + 1)) + 1;
    } else {
        uv = (u64)v;
    }
    char buf[22];
    int len = 0;
    if (uv == 0) { buf[len++] = '0'; }
    while (uv) { buf[len++] = '0' + (char)(uv % 10); uv /= 10; }
    int total_len = len + (neg ? 1 : 0);
    int pad_count = (min_width > (u32)total_len) ? (int)(min_width - total_len) : 0;
    if (pad == '0' && neg) {
        kputc('-');
        for (int i = 0; i < pad_count; i++) kputc('0');
        for (int i = len - 1; i >= 0; i--) kputc(buf[i]);
    } else if (!left_align) {
        for (int i = 0; i < pad_count; i++) kputc(pad);
        if (neg) kputc('-');
        for (int i = len - 1; i >= 0; i--) kputc(buf[i]);
    } else {
        if (neg) kputc('-');
        for (int i = len - 1; i >= 0; i--) kputc(buf[i]);
        for (int i = 0; i < pad_count; i++) kputc(' ');
    }
}

void kprintf(const char *fmt, ...)
{
    irqflags_t irqf = spinlock_lock_irqsave(&g_console_lock);
    va_list ap;
    va_start(ap, fmt);

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { kputc(*p); continue; }
        p++;
        if (!*p) break; /* Stop if string ends with % */

        bool   left_align = false;
        bool   is_long    = false;
        bool   is_llong   = false;
        u32    width      = 0;
        s32    prec       = -1;   /* -1 = no precision specified */
        char   pad        = ' ';

        while (*p == '-' || *p == '+' || *p == '0' || *p == ' ') {
            if (*p == '-') left_align = true;
            else if (*p == '0') pad = '0';
            p++;
        }
        if (left_align) pad = ' ';

        while (*p >= '0' && *p <= '9') { width = width * 10 + (u32)(*p - '0'); p++; }
        if (*p == '.') {
            p++;
            prec = 0;
            while (*p >= '0' && *p <= '9') { prec = prec * 10 + (*p - '0'); p++; }
        }
        if (*p == 'l') { is_long  = true; p++; }
        if (*p == 'l') { is_llong = true; p++; }
        if (*p == 'z') { is_long  = true; p++; }
        
        if (!*p) break; /* Stop if string ends prematurely after modifiers */

        switch (*p) {
        case 'd': case 'i': {
            s64 v = is_llong ? va_arg(ap, s64)
                             : (is_long ? va_arg(ap, long) : va_arg(ap, int));
            kprintf_s64(v, width, left_align, pad);
            break;
        }
        case 'u': {
            u64 v = is_llong ? va_arg(ap, u64)
                             : (is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
            kprintf_u64(v, 10, false, width, left_align, pad);
            break;
        }
        case 'x': {
            u64 v = is_llong ? va_arg(ap, u64)
                             : (is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
            kprintf_u64(v, 16, false, width, left_align, pad);
            break;
        }
        case 'X': {
            u64 v = is_llong ? va_arg(ap, u64)
                             : (is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
            kprintf_u64(v, 16, true, width, left_align, pad);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            kprintf_puts("0x", -1, 0, false);
            kprintf_u64((u64)v, 16, false, 16, false, '0');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            kprintf_puts(s, prec, width, left_align);
            break;
        }
        case 'c': kputc((char)va_arg(ap, int)); break;
        case '%': kputc('%'); break;
        default:  kputc('%'); kputc(*p); break;
        }
    }
    va_end(ap);
    spinlock_unlock_irqrestore(&g_console_lock, irqf);
}
