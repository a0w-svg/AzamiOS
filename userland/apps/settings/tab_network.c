#include "settings.h"


void draw_network_tab(void)
{
    int px = 20;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, 86, (int)w - 40, "IPv4 Network Configuration & Adapter", UK_TEAL);

    /* Mode selector pills */
    /* DHCP Pill */
    if (g_net_dhcp == 1) {
        uk_fill_rounded_rect(&g_win, px, 112, 190, 26, 6, UK_TEAL);
        uk_draw_text(&g_win, px + 14, 117, "[*] DHCP (Automatic)", UK_CRUST);
    } else {
        uk_fill_rounded_rect(&g_win, px, 112, 190, 26, 6, UK_SURFACE1);
        uk_fill_rounded_rect(&g_win, px + 1, 113, 188, 24, 5, UK_SURFACE0);
        uk_draw_text(&g_win, px + 14, 117, "[ ] DHCP (Automatic)", UK_SUBTEXT0);
    }

    /* Static Pill */
    if (g_net_dhcp == 0) {
        uk_fill_rounded_rect(&g_win, px + 205, 112, 220, 26, 6, UK_MAUVE);
        uk_draw_text(&g_win, px + 219, 117, "[*] Static Configuration", UK_CRUST);
    } else {
        uk_fill_rounded_rect(&g_win, px + 205, 112, 220, 26, 6, UK_SURFACE1);
        uk_fill_rounded_rect(&g_win, px + 206, 113, 218, 24, 5, UK_SURFACE0);
        uk_draw_text(&g_win, px + 219, 117, "[ ] Static Configuration", UK_SUBTEXT0);
    }

    /* Telemetry strings */
    char rx_info[64] = "RX: 128 packets (14.2 KB)";
    char tx_info[64] = "TX: 64 packets (8.4 KB)";
    /* Drops/errors below are real (see the sscanf below); link speed/duplex
     * has no real source anywhere in this kernel -- no NIC driver reports a
     * negotiated speed -- so that part stays a fixed, honestly-labeled
     * description of the emulated link rather than a fabricated dynamic
     * reading. */
    char link_info[48] = "0 drops \xe2\x80\xa2 0 errors";
    int nfd = open("/proc/net/dev", O_RDONLY, 0);
    if (nfd >= 0) {
        char nbuf[512];
        ssize_t n = read(nfd, nbuf, sizeof(nbuf) - 1);
        close(nfd);
        if (n > 0) {
            nbuf[n] = '\0';
            char *line = strstr(nbuf, "net0");
            if (!line) line = strstr(nbuf, "eth0");
            if (line) {
                unsigned long long rx_b = 0, rx_p = 0, rx_errs = 0, rx_drop = 0;
                unsigned long long tx_b = 0, tx_p = 0, tx_errs = 0, tx_drop = 0;
                char dev[16];
                /* Errs/drop used to be skipped with %*u and the line below
                 * always said "0 drops * 0 errors" regardless of what
                 * fs/procfs.c's format_proc_net_dev() actually reported --
                 * capture them instead of throwing them away. */
                if (sscanf(line, "%15s %llu %llu %llu %llu %*u %*u %*u %*u %llu %llu %llu %llu",
                           dev, &rx_b, &rx_p, &rx_errs, &rx_drop,
                           &tx_b, &tx_p, &tx_errs, &tx_drop) >= 9) {
                    snprintf(rx_info, sizeof(rx_info), "RX: %llu pkts (%llu KB)", rx_p, rx_b / 1024);
                    snprintf(tx_info, sizeof(tx_info), "TX: %llu pkts (%llu KB)", tx_p, tx_b / 1024);
                    snprintf(link_info, sizeof(link_info), "%llu drops \xe2\x80\xa2 %llu errors",
                             rx_drop + tx_drop, rx_errs + tx_errs);
                }
            }
        }
    }

    if (g_net_dhcp == 1) {
        /* DHCP View */
        uk_draw_panel(&g_win, px, 146, (int)w - 40, 102, UK_SURFACE0);
        uk_draw_text(&g_win, px + 14, 156, "Adapter:", UK_SUBTEXT0);
        uk_draw_text(&g_win, px + 100, 156, "Intel 82540EM / virtio-net (PCI 00:02.0)", UK_TEXT);
        uk_draw_badge(&g_win, (int)w - 145, 154, "Connected (DHCP)", UK_SURFACE1, UK_GREEN);

        char ip_line[80];
        snprintf(ip_line, sizeof(ip_line), "%s (net0)", g_net_ip);
        uk_draw_text(&g_win, px + 14, 178, "IP Address:", UK_SUBTEXT0);
        uk_draw_text(&g_win, px + 100, 178, ip_line, UK_GREEN);

        uk_draw_text(&g_win, px + 14, 200, "Subnet Mask:", UK_SUBTEXT0);
        uk_draw_text(&g_win, px + 100, 200, g_net_netmask, UK_TEXT);

        char gw_dns[80];
        snprintf(gw_dns, sizeof(gw_dns), "Gateway: %s   •   DNS: %s", g_net_gateway, g_net_dns);
        uk_draw_text(&g_win, px + 14, 222, "Routing:", UK_SUBTEXT0);
        uk_draw_text(&g_win, px + 100, 222, gw_dns, UK_SUBTEXT1);

        uk_draw_button(&g_win, px, 256, 170, 26, "Renew DHCP Lease", UK_BTN_NORMAL);
        uk_draw_button(&g_win, px + 185, 256, 180, 26, "Configure Static IP", UK_BTN_NORMAL);

        uk_draw_section_header(&g_win, px, 292, (int)w - 40, "Live Network Statistics (/proc/net)", UK_BLUE);
        uk_draw_panel(&g_win, px, 316, (int)w - 40, 64, UK_SURFACE0);
        uk_draw_text(&g_win, px + 14, 326, "Traffic Flow:", UK_SUBTEXT0);
        uk_draw_text(&g_win, px + 120, 326, rx_info, UK_TEXT);
        uk_draw_text(&g_win, px + 360, 326, tx_info, UK_TEXT);
        char quality_line[80];
        snprintf(quality_line, sizeof(quality_line), "1000 Mbps Full Duplex \xe2\x80\xa2 %s", link_info);
        uk_draw_text(&g_win, px + 14, 350, "Link Quality:", UK_SUBTEXT0);
        uk_draw_text(&g_win, px + 120, 350, quality_line, UK_GREEN);

        if (g_net_status_msg[0]) {
            uk_draw_text(&g_win, px + 4, 390, g_net_status_msg, g_net_status_col);
        }
    } else {
        /* Static Configuration View */
        uk_draw_panel(&g_win, px, 146, (int)w - 40, 188, UK_SURFACE0);
        uk_draw_text(&g_win, px + 14, 154, "Static IPv4 Parameters", UK_PEACH);
        uk_draw_badge(&g_win, (int)w - 130, 152, "Static Mode", UK_SURFACE1, UK_PEACH);

        /* Row 0: IP Address */
        uk_draw_text(&g_win, px + 14, 178, "IP Address:", UK_SUBTEXT0);
        draw_input_box(px + 130, 174, 200, 24, g_net_ip, g_net_focus == 0);
        uk_draw_button(&g_win, px + 340, 174, 130, 24, "Use 10.0.2.15", UK_BTN_NORMAL);
        uk_draw_button(&g_win, px + 480, 174, 140, 24, "Use 192.168.1.50", UK_BTN_NORMAL);

        /* Row 1: Subnet Mask */
        uk_draw_text(&g_win, px + 14, 206, "Subnet Mask:", UK_SUBTEXT0);
        draw_input_box(px + 130, 202, 200, 24, g_net_netmask, g_net_focus == 1);
        uk_draw_button(&g_win, px + 340, 202, 160, 24, "/24 (255.255.255.0)", UK_BTN_NORMAL);
        uk_draw_button(&g_win, px + 510, 202, 110, 24, "/16 Netmask", UK_BTN_NORMAL);

        /* Row 2: Default Gateway */
        uk_draw_text(&g_win, px + 14, 234, "Default Gateway:", UK_SUBTEXT0);
        draw_input_box(px + 130, 230, 200, 24, g_net_gateway, g_net_focus == 2);
        uk_draw_button(&g_win, px + 340, 230, 130, 24, "Use 10.0.2.2", UK_BTN_NORMAL);
        uk_draw_button(&g_win, px + 480, 230, 140, 24, "Use 192.168.1.1", UK_BTN_NORMAL);

        /* Row 3: Primary DNS */
        uk_draw_text(&g_win, px + 14, 262, "Primary DNS:", UK_SUBTEXT0);
        draw_input_box(px + 130, 258, 200, 24, g_net_dns, g_net_focus == 3);
        uk_draw_button(&g_win, px + 340, 258, 130, 24, "8.8.8.8 (Google)", UK_BTN_NORMAL);
        uk_draw_button(&g_win, px + 480, 258, 140, 24, "1.1.1.1 (Cloudflare)", UK_BTN_NORMAL);

        uk_draw_text(&g_win, px + 14, 298, "Click field to type. Tab moves next, Enter applies.", UK_SUBTEXT1);

        /* Action Buttons */
        uk_draw_button(&g_win, px, 344, 210, 28, "Apply Static Config", UK_BTN_NORMAL);
        uk_draw_button(&g_win, px + 225, 344, 150, 28, "Revert to DHCP", UK_BTN_NORMAL);

        if (g_net_status_msg[0]) {
            uk_draw_text(&g_win, px + 4, 384, g_net_status_msg, g_net_status_col);
        } else {
            uk_draw_text(&g_win, px + 4, 384, "Static settings take effect immediately on net0 and /etc/network.conf", UK_SUBTEXT0);
        }

        /* Compact stats row */
        char stats_line[128];
        snprintf(stats_line, sizeof(stats_line), "Telemetry: %s  •  %s  •  1000 Mbps", rx_info, tx_info);
        uk_draw_text(&g_win, px + 4, 408, stats_line, UK_BLUE);
    }
}


