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
    g_fb_ready  = true;
}

void console_disable_fb(void)
{
    g_fb_ready = false;
}

/* ── Framebuffer character output ─────────────────────────────────────────── */

static void fb_put_pixel(u32 x, u32 y, u32 color)
{
    u32 bytes_per_pixel = g_fb_bpp / 8;
    u8 *pixel = g_fb_base + y * g_fb_pitch + x * bytes_per_pixel;
    pixel[0] = (u8)(color);
    pixel[1] = (u8)(color >> 8);
    pixel[2] = (u8)(color >> 16);
    if (bytes_per_pixel == 4) pixel[3] = 0xFF;
}

static void fb_draw_char(u32 col, u32 row, char c)
{
    u32 px = col * FONT_W;
    u32 py = row * FONT_H;
    u8 idx = (u8)c;
    const u8 *glyph = (idx >= 0x20 && idx < 0x7F)
                       ? g_vga_font[idx - 0x20]
                       : g_vga_font[0];  /* fallback: space */

    for (u32 row_i = 0; row_i < FONT_H; row_i++) {
        u8 bits = glyph[row_i];
        for (u32 bit = 0; bit < FONT_W; bit++) {
            u32 color = (bits & (0x80 >> bit)) ? FG_COLOR : BG_COLOR;
            if (px + bit < g_fb_width && py + row_i < g_fb_height)
                fb_put_pixel(px + bit, py + row_i, color);
        }
    }
}

static void fb_scroll(void)
{
    u32 scroll_bytes = (g_fb_height - FONT_H) * g_fb_pitch;
    u8 *src = g_fb_base + g_fb_pitch * FONT_H;
    memmove(g_fb_base, src, scroll_bytes);

    /* Clear bottom row */
    u8 *bottom = g_fb_base + scroll_bytes;
    memset(bottom, 0, FONT_H * g_fb_pitch);
}

static void fb_putc(char c)
{
    if (!g_fb_ready || !g_fb_base) return;
    u32 char_cols = g_fb_width  / FONT_W;
    u32 char_rows = g_fb_height / FONT_H;

    if (c == '\n') {
        g_fb_col = 0;
        g_fb_row++;
    } else if (c == '\r') {
        g_fb_col = 0;
    } else if (c >= 0x20 && c < 0x7F) {
        fb_draw_char(g_fb_col, g_fb_row, c);
        g_fb_col++;
        if (g_fb_col >= char_cols) { g_fb_col = 0; g_fb_row++; }
    }

    if (g_fb_row >= char_rows) {
        fb_scroll();
        g_fb_row = char_rows - 1;
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
