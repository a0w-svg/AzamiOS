#include "settings.h"


#define SEC_HEADER_Y   86
#define SEC_LCOL_X     20
#define SEC_RCOL_X     370
#define SEC_ROW1_Y    114
#define SEC_ROW2_Y    144
#define SEC_ROW3_Y    174

#define SEC_AUTO_HDR_Y 212
#define SEC_AUTO1_Y    240
#define SEC_AUTO2_Y    268
#define SEC_AUTO3_Y    296
#define SEC_AUTO4_Y    324
#define SEC_FOOTER_Y   362

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

    uk_draw_section_header(&g_win, px, SEC_AUTO_HDR_Y, (int)w - 40, "Auto-Accept & Unattended Policies", UK_MAUVE);

    draw_toggle(px, SEC_AUTO1_Y, g_sec_auto_ipc,   "Auto-Accept IPC Capability Transfers (SCM_RIGHTS & Channels)");
    draw_toggle(px, SEC_AUTO2_Y, g_sec_auto_admin, "Auto-Approve Desktop Administrative Tasks (Unattended Admin)");
    draw_toggle(px, SEC_AUTO3_Y, g_sec_auto_dhcp,  "Auto-Accept Network DHCP Lease Transitions (Zero Disruption)");
    draw_toggle(px, SEC_AUTO4_Y, g_sec_auto_trace, "Auto-Accept System Tracing & Telemetry (ptrace / ktrace hooks)");

    uk_draw_panel(&g_win, px, SEC_FOOTER_Y, (int)w - 40, 48, UK_SURFACE0);
    int all_auto = g_sec_auto_ipc && g_sec_auto_admin && g_sec_auto_dhcp && g_sec_auto_trace;
    if (all_auto) {
        uk_draw_badge(&g_win, px + 10, SEC_FOOTER_Y + 8, "Auto-Accept: ACTIVE", UK_SURFACE1, UK_GREEN);
        uk_draw_text(&g_win, px + 175, SEC_FOOTER_Y + 10, "Automated execution enabled for IPC, admin, DHCP, and telemetry.", UK_TEXT);
    } else {
        uk_draw_badge(&g_win, px + 10, SEC_FOOTER_Y + 8, "Auto-Accept: CUSTOM", UK_SURFACE1, UK_YELLOW);
        uk_draw_text(&g_win, px + 175, SEC_FOOTER_Y + 10, "Custom policy active. Selected operations prompt for confirmation.", UK_SUBTEXT0);
    }
    uk_draw_text(&g_win, px + 10, SEC_FOOTER_Y + 28, "Security policies persist to /etc/security.conf and apply immediately.", UK_OVERLAY0);
}


void handle_security_mouse(int mx, int my)
{
    unsigned int w = g_win.width, h = g_win.height;
    (void)w; (void)h;
    /* Left col sysctl */
                    if (hit_toggle(SEC_LCOL_X, SEC_ROW1_Y, mx, my)) {
                        g_sec_dmesg ^= 1;
                        write_proc_val("/proc/sys/kernel/dmesg_restrict", g_sec_dmesg);
                        draw_settings();
                        return;
                    }
                    if (hit_toggle(SEC_LCOL_X, SEC_ROW2_Y, mx, my)) {
                        g_sec_kptr ^= 1;
                        write_proc_val("/proc/sys/kernel/kptr_restrict", g_sec_kptr);
                        draw_settings();
                        return;
                    }
                    if (hit_toggle(SEC_LCOL_X, SEC_ROW3_Y, mx, my)) {
                        g_sec_mmap ^= 1;
                        write_proc_val("/proc/sys/kernel/mmap_min_addr", g_sec_mmap ? 65536 : 0);
                        draw_settings();
                        return;
                    }
                    /* Right col sysctl */
                    if (hit_toggle(SEC_RCOL_X, SEC_ROW1_Y, mx, my)) {
                        g_sec_yama ^= 1;
                        write_proc_val("/proc/sys/kernel/yama/ptrace_scope", g_sec_yama);
                        draw_settings();
                        return;
                    }
                    if (hit_toggle(SEC_RCOL_X, SEC_ROW2_Y, mx, my)) {
                        g_sec_hlinks ^= 1;
                        write_proc_val("/proc/sys/fs/protected_hardlinks", g_sec_hlinks);
                        draw_settings();
                        return;
                    }
                    if (hit_toggle(SEC_RCOL_X, SEC_ROW3_Y, mx, my)) {
                        g_sec_slinks ^= 1;
                        write_proc_val("/proc/sys/fs/protected_symlinks", g_sec_slinks);
                        draw_settings();
                        return;
                    }
                    /* Auto-Accept toggles */
                    if (hit_toggle_wide(20, SEC_AUTO1_Y, mx, my, 650)) {
                        g_sec_auto_ipc ^= 1;
                        save_security_config();
                        draw_settings();
                        return;
                    }
                    if (hit_toggle_wide(20, SEC_AUTO2_Y, mx, my, 650)) {
                        g_sec_auto_admin ^= 1;
                        save_security_config();
                        draw_settings();
                        return;
                    }
                    if (hit_toggle_wide(20, SEC_AUTO3_Y, mx, my, 650)) {
                        g_sec_auto_dhcp ^= 1;
                        save_security_config();
                        draw_settings();
                        return;
                    }
                    if (hit_toggle_wide(20, SEC_AUTO4_Y, mx, my, 650)) {
                        g_sec_auto_trace ^= 1;
                        save_security_config();
                        draw_settings();
                        return;
                    }
}
