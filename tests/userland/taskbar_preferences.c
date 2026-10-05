/* Real taskbar preference refresh and real AzamiOS timezone conversion.
 * Substitute configuration I/O, wall time and font services on the host. */
#define time tb_test_time
#define main taskbar_app_main
#include "../../userland/apps/taskbar/main.c"
#undef main
#undef time
#define assert(condition) do { if (!(condition)) { \
    printf("Taskbar assertion failed at line %d\n", __LINE__); exit(1); \
} } while (0)

static const char *test_zone = "UTC\n";
static const char *test_font_config = "system_font=/fonts/first.azf\n";
static const char *test_user_font_config;
static size_t positions[3];
static time_t test_now = 1787097000; /* 2026-08-18 23:50 UTC */
static az_font_t first_font = {.glyph_w = 8, .glyph_h = 16};
static az_font_t second_font = {.glyph_w = 10, .glyph_h = 20};
static az_font_t *test_selected_font = &first_font;
static int font_loads, font_frees, drawn_chars, notified;
static int drawn_x[5];
static az_font_t *freed_font;
static const az_font_t *drawn_font;
static az_wm_msg_t invalidation;

time_t tb_test_time(time_t *out)
{
    if (out) *out = test_now;
    return test_now;
}
int sys_open(const char *path, int flags, int mode)
{
    (void)flags; (void)mode;
    int index;
    if (!strcmp(path, "/etc/timezone")) index = 0;
    else if (!strcmp(path, AZ_FONT_CONFIG_FILE)) index = 1;
    else if (strstr(path, "/.config/font.conf") && test_user_font_config) index = 2;
    else return -2;
    positions[index] = 0;
    return 80 + index;
}
ssize_t sys_read(int fd, void *out, size_t count)
{
    assert(fd >= 80 && fd <= 82);
    int index = fd - 80;
    const char *text = index == 0 ? test_zone : index == 1 ? test_font_config : test_user_font_config;
    size_t left = strlen(text) - positions[index];
    if (count > left) count = left;
    /* Exercise the streaming hash with short reads. */
    if (index > 0 && count > 9) count = 9;
    memcpy(out, text + positions[index], count);
    positions[index] += count;
    return (ssize_t)count;
}
int sys_close(int fd) { assert(fd >= 80 && fd <= 82); return 0; }
az_font_t *az_font_load_default(void) { font_loads++; return test_selected_font; }
void az_font_free(az_font_t *font) { font_frees++; freed_font = font; }
void az_font_draw_char(uint32_t *pixels, uint32_t pitch, uint32_t width, uint32_t height,
                       int x, int y, char c, uint32_t color, const az_font_t *font,
                       int scale, bool bold)
{
    (void)pixels; (void)pitch; (void)width; (void)height;
    (void)y; (void)c; (void)color; (void)scale; (void)bold;
    drawn_font = font;
    if (drawn_chars < 5) drawn_x[drawn_chars] = x;
    drawn_chars++;
}
int az_channel_send(int channel, const az_ipc_msg_t *message)
{
    (void)channel;
    memcpy(&invalidation, message, sizeof(invalidation));
    notified++;
    return 0;
}
int main(void)
{
    /* TZ inherited from another process must not override desktop settings. */
    setenv("TZ", "Asia/Tokyo", 1);
    assert(tb_refresh_preferences());
    assert(!getenv("TZ"));
    assert(g_panel_font == &first_font && font_loads == 1);
    char utc[6], tokyo[6], date[20];
    tb_build_clock(utc);
    assert(!strcmp(utc, "23:50"));
    assert(!tb_refresh_preferences() && font_loads == 1);
    test_zone = "Asia/Tokyo\n";
    assert(tb_refresh_preferences());
    tb_build_clock(tokyo);
    assert(!strcmp(tokyo, "08:50"));
    /* Same UTC instant crosses midnight in Tokyo: both labels must update. */
    tb_build_date(date, sizeof(date));
    assert(!strcmp(date, g_last_date));
    test_zone = "UTC\n";
    assert(tb_refresh_preferences());
    tb_build_clock(tokyo);
    assert(!strcmp(utc, tokyo));
    assert(strcmp(date, g_last_date));
    test_zone = "Europe/Warsaw\n";
    assert(tb_refresh_preferences());
    tb_build_clock(tokyo);
    assert(!strcmp(tokyo, "01:50")); /* summer offset, not standard offset */
    test_zone = "America/New_York\n";
    assert(tb_refresh_preferences());
    tb_build_clock(tokyo);
    assert(!strcmp(tokyo, "19:50"));

    test_font_config = "system_font=/fonts/second.azf\n";
    test_selected_font = &second_font;
    assert(tb_refresh_preferences());
    assert(g_panel_font == &second_font && font_loads == 2);
    assert(font_frees == 1 && freed_font == &first_font);
    assert(tb_font_width() == 10 && tb_font_height() == 20);
    assert(!tb_refresh_preferences() && font_loads == 2);
    test_user_font_config = "system_font=/fonts/first.azf\n";
    test_selected_font = &first_font;
    assert(tb_refresh_preferences());
    assert(g_panel_font == &first_font && font_loads == 3);
    test_user_font_config = NULL;
    test_selected_font = &second_font;
    assert(tb_refresh_preferences());
    assert(g_panel_font == &second_font && font_loads == 4);

    test_font_config = "system_font=/fonts/temporarily-unavailable.azf\n";
    test_selected_font = NULL;
    int frees_before = font_frees;
    assert(!tb_refresh_preferences());
    assert(g_panel_font == &second_font && font_frees == frees_before);
    test_selected_font = &second_font;
    assert(tb_refresh_preferences()); /* retry after the load becomes available */
    assert(g_panel_font == &second_font && font_frees == frees_before);

    test_zone = "UTC\n";
    test_now = 1787097599; /* 23:59:59 UTC */
    assert(tb_refresh_preferences());
    strcpy(date, g_last_date);
    test_now++;
    assert(tb_refresh_preferences());
    assert(!strcmp(g_last_clock, "00:00") && strcmp(date, g_last_date));
    assert(!tb_refresh_preferences());

    g_w = 640; g_h = WINDOW_H;
    g_px = malloc((size_t)g_w * g_h * sizeof(*g_px));
    assert(g_px);
    draw_clock_only();
    assert(drawn_font == &second_font && drawn_chars == 5);
    for (int i = 1; i < 5; i++) assert(drawn_x[i] - drawn_x[i - 1] == 10);
    assert(notified == 1 && invalidation.invalidate.w == 50 && invalidation.invalidate.h == 20);
    free(g_px);
    puts("Taskbar timezone/date refresh, font reload caching and clock glyph geometry: PASS");
    return 0;
}
