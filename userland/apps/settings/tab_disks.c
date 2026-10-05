#include "settings.h"
#include "../shared/mount_table.h"

#define STORAGE_MOUNTS_MAX 32
static az_mount_entry_t s_mounts[STORAGE_MOUNTS_MAX];
static int s_mount_count = -1, s_mounts_omitted, s_mounts_unavailable;

static void refresh_mounts(void)
{
    az_mount_entry_t *new_mounts = malloc(sizeof(s_mounts));
    int omitted = 0;
    int count = new_mounts ? az_mount_read_table("/proc/mounts", new_mounts, STORAGE_MOUNTS_MAX, &omitted) : -1;
    s_mounts_unavailable = count < 0;
    if (count >= 0) {
        memcpy(s_mounts, new_mounts, (size_t)count * sizeof(s_mounts[0]));
        s_mount_count = count;
        s_mounts_omitted = omitted;
    } else if (s_mount_count < 0) {
        memset(s_mounts, 0, sizeof(s_mounts));
        strcpy(s_mounts[0].source, "Root filesystem");
        strcpy(s_mounts[0].path, "/");
        s_mount_count = 1;
    }
    free(new_mounts);
}

static int maintenance_y(void)
{
    if (s_mount_count < 0) refresh_mounts();
    return 142 + s_mount_count * 90;
}

int disks_content_height(void)
{
    int bottom = maintenance_y() + 110;
    return bottom < 464 ? 464 : bottom;
}

void draw_disks_tab(void)
{
    int px = 20, width = (int)g_win.width;
    if (s_mount_count < 0) refresh_mounts();
    uk_draw_section_header(&g_win, px, 86, width - 170, "Mounted Filesystems", UK_TEAL);
    uk_draw_button(&g_win, width - 120, 82, 100, 26, "Refresh", UK_BTN_NORMAL);
    char summary[96];
    if (s_mounts_unavailable) snprintf(summary, sizeof(summary), "Mount table unavailable; showing last known locations.");
    else snprintf(summary, sizeof(summary), "%d data filesystems; %d entries omitted. Usage is read from each mount.", s_mount_count, s_mounts_omitted);
    uk_draw_text_small(&g_win, px, 116, summary, UK_SUBTEXT0);

    for (int i = 0; i < s_mount_count; i++) {
        int y = 138 + i * 90;
        draw_storage_card(px, y, width - 40, 80, s_mounts[i].source,
                          s_mounts[i].path, s_mounts[i].type, s_mounts[i].path);
        const char *mode = s_mounts[i].options[0] ?
            (az_mount_readonly(&s_mounts[i]) ? "Read-only" : "Read/write") : "Access mode unavailable";
        uk_draw_text_small(&g_win, px + 12, y + 62, mode, UK_OVERLAY0);
    }
    int y = maintenance_y();
    uk_draw_section_header(&g_win, px, y, width - 40, "Storage Maintenance", UK_SAPPHIRE);
    uk_draw_button(&g_win, px, y + 28, 190, 30, "Clean Temporary Files", UK_BTN_NORMAL);
    uk_draw_button(&g_win, px + 205, y + 28, 130, 30, "Sync Buffers", UK_BTN_NORMAL);
    uk_draw_panel(&g_win, px, y + 70, width - 40, 26, UK_SURFACE0);
    uk_draw_text_clip(&g_win, px + 10, y + 75, g_disk_status_msg, UK_TEXT, width - 60);
}

void handle_disks_mouse(int mx, int my)
{
    int width = settings_content_width();
    if (mx >= width - 120 && mx < width - 20 && my >= 82 && my < 108) {
        refresh_mounts();
        snprintf(g_disk_status_msg, sizeof(g_disk_status_msg), s_mounts_unavailable
                 ? "Could not refresh mounted filesystems." : "Mounted filesystems and usage refreshed.");
        draw_settings();
        return;
    }
    int y = maintenance_y() + 28;
    if (my < y || my >= y + 30) return;
    if (mx >= 20 && mx < 210) clean_temp_files();
    else if (mx >= 225 && mx < 355) {
        sync();
        snprintf(g_disk_status_msg, sizeof(g_disk_status_msg), "Filesystem buffers synchronized.");
    } else return;
    draw_settings();
}
