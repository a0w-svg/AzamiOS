#ifndef GFX_H
#define GFX_H
#include <stdint.h>
#include <stdbool.h>

#define GFX_WIDTH  640
#define GFX_HEIGHT 480

void gfx_init_mode13h(void);
void gfx_init_bga(void);
void gfx_put_pixel(int x, int y, uint32_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint32_t color);
void gfx_clear(uint32_t color);
void gfx_flip(void);
void gfx_draw_char(int x, int y, char c, uint32_t color, uint32_t bg_color);
void gfx_draw_text(int x, int y, const char *str, uint32_t color, uint32_t bg_color);
void gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void gfx_draw_circle(int xc, int yc, int r, uint32_t color);
void gfx_fill_circle(int xc, int yc, int r, uint32_t color);

void gfx_on_mouse_move(int x, int y);
bool gfx_is_enabled(void);

#endif

