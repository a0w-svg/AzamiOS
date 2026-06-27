/**
 * kernel/hal/hal_console.h  –  Console/TTY Hardware Abstraction Layer
 *
 * Defines the interface contract used by kernel-independent code
 * (lib/net, lib/gfx, printf, …) to emit text output without directly
 * touching VGA ports or terminal_write().
 *
 * The kernel initialises g_hal_console once at boot after the terminal
 * driver is ready; user-space stubs may provide a different back-end.
 */
#ifndef HAL_CONSOLE_H
#define HAL_CONSOLE_H

#include <stdint.h>

typedef struct {
    /** Write a NUL-terminated string. Returns bytes written, or -1 on error. */
    int  (*write)(const char *str);
    /** Write a single character. Returns 0 on success, -1 on error. */
    int  (*putc)(char c);
    /** Clear the console to its default state. */
    void (*clear)(void);
} hal_console_t;

/**
 * Global console handle — set once by terminal_hw_init() at kernel boot.
 * Remains NULL until the terminal driver is up; callers should guard
 * against NULL before boot is complete.
 */
extern hal_console_t *g_hal_console;

#endif /* HAL_CONSOLE_H */