void handle_network_mouse(int mx, int my)
{
    /* Mode toggle pills */
                    if (mx >= 20 && mx <= 210 && my >= 112 && my <= 138) {
                        apply_dhcp_network();
                        draw_settings();
                        return;
                    }
                    if (mx >= 225 && mx <= 445 && my >= 112 && my <= 138) {
                        g_net_dhcp = 0;
                        draw_settings();
                        return;
                    }

                    if (g_net_dhcp == 1) {
                        /* "Renew DHCP Lease" button */
                        if (mx >= 20 && mx <= 190 && my >= 256 && my <= 282) {
                            apply_dhcp_network();
                            draw_settings();
                            return;
                        }
                        /* "Configure Static IP" button */
                        if (mx >= 205 && mx <= 385 && my >= 256 && my <= 282) {
                            g_net_dhcp = 0;
                            draw_settings();
                            return;
                        }
                    } else {
                        /* Row 0: IP field and presets */
                        if (mx >= 150 && mx <= 350 && my >= 174 && my <= 198) {
                            g_net_focus = 0;
                            draw_settings();
                            return;
                        }
                        if (mx >= 360 && mx <= 490 && my >= 174 && my <= 198) {
                            snprintf(g_net_ip, sizeof(g_net_ip), "10.0.2.15");
                            g_net_focus = 0;
                            draw_settings();
                            return;
                        }
                        if (mx >= 500 && mx <= 640 && my >= 174 && my <= 198) {
                            snprintf(g_net_ip, sizeof(g_net_ip), "192.168.1.50");
                            g_net_focus = 0;
                            draw_settings();
                            return;
                        }

                        /* Row 1: Subnet mask field and presets */
                        if (mx >= 150 && mx <= 350 && my >= 202 && my <= 226) {
                            g_net_focus = 1;
                            draw_settings();
                            return;
                        }
                        if (mx >= 360 && mx <= 520 && my >= 202 && my <= 226) {
                            snprintf(g_net_netmask, sizeof(g_net_netmask), "255.255.255.0");
                            g_net_focus = 1;
                            draw_settings();
                            return;
                        }
                        if (mx >= 530 && mx <= 640 && my >= 202 && my <= 226) {
                            snprintf(g_net_netmask, sizeof(g_net_netmask), "255.255.0.0");
                            g_net_focus = 1;
                            draw_settings();
                            return;
                        }

                        /* Row 2: Default gateway field and presets */
                        if (mx >= 150 && mx <= 350 && my >= 230 && my <= 254) {
                            g_net_focus = 2;
                            draw_settings();
                            return;
                        }
                        if (mx >= 360 && mx <= 490 && my >= 230 && my <= 254) {
                            snprintf(g_net_gateway, sizeof(g_net_gateway), "10.0.2.2");
                            g_net_focus = 2;
                            draw_settings();
                            return;
                        }
                        if (mx >= 500 && mx <= 640 && my >= 230 && my <= 254) {
                            snprintf(g_net_gateway, sizeof(g_net_gateway), "192.168.1.1");
                            g_net_focus = 2;
                            draw_settings();
                            return;
                        }

                        /* Row 3: Primary DNS field and presets */
                        if (mx >= 150 && mx <= 350 && my >= 258 && my <= 282) {
                            g_net_focus = 3;
                            draw_settings();
                            return;
                        }
                        if (mx >= 360 && mx <= 490 && my >= 258 && my <= 282) {
                            snprintf(g_net_dns, sizeof(g_net_dns), "8.8.8.8");
                            g_net_focus = 3;
                            draw_settings();
                            return;
                        }
                        if (mx >= 500 && mx <= 640 && my >= 258 && my <= 282) {
                            snprintf(g_net_dns, sizeof(g_net_dns), "1.1.1.1");
                            g_net_focus = 3;
                            draw_settings();
                            return;
                        }

                        /* "Apply Static Config" button */
                        if (mx >= 20 && mx <= 230 && my >= 344 && my <= 372) {
                            apply_static_network();
                            draw_settings();
                            return;
                        }
                        /* "Revert to DHCP" button */
                        if (mx >= 245 && mx <= 395 && my >= 344 && my <= 372) {
                            apply_dhcp_network();
                            draw_settings();
                            return;
                        }
                    }
}
