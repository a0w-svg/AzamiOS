/* Exercise the real Settings renderer with patterned tab surfaces. Hardware
 * and font services are substituted so this runs without an AzamiOS kernel. */
#include "../../userland/apps/settings/settings.h"
#define assert(condition) do { if (!(condition)) { \
    printf("Viewport assertion failed at line %d\n", __LINE__); abort(); \
} } while (0)

int az_theme_count(void) { return 32; }
int display_content_height(void) { return 744; }
int disks_content_height(void) { return 904; }
int az_channel_send(int chan, const az_ipc_msg_t *message) { (void)chan; (void)message; return 0; }
void az_font_draw_str(unsigned int *pixels, unsigned int pitch, unsigned int w,
                      unsigned int h, int x, int y, const char *str,
                      unsigned int color, const az_font_t *font, int scale, bool bold)
{
    (void)pixels; (void)pitch; (void)w; (void)h; (void)x; (void)y;
    (void)str; (void)color; (void)font; (void)scale; (void)bold;
}
void az_font_draw_char(unsigned int *pixels, unsigned int pitch, unsigned int w,
                       unsigned int h, int x, int y, char c,
                       unsigned int color, const az_font_t *font, int scale, bool bold)
{
    (void)pixels; (void)pitch; (void)w; (void)h; (void)x; (void)y;
    (void)c; (void)color; (void)font; (void)scale; (void)bold;
}
static unsigned int pattern(unsigned int x, unsigned int y)
{
    return 0xff000000U | (y << 12) | x;
}
static void draw_pattern(void)
{
    for (unsigned int y = 0; y < g_win.height; y++)
        for (unsigned int x = 0; x < g_win.width; x++)
            g_win.pixels[(size_t)y * g_win.width + x] = pattern(x, y);
}
void draw_display_tab(void) { draw_pattern(); }
void draw_audio_tab(void) { draw_pattern(); }
void draw_theme_tab(void) { draw_pattern(); }
void draw_time_tab(void) { draw_pattern(); }
void draw_network_tab(void) { draw_pattern(); }
void draw_power_tab(void) { draw_pattern(); }
void draw_disks_tab(void) { draw_pattern(); }
void draw_security_tab(void) { draw_pattern(); }
void draw_system_tab(void) { draw_pattern(); }

int main(void)
{
    const unsigned int sizes[][2] = {{100, 100}, {320, 250}, {480, 320}, {720, 510}, {1024, 768}};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        unsigned int w = sizes[i][0], h = sizes[i][1];
        size_t count = (size_t)w * h;
        unsigned int *allocation = malloc((count + 32) * sizeof(*allocation));
        assert(allocation);
        for (size_t j = 0; j < count + 32; j++) allocation[j] = 0xdeadbeef;
        memset(&g_win, 0, sizeof(g_win));
        g_win.width = w; g_win.height = h;
        g_win.pixels = g_win.shared = allocation + 16;
        g_win.clip_x1 = (int)w; g_win.clip_y1 = (int)h;
        for (int tab = 0; tab < 9; tab++) {
            g_active_tab = tab;
            draw_settings();
            assert(g_win.width == w && g_win.height == h);
            assert(g_win.pixels == allocation + 16 && g_win.shared == allocation + 16);
            assert(g_win.clip_depth == 0 && g_win.clip_x1 == (int)w && g_win.clip_y1 == (int)h);
            for (int j = 0; j < 16; j++) {
                assert(allocation[j] == 0xdeadbeef);
                assert(allocation[count + 16 + j] == 0xdeadbeef);
            }
            unsigned int top = w < 360 ? 80 : w < 720 ? 116 : 80;
            if (top < h - 38) assert(g_win.pixels[(size_t)top * w + 50] == pattern(50, 80));
        }
        free(allocation);
    }
    puts("Settings viewport: nine tabs at five window sizes, bounds and surface restoration: PASS");
    return 0;
}
