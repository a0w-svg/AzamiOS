#include "settings.h"


#define SEC_HEADER_Y   86
#define SEC_LCOL_X     20
#define SEC_RCOL_X     370
#define SEC_ROW1_Y    114
#define SEC_ROW2_Y    144
#define SEC_ROW3_Y    174

#define SEC_AUTO_HDR_Y 212
void draw_security_tab(void)
{
    int px = 20;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, SEC_HEADER_Y, (int)w - 40, "Kernel Runtime Hardening & Sysctl Mitigations", UK_RED);

    draw_toggle(SEC_LCOL_X, SEC_ROW1_Y, g_sec_dmesg,  "dmesg_restrict (Ring buffer guard)");
    draw_toggle(SEC_LCOL_X, SEC_ROW2_Y, g_sec_kptr,   "kptr_restrict (Mask kernel ptrs)");
    draw_toggle(SEC_LCOL_X, SEC_ROW3_Y, g_sec_mmap,   "mmap_min_addr (NULL deref guard)");

    draw_toggle(SEC_RCOL_X, SEC_ROW1_Y, g_sec_yama,   "yama.ptrace_scope (YAMA security)");
    draw_toggle(SEC_RCOL_X, SEC_ROW2_Y, g_sec_hlinks, "protected_hardlinks (Link guard)");
    draw_toggle(SEC_RCOL_X, SEC_ROW3_Y, g_sec_slinks, "protected_symlinks (Traversal guard)");

    uk_draw_section_header(&g_win, px, SEC_AUTO_HDR_Y, (int)w - 40, "Administrative Policy Support", UK_MAUVE);
    uk_draw_panel(&g_win, px, 240, (int)w - 40, 80, UK_SURFACE0);
    uk_draw_text(&g_win, px + 12, 250, "Auto-accept policy enforcement is not implemented.", UK_SUBTEXT0);
    uk_draw_text(&g_win, px + 12, 274, "IPC, DHCP and tracing use their existing kernel permissions.", UK_SUBTEXT0);
    uk_draw_text(&g_win, px + 12, 298, "Kernel hardening switches above apply for the current boot.", UK_OVERLAY0);
}


void handle_security_mouse(int mx, int my)
{
    unsigned int w = g_win.width, h = g_win.height;
    (void)w; (void)h;
    /* Left col sysctl */
                    if (hit_toggle(SEC_LCOL_X, SEC_ROW1_Y, mx, my)) {
                        toggle_proc_setting("/proc/sys/kernel/dmesg_restrict", &g_sec_dmesg, 1);
                        draw_settings();
                        return;
                    }
                    if (hit_toggle(SEC_LCOL_X, SEC_ROW2_Y, mx, my)) {
                        toggle_proc_setting("/proc/sys/kernel/kptr_restrict", &g_sec_kptr, 1);
                        draw_settings();
                        return;
                    }
                    if (hit_toggle(SEC_LCOL_X, SEC_ROW3_Y, mx, my)) {
                        toggle_proc_setting("/proc/sys/kernel/mmap_min_addr", &g_sec_mmap, 65536);
                        draw_settings();
                        return;
                    }
                    /* Right col sysctl */
                    if (hit_toggle(SEC_RCOL_X, SEC_ROW1_Y, mx, my)) {
                        toggle_proc_setting("/proc/sys/kernel/yama/ptrace_scope", &g_sec_yama, 1);
                        draw_settings();
                        return;
                    }
                    if (hit_toggle(SEC_RCOL_X, SEC_ROW2_Y, mx, my)) {
                        toggle_proc_setting("/proc/sys/fs/protected_hardlinks", &g_sec_hlinks, 1);
                        draw_settings();
                        return;
                    }
                    if (hit_toggle(SEC_RCOL_X, SEC_ROW3_Y, mx, my)) {
                        toggle_proc_setting("/proc/sys/fs/protected_symlinks", &g_sec_slinks, 1);
                        draw_settings();
                        return;
                    }
}
