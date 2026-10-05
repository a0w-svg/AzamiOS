#define time clock_test_time
#define main clock_app_main
#include "../../userland/apps/clock/main.c"
#undef main
#undef time
#define check(c) do { if (!(c)) { printf("Clock assertion failed at line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static time_t wall_now = 1787097000;
static unsigned long long mono_now = 10000;
static const char *system_zone = "UTC\n";
static bool clock_fails;
time_t clock_test_time(time_t *out) { if (out) *out = wall_now; return wall_now; }
int clock_gettime(int clock_id, struct timespec *out)
{
    check(clock_id == CLOCK_MONOTONIC);
    if (clock_fails) return -1;
    out->tv_sec = (time_t)(mono_now / 1000);
    out->tv_nsec = (long)(mono_now % 1000) * 1000000;
    return 0;
}
int sys_open(const char *path, int flags, int mode)
{
    (void)flags; (void)mode;
    return !strcmp(path, "/etc/timezone") ? 80 : -2;
}
ssize_t sys_read(int fd, void *out, size_t count)
{
    check(fd == 80);
    size_t len = strlen(system_zone); if (count > len) count = len;
    memcpy(out, system_zone, count); return (ssize_t)count;
}
int sys_close(int fd) { check(fd == 80); return 0; }
static void test_clock(void)
{
    setenv("TZ", "Asia/Tokyo", 1);
    check(update_system_time() && !getenv("TZ"));
    check(g_hours == 23 && g_mins == 50 && g_secs == 0);
    check(!update_system_time());
    /* Changing zone without changing seconds must trigger a repaint. */
    system_zone = "Asia/Tokyo\n";
    check(update_system_time());
    check(g_hours == 8 && g_mins == 50 && g_secs == 0);
    check(strstr(g_date_str, "August 19") && !strcmp(g_offset_str, "+0900"));
    system_zone = "UTC\n"; update_system_time();
    struct tm city; char badge[32];
    check(world_city_time(2, wall_now, &city, badge, sizeof(badge)));
    check(city.tm_hour == 19 && city.tm_isdst == 1 && !strcmp(badge, "EDT (-0400)"));
    check(world_city_time(5, wall_now, &city, badge, sizeof(badge)));
    check(city.tm_hour == 9 && city.tm_isdst == 0 && !strcmp(badge, "AEST (+1000)"));
    struct tm jan = {.tm_year=126, .tm_mon=0, .tm_mday=15, .tm_hour=23, .tm_min=50};
    time_t winter = timegm(&jan);
    check(world_city_time(2, winter, &city, badge, sizeof(badge)));
    check(city.tm_hour == 18 && city.tm_isdst == 0 && !strcmp(badge, "EST (-0500)"));
    check(world_city_time(5, winter, &city, badge, sizeof(badge)));
    check(city.tm_hour == 10 && city.tm_mday == 16 && !strcmp(badge, "AEDT (+1100)"));
    check(!getenv("TZ"));
    localtime_r(&wall_now, &city);
    check(city.tm_hour == 23 && city.tm_gmtoff == 0 && !strcmp(city.tm_zone, "UTC"));
    check(!world_city_time(6, wall_now, &city, badge, sizeof(badge)));
}
static void test_stopwatch(void)
{
    stopwatch_reset(); stopwatch_lap(); check(g_lap_count == 0);
    stopwatch_toggle(); check(g_sw_running && g_sw_elapsed_ms == 0);
    mono_now += 1350; check(stopwatch_update()); check(g_sw_elapsed_ms == 1350);
    /* A delayed event accounts for the entire gap; wall-clock changes don't. */
    wall_now += 3600; mono_now += 4250; stopwatch_lap();
    check(g_sw_elapsed_ms == 5600 && g_lap_count == 1 && g_laps[0] == 5600);
    mono_now += 30; stopwatch_toggle(); check(!g_sw_running && g_sw_elapsed_ms == 5630);
    mono_now += 9000; stopwatch_update(); check(g_sw_elapsed_ms == 5630);
    stopwatch_toggle(); mono_now += 75; stopwatch_lap(); check(g_laps[1] == 5705);
    clock_fails = true; stopwatch_toggle();
    check(g_sw_running && g_sw_elapsed_ms == 5705 && g_clock_status[0]);
    clock_fails = false; mono_now += 95; check(stopwatch_update());
    check(g_sw_elapsed_ms == 5800 && !g_clock_status[0]);
    unsigned long long saved = mono_now; mono_now = g_sw_anchor_ms - 1;
    check(!stopwatch_update() && g_sw_elapsed_ms == 5800 && g_clock_status[0]);
    mono_now = saved;
    for (int i = 0; i < MAX_LAPS + 2; i++) stopwatch_lap();
    check(g_lap_count == MAX_LAPS);
    stopwatch_reset(); check(!g_sw_running && g_sw_elapsed_ms == 0 && !g_clock_status[0]);
    clock_fails = true; stopwatch_toggle(); check(!g_sw_running && g_clock_status[0]);
}
int main(void)
{
    test_clock(); test_stopwatch();
    puts("Clock timezone refresh, seasonal world times and monotonic stopwatch actions: PASS");
    return 0;
}
