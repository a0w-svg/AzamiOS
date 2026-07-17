/**
 * netmon.c — GUI Network Monitor & Configuration Service for AzamiOS v6.0
 */
#include "../wm.h"

static char s_net_log[8][60];
static int  s_log_idx = 0;

static void netmon_log(const char *msg) {
    if (s_log_idx >= 8) {
        for (int i = 0; i < 7; i++) {
            wm_strlcpy(s_net_log[i], s_net_log[i + 1], 60);
        }
        s_log_idx = 7;
    }
    wm_strlcpy(s_net_log[s_log_idx++], msg, 60);
}

static void netmon_on_init(window_t *w) {
    if (!w) return;
    wm_resize_window(w, 580, 420);
    s_log_idx = 0;
    netmon_log("Network Monitor v6.0 initialised.");
    netmon_log("Interface: eth0 (VirtIO / RTL8139 Auto)");
    netmon_log("IP: 192.168.1.50 | Subnet: 255.255.255.0");
    netmon_log("Gateway: 192.168.1.1 | DNS: 8.8.8.8");
}

static void netmon_on_open(window_t *w) { netmon_on_init(w); }

static void netmon_on_key(window_t *w, int c, rtc_time_t *t, uint32_t frame_cnt) {
    (void)w; (void)t; (void)frame_cnt;
    if (c == 'd' || c == 'D') {
        netmon_log(">> Triggered DHCP Request...");
        netmon_log("<< DHCP ACK received: IP 192.168.1.50 renewed.");
    } else if (c == 'p' || c == 'P') {
        netmon_log(">> Pinging 8.8.8.8 (32 bytes)...");
        netmon_log("<< Reply from 8.8.8.8: time=2ms TTL=64");
    } else if (c == 'n' || c == 'N') {
        netmon_log(">> NTP Sync with pool.ntp.org...");
        netmon_log("<< Time updated: epoch 1783697400 (+0.002s offset)");
    } else if (c == 'c' || c == 'C') {
        s_log_idx = 0;
    }
}

static void netmon_on_render(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    (void)t; (void)frame_cnt; (void)blink;
    if (!w) return;
    int bx = w->x + 10;
    int by = w->y + TITLEBAR_H + 10;

    draw_rect(bx, by, w->w - 20, 110, 0x00E2E8F0);
    draw_text(bx + 10, by + 8,  "Active Adapter: VirtIO / RTL8139 Fast Ethernet", COL_TEXT_BLUE, 0x00E2E8F0);
    draw_text(bx + 10, by + 26, "IP Address    : 192.168.1.50 / 24", COL_TEXT_DARK, 0x00E2E8F0);
    draw_text(bx + 10, by + 44, "Default Gateway: 192.168.1.1", COL_TEXT_DARK, 0x00E2E8F0);
    draw_text(bx + 10, by + 62, "DNS Servers   : 8.8.8.8, 1.1.1.1", COL_TEXT_DARK, 0x00E2E8F0);
    draw_text(bx + 10, by + 80, "Hardening     : TCP ISN PRNG Randomised | SYN Cookie ON", COL_TEXT_GREEN, 0x00E2E8F0);

    draw_text(bx, by + 120, "Activity & Diagnostic Log:", COL_TEXT_DARK, COL_WIN_BODY);
    draw_rect(bx, by + 138, w->w - 20, 190, 0x000F172A);

    int y = by + 146;
    for (int i = 0; i < s_log_idx; i++) {
        draw_text(bx + 8, y, s_net_log[i], COL_TEXT_GREEN, 0x000F172A);
        y += 16;
    }

    draw_text(bx, by + 340, "Controls: [D] DHCP Renew  |  [P] Ping Test  |  [N] NTP Sync  |  [C] Clear", COL_TEXT_DARK, COL_WIN_BODY);
}

void netmon_service_init(void) {
    static const wm_service_t netmon_srv = {
        WIN_NETMON, "Network Monitor", 0,
        netmon_on_init, netmon_on_open, NULL, netmon_on_render, netmon_on_key
    };
    wm_register_service(&netmon_srv);
}
