/* Compare actual rasterization against an independent pixel-by-pixel oracle,
 * including pitch padding, scaled clipping and bold across byte boundaries. */
#include "../../userland/apps/shared/ui_kit.h"
#include <stdlib.h>
#include <limits.h>
#define check(c) do { if (!(c)) { printf("Font assertion failed: %s:%d\n", __FILE__, __LINE__); exit(1); } } while (0)
#define W 43
#define H 31
#define PITCH 49
#define INK 0xffabc123U
#define BACK 0x12345678U
static unsigned int actual[PITCH * H + 32], expected[PITCH * H + 32];

static int bitmap_pixel(const unsigned char *glyph, int width, int x, int y)
{
    return (glyph[y * ((width + 7) / 8) + x / 8] >> (7 - x % 8)) & 1;
}
static void reference(const az_font_t *font, int px, int py, int scale, bool bold)
{
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int64_t dx = (int64_t)x - px, dy = (int64_t)y - py;
        if (dx < 0 || dy < 0 || dx >= (int64_t)font->glyph_w * scale || dy >= (int64_t)font->glyph_h * scale) continue;
        int gx = (int)(dx / scale), gy = (int)(dy / scale);
        if (bitmap_pixel(font->glyph_data, font->glyph_w, gx, gy) ||
            (bold && gx && bitmap_pixel(font->glyph_data, font->glyph_w, gx - 1, gy)))
            expected[16 + y * PITCH + x] = INK;
    }
}
static void test_raster(void)
{
    const int widths[] = {1, 7, 8, 9, 10, 16, 17, 255};
    const int positions[][2] = {{0,0}, {3,4}, {-6,-2}, {40,29}, {-500,0}, {INT_MIN,0}, {INT_MAX,0}};
    unsigned char glyph[32 * 7];
    for (unsigned int i = 0; i < sizeof(glyph); i++) glyph[i] = (unsigned char)(i * 53 + 1);
    for (unsigned int w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
        az_font_t font = {.glyph_w = (uint8_t)widths[w], .glyph_h = 7,
            .first_char = 'A', .last_char = 'A', .num_glyphs = 1,
            .bytes_per_glyph = (uint16_t)(((widths[w] + 7) / 8) * 7), .glyph_data = glyph};
        for (int scale = 1; scale <= 3; scale++) for (int bold = 0; bold < 2; bold++)
            for (unsigned int p = 0; p < sizeof(positions) / sizeof(positions[0]); p++) {
                for (unsigned int j = 0; j < sizeof(actual) / sizeof(actual[0]); j++) actual[j] = expected[j] = BACK;
                reference(&font, positions[p][0], positions[p][1], scale, bold);
                az_font_draw_char(actual + 16, PITCH, W, H, positions[p][0], positions[p][1], 'A', INK, &font, scale, bold);
                check(memcmp(actual, expected, sizeof(actual)) == 0);
            }
        for (unsigned int j = 0; j < sizeof(actual) / sizeof(actual[0]); j++) actual[j] = BACK;
        font.bytes_per_glyph = 0;
        az_font_draw_char(actual + 16, PITCH, W, H, 0, 0, 'A', INK, &font, 1, false);
        for (unsigned int j = 0; j < sizeof(actual) / sizeof(actual[0]); j++) check(actual[j] == BACK);
    }
}
static void draw_variant(uk_window_t *win, int mode)
{
    switch (mode) {
    case 0: uk_draw_text(win, 1, 2, "AAA", INK); break;
    case 1: uk_draw_text_2x(win, 1, 2, "AAA", INK); break;
    case 2: uk_draw_text_small(win, 1, 2, "AAA", INK); break;
    case 3: uk_draw_text_bold(win, 1, 2, "AAA", INK); break;
    case 4: uk_draw_char(win, 1, 2, 'A', INK); break;
    case 5: uk_draw_text_ex(win, 1, 2, "AAA", INK, &de_font_regular, 2, true); break;
    case 6: uk_draw_text_font(win, 1, 2, "AAA", INK, win->font, 2, true); break;
    case 7: uk_draw_text_clip(win, 1, 2, "AAA", INK, 20); break;
    case 8: uk_draw_button(win, -10, -5, 60, 30, "AAA", UK_BTN_NORMAL); break;
    }
}
static void test_scissor(void)
{
    unsigned int pixels[W * H + 32], full[W * H + 32];
    unsigned char glyph[] = {0x81,0x40, 0x42,0x80, 0x24,0x40};
    az_font_t font = {.glyph_w=10, .glyph_h=3, .first_char='A', .last_char='A',
        .num_glyphs=1, .bytes_per_glyph=6, .glyph_data=glyph};
    uk_window_t win = {.pixels=pixels+16, .width=W, .height=H, .clip_x1=W, .clip_y1=H};
    for (int custom = 0; custom < 2; custom++) for (int mode = 0; mode <= 8; mode++) {
        win.font = custom ? &font : NULL;
        for (unsigned int i = 0; i < W * H + 32; i++) pixels[i] = full[i] = BACK;
        win.pixels = full + 16;
        draw_variant(&win, mode);
        win.pixels = pixels + 16;
        uk_push_clip(&win, 5, 5, 15, 7);
        draw_variant(&win, mode);
        uk_pop_clip(&win);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
            check(pixels[16+y*W+x] == (x >= 5 && x < 20 && y >= 5 && y < 12 ? full[16+y*W+x] : BACK));
        for (int i = 0; i < 16; i++) check(pixels[i] == BACK && pixels[16 + W * H + i] == BACK);
    }
    win.font = &font;
    for (int i = 0; i < W * H + 32; i++) pixels[i] = BACK;
    uk_draw_text_clip(&win, 0, 0, "AAA", INK, 19);
    check(pixels[16] == INK);
    check(pixels[16+10] == BACK); /* Only one whole 10px glyph fits. */
    for (int i = 0; i < UK_CLIP_STACK_MAX + 3; i++) uk_push_clip(&win, 5, 5, 15, 7);
    for (int i = 0; i < 3; i++) uk_pop_clip(&win);
    check(win.clip_x0 == 5 && win.clip_depth == UK_CLIP_STACK_MAX);
    for (int i = 0; i < UK_CLIP_STACK_MAX; i++) uk_pop_clip(&win);
    check(win.clip_x0 == 0 && win.clip_x1 == W && win.clip_depth == 0);
    uk_push_clip(&win, INT_MAX, INT_MAX, 20, 20);
    check(win.clip_x0 == W && win.clip_x1 == W && win.clip_y0 == H && win.clip_y1 == H);
    uk_draw_text(&win, 0, 0, "AAA", INK);
    uk_pop_clip(&win);
}
int main(void)
{
    test_raster(); test_scissor();
    puts("Wide fonts, bold byte carry, scaled clipping, text scissor and font spacing: PASS");
    return 0;
}
