/* ============================================================================
 * AzamiOS — Unified Console (UART + optional framebuffer)
 * File: drivers/char/console.h / console.c
 *
 * kprintf() is the primary kernel log function.
 * Early boot: output goes to UART COM1 only.
 * After vmm_init(): output also goes to the Limine framebuffer.
 * ============================================================================ */
#pragma once
#include "../../include/azami/types.h"
#include "../../include/azami/defs.h"

/** console_init_early() — Set up UART COM1, ready before any other init. */
void console_init_early(void);

/** console_init_fb(fb_base, width, height, pitch, bpp) — Enable framebuffer. */
void console_init_fb(void *fb_base, u32 width, u32 height, u32 pitch, u8 bpp);

/** console_disable_fb() — Disable direct kernel text rendering to framebuffer. */
void console_disable_fb(void);

/** kprintf(fmt, ...) — Kernel printf (subset: %s %c %d %u %x %llx %p %%). */
__printf(1, 2) void kprintf(const char *fmt, ...);

/** kputs(s) — Output a string + newline to all console backends. */
void kputs(const char *s);

/** kputc(c) — Output a single character. */
void kputc(char c);

/** console_read_klog(buf, max_len, offset) — Read from kernel log ring buffer. */
s64 console_read_klog(void *buf, size_t max_len, u64 *offset);

/** console_get_klog_size() — Return total bytes written to kernel log buffer. */
u64 console_get_klog_size(void);

/* To enable debug logs, compile with -DCONFIG_DEBUG_LOGS or uncomment below: */
// #define CONFIG_DEBUG_LOGS 1

#ifdef CONFIG_DEBUG_LOGS
#define kdebug(...) kprintf("[DEBUG] " __VA_ARGS__)
#define pr_debug(fmt, ...) kprintf(fmt, ##__VA_ARGS__)
#else
#define kdebug(...) do {} while(0)
#ifndef pr_debug
#define pr_debug(fmt, ...) do {} while(0)
#endif
#endif
