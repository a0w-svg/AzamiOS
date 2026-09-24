#include "settings.h"


void draw_disks_tab(void)
{
    int px = 20;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, 86, (int)w - 40, "Storage Partitions & Mounted Filesystems", UK_TEAL);

    draw_storage_card(px, 114, (int)w - 40, 62, "Root Partition (sata0p2)", "/", "SATA Rootfs Ext2", "/");
    draw_storage_card(px, 186, (int)w - 40, 62, "Boot Partition (sata0p1)", "/boot", "SATA Boot Ext2", "/boot");
    draw_storage_card(px, 258, (int)w - 40, 62, "RAM Scratchpad", "/tmp", "tmpfs Volatile RAM", "/tmp");

    uk_draw_section_header(&g_win, px, 330, (int)w - 40, "Storage Maintenance & Cache Flush", UK_SAPPHIRE);

    uk_draw_button(&g_win, px, 358, 190, 30, "Clean Temporary Files", UK_BTN_NORMAL);
    uk_draw_text(&g_win, px + 205, 365, "Purges /tmp scratch files and synchronizes VFS block cache.", UK_SUBTEXT0);

    uk_draw_panel(&g_win, px, 400, (int)w - 40, 26, UK_SURFACE0);
    uk_draw_text(&g_win, px + 10, 405, g_disk_status_msg, UK_TEXT);
}


void handle_disks_mouse(int mx, int my)
{
    unsigned int w = g_win.width, h = g_win.height;
    (void)w; (void)h;
    /* Clean Temporary Files button (y: 358..388, x: 20..210) */
                    if (mx >= 20 && mx <= 210 && my >= 358 && my <= 388) {
                        clean_temp_files();
                        draw_settings();
                        return;
                    }
}
