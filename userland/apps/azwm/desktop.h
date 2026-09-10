/* ============================================================================
 * AzamiOS — Desktop Rendering (Background, Taskbar, Cursor, Font)
 * File: user/apps/azwm/desktop.h
 * ============================================================================ */
#pragma once

#include "compositor.h"

/** desktop_draw_background — Render gradient desktop background. */
void desktop_draw_background(unsigned int *buf, unsigned int w, unsigned int h, unsigned int pitch_px);

/** desktop_draw_taskbar — Render bottom taskbar with open window list. */
void desktop_draw_taskbar(unsigned int *buf, unsigned int w, unsigned int h, unsigned int pitch_px,
                          az_window_t *windows, unsigned int max_windows);

/** desktop_draw_cursor — Render mouse cursor sprite at (cx, cy). */
void desktop_draw_cursor(unsigned int *buf, unsigned int w, unsigned int h, unsigned int pitch_px,
                         int cx, int cy);

/* Native size of the pointer sprite, and its hotspot (the arrow tip). */
#define DESKTOP_CURSOR_W    14
#define DESKTOP_CURSOR_H    21
#define DESKTOP_CURSOR_HX   0
#define DESKTOP_CURSOR_HY   0

/** desktop_cursor_blit_bgra — write the pointer sprite into a caller buffer of
 *  @dst_w x @dst_h BGRA8888 pixels, top-left aligned, rest left untouched. */
void desktop_cursor_blit_bgra(unsigned int *dst, unsigned int dst_w, unsigned int dst_h);

/** desktop_draw_char_at — Draw a single character using the embedded 8x16 font. */
void desktop_draw_char_at(unsigned int *buf, unsigned int w, unsigned int h, unsigned int pitch_px,
                          int x, int y, char c, unsigned int color);

/** desktop_draw_text_at — Draw a null-terminated string. */
void desktop_draw_text_at(unsigned int *buf, unsigned int w, unsigned int h, unsigned int pitch_px,
                          int x, int y, const char *text, unsigned int color);
