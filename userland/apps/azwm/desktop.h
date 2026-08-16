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

/** desktop_draw_char_at — Draw a single character using the embedded 8x16 font. */
void desktop_draw_char_at(unsigned int *buf, unsigned int w, unsigned int h, unsigned int pitch_px,
                          int x, int y, char c, unsigned int color);

/** desktop_draw_text_at — Draw a null-terminated string. */
void desktop_draw_text_at(unsigned int *buf, unsigned int w, unsigned int h, unsigned int pitch_px,
                          int x, int y, const char *text, unsigned int color);
