#include "settings.h"


void draw_power_tab(void)
{
    int px = 20;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, 86, (int)w - 40, "System Power & Energy Profiles", UK_YELLOW);

    /* 3 Profile Cards */
    const char *pnames[3] = { "Performance", "Balanced", "Power Saver" };
    const char *pdescs[3] = { "Max clock & I/O speed", "Adaptive energy balance", "Max battery conservation" };
    int card_w = ((int)w - 40 - 24) / 3;
    for (int i = 0; i < 3; i++) {
        int cx = px + i * (card_w + 12);
        int cy = 114;
        bool is_sel = (g_power_profile == i);
        unsigned int bg_col = is_sel ? UK_SURFACE1 : UK_SURFACE0;
        unsigned int border_col = is_sel ? UK_YELLOW : UK_SURFACE1;

        uk_fill_rounded_rect(&g_win, cx, cy, card_w, 48, 6, bg_col);
        uk_draw_rounded_rect_outline(&g_win, cx, cy, card_w, 48, 6, border_col);
        if (is_sel) {
            uk_fill_rounded_rect(&g_win, cx + 2, cy + 2, 4, 44, 2, UK_YELLOW);
            uk_draw_badge(&g_win, cx + card_w - 56, cy + 8, "Active", UK_SURFACE0, UK_YELLOW);
        }
        uk_draw_text(&g_win, cx + 12, cy + 8, pnames[i], is_sel ? UK_TEXT : UK_SUBTEXT1);
        uk_draw_text(&g_win, cx + 12, cy + 26, pdescs[i], UK_OVERLAY0);
    }

    uk_draw_section_header(&g_win, px, 172, (int)w - 40, "Display Sleep & Inactivity Timeout", UK_BLUE);

    int timeouts[4] = { 5, 15, 30, 0 };
    const char *tlabels[4] = { "5 Minutes", "15 Minutes", "30 Minutes", "Never" };
    int pill_w = ((int)w - 40 - 36) / 4;
    for (int i = 0; i < 4; i++) {
        int tx = px + i * (pill_w + 12);
        int ty = 200;
        bool is_sel = (g_screen_timeout == timeouts[i]);
        unsigned int bg_col = is_sel ? UK_MAUVE : UK_SURFACE0;
        unsigned int fg_col = is_sel ? UK_BASE : UK_TEXT;

        uk_fill_rounded_rect(&g_win, tx, ty, pill_w, 26, 4, bg_col);
        uk_draw_rounded_rect_outline(&g_win, tx, ty, pill_w, 26, 4, is_sel ? UK_MAUVE : UK_SURFACE1);
        int slen = uk_strlen(tlabels[i]);
        uk_draw_text(&g_win, tx + (pill_w - slen * 8) / 2, ty + 5, tlabels[i], fg_col);
    }

    uk_draw_section_header(&g_win, px, 236, (int)w - 40, "ACPI Hardware & Subsystem Telemetry", UK_GREEN);
    uk_draw_panel(&g_win, px, 264, (int)w - 40, 56, UK_SURFACE0);
    uk_draw_text(&g_win, px + 12, 270, "ACPI Controller: Intel PIIX4 Power Management Interface (I/O 0xB000)", UK_TEXT);
    uk_draw_text(&g_win, px + 12, 286, "PM Timer Clock : 3.579545 MHz High-Precision 24-bit Counter (Fixed Rate)", UK_SUBTEXT0);
    uk_draw_text(&g_win, px + 12, 302, "System Power   : AC Mains Online (ACPI S0 working, S5 soft-off)", UK_GREEN);

    uk_draw_section_header(&g_win, px, 330, (int)w - 40, "System Power Actions", UK_PEACH);

    uk_draw_button(&g_win, px, 358, 140, 30, "Blank Display", UK_BTN_NORMAL);
    uk_draw_button(&g_win, px + 152, 358, 140, 30, "Restart System", UK_BTN_NORMAL);
    uk_draw_button(&g_win, px + 304, 358, 140, 30, "Power Off", UK_BTN_NORMAL);

    uk_draw_panel(&g_win, px, 400, (int)w - 40, 26, UK_SURFACE0);
    uk_draw_text(&g_win, px + 10, 405, g_power_status_msg, UK_TEXT);
}


void handle_power_mouse(int mx, int my)
{
    unsigned int w = g_win.width, h = g_win.height;
    (void)w; (void)h;
    /* Profile cards (y: 114..162) */
                    int card_w = ((int)w - 40 - 24) / 3;
                    if (my >= 114 && my <= 162) {
                        for (int i = 0; i < 3; i++) {
                            int cx = 20 + i * (card_w + 12);
                            if (mx >= cx && mx <= cx + card_w) {
                                apply_power_profile(i);
                                draw_settings();
                                break;
                            }
                        }
                        return;
                    }
                    /* Timeout pills (y: 200..226) */
                    int timeouts[4] = { 5, 15, 30, 0 };
                    int pill_w = ((int)w - 40 - 36) / 4;
                    if (my >= 200 && my <= 226) {
                        for (int i = 0; i < 4; i++) {
                            int tx = 20 + i * (pill_w + 12);
                            if (mx >= tx && mx <= tx + pill_w) {
                                apply_screen_timeout(timeouts[i]);
                                draw_settings();
                                break;
                            }
                        }
                        return;
                    }
                    /* Power buttons (y: 358..388) */
                    if (my >= 358 && my <= 388) {
                        if (mx >= 20 && mx <= 160) {
                            /* This used to print "Entering ACPI S3 Standby
                             * state..." and do nothing at all — the kernel
                             * implements S5 (soft off) and no sleep states.
                             * Blanking the display is the part that is real,
                             * so that is what the button does and what it is
                             * now called: the compositor turns the screen
                             * black and the next key or mouse event brings
                             * it back (see AZ_WM_BLANK_SCREEN in azwm). */
                            az_wm_msg_t bmsg;
                            memset(&bmsg, 0, sizeof(bmsg));
                            bmsg.type = AZ_WM_BLANK_SCREEN;
                            bmsg.wid  = g_win.wid;
                            az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&bmsg);
                            snprintf(g_power_status_msg, sizeof(g_power_status_msg),
                                     "Display blanked — press a key or move the mouse to wake.");
                            draw_settings();
                            return;
                        }
                        if (mx >= 172 && mx <= 312) {
                            snprintf(g_power_status_msg, sizeof(g_power_status_msg), "Initiating system reboot...");
                            draw_settings();
                            reboot(RB_AUTOBOOT);
                            return;
                        }
                        if (mx >= 324 && mx <= 464) {
                            snprintf(g_power_status_msg, sizeof(g_power_status_msg), "Initiating ACPI poweroff...");
                            draw_settings();
                            reboot(RB_POWER_OFF);
                            return;
                        }
                    }
}
