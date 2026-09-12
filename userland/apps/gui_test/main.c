/* ============================================================================
 * AzamiOS — Component & Compositor Studio (gui_test)
 * File: userland/apps/gui_test/main.c
 *
 * Interactive studio showcasing:
 *  • Modern UI Kit widgets (segmented control, search bar, dropdown, metric cards,
 *    avatars, spinners, progress bars, toggle buttons, badges)
 *  • Advanced GFX Pipeline primitives (radial gradient, diagonal gradient,
 *    thick rounded outlines, ambient glow)
 *  • Window Manager APIs (live window opacity slider, dynamic titlebar switching,
 *    pin always-on-top, dynamic cursor type switching)
 * ============================================================================ */

#include <stdbool.h>
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/gfx_pipeline.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN 1
#define WIN_W       680
#define WIN_H       480
#define MAP_ADDR    ((void *)0x67000000)

static uk_window_t g_win;

/* ── Studio State ──────────────────────────────────────────────────────────── */
static int  g_tab = 0;              /* 0: Widgets, 1: Compositor & FX, 2: Cursor Studio */
static int  g_hover_tab = -1;
static bool g_pinned = false;
static int  g_opacity_pct = 100;    /* 30% to 100% */
static int  g_selected_cursor = AZ_CURSOR_DEFAULT;
static int  g_spinner_step = 0;
static char g_search_query[48] = "";
static bool g_search_focused = false;
static bool g_dropdown_open = false;
static int  g_dropdown_selected = 0;
static bool g_toggle_state = true;
static int  g_progress_pct = 65;
static int  g_title_idx = 0;
static int  g_mx = 0, g_my = 0;
static bool g_mouse_down = false;

static const char *k_tab_names[] = {
    "UI Widgets",
    "Compositor & FX",
    "Cursor Studio",
    "Auto-Accept Lab"
};
#define NUM_TABS 4

static const char *k_titles[] = {
    "AzamiOS Studio",
    "Compositor Playground",
    "Graphics Pipeline Demo",
    "Component Showcase"
};
#define NUM_TITLES 4

static const char *k_dropdown_options[] = {
    "Catppuccin Mocha (Dark)",
    "Nord Arctic (Frost)",
    "Cyberpunk Neon (Vibrant)",
    "OLED Pure Dark"
};
#define NUM_DROPDOWN 4

static const struct {
    unsigned int id;
    const char  *name;
    const char  *desc;
} k_cursor_catalog[] = {
    { AZ_CURSOR_DEFAULT,     "Default",       "Standard arrow" },
    { AZ_CURSOR_POINTER,     "Pointer Hand", "Link / clickable" },
    { AZ_CURSOR_IBEAM,       "Text I-Beam",  "Text selection" },
    { AZ_CURSOR_CROSSHAIR,   "Crosshair",     "Precision target" },
    { AZ_CURSOR_MOVE,        "Move 4-Way",    "Window / viewport drag" },
    { AZ_CURSOR_RESIZE_NWSE, "Resize NW-SE",  "Diagonal corner resize" },
    { AZ_CURSOR_RESIZE_NESW, "Resize NE-SW",  "Diagonal corner resize" },
    { AZ_CURSOR_RESIZE_EW,   "Resize E-W",    "Horizontal edge resize" },
    { AZ_CURSOR_RESIZE_NS,   "Resize N-S",    "Vertical edge resize" },
    { AZ_CURSOR_WAIT,        "Wait Spinner",  "Background task busy" },
};
#define NUM_CURSORS 10

/* ── UI Drawing Functions ──────────────────────────────────────────────────── */

static void draw_tab_widgets(void)
{
    /* Left column: Metrics & Search & Dropdown */
    uk_draw_metric_card(&g_win, 24, 76, 196, 88,
                        "RENDER PIPELINE", "60 FPS", "+ VSync Locked", UK_GREEN);

    uk_draw_metric_card(&g_win, 236, 76, 196, 88,
                        "SHARED MEMORY", "4,096 KB", "Zero-Copy IPC", UK_SAPPHIRE);

    uk_draw_metric_card(&g_win, 448, 76, 208, 88,
                        "COMPOSITING MODE", "HW Blit", "Hardware Accel", UK_MAUVE);

    /* Search bar */
    uk_draw_text_bold(&g_win, 24, 182, "Modern Search Bar:", UK_SUBTEXT1);
    bool search_hover = (g_mx >= 24 && g_mx <= 340 && g_my >= 204 && g_my <= 236);
    uk_draw_search_bar(&g_win, 24, 204, 316, 32,
                       "Search components, APIs...", g_search_query, g_search_focused, search_hover);

    /* Dropdown Picker */
    uk_draw_text_bold(&g_win, 360, 182, "Dropdown Select Menu:", UK_SUBTEXT1);
    bool dd_hover = (g_mx >= 360 && g_mx <= 656 && g_my >= 204 && g_my <= 236);
    uk_draw_dropdown_select(&g_win, 360, 204, 296, 32,
                            k_dropdown_options[g_dropdown_selected], g_dropdown_open, dd_hover);

    /* Section Divider */
    uk_draw_divider_text(&g_win, 24, 252, WIN_W - 48, "INTERACTIVE CONTROLS");

    /* Buttons row */
    uk_btn_state_t btn1_state = (g_mx >= 24 && g_mx <= 134 && g_my >= 280 && g_my <= 312)
                                 ? (g_mouse_down ? UK_BTN_PRESSED : UK_BTN_HOVER) : UK_BTN_NORMAL;
    uk_draw_button(&g_win, 24, 280, 110, 32, "Primary", btn1_state);

    uk_btn_state_t btn2_state = (g_mx >= 148 && g_mx <= 258 && g_my >= 280 && g_my <= 312)
                                 ? (g_mouse_down ? UK_BTN_PRESSED : UK_BTN_HOVER) : UK_BTN_NORMAL;
    uk_draw_button(&g_win, 148, 280, 110, 32, "Action", btn2_state);

    uk_draw_button(&g_win, 272, 280, 110, 32, "Disabled", UK_BTN_DISABLED);

    /* Toggle & Progress */
    uk_draw_text(&g_win, 410, 288, "Dark Mode:", UK_TEXT);
    uk_draw_toggle_modern(&g_win, 500, 285, g_toggle_state ? 1 : 0);

    uk_draw_text(&g_win, 24, 336, "Buffer Progress:", UK_SUBTEXT0);
    uk_draw_progress_bar_modern(&g_win, 160, 334, 280, 16, g_progress_pct, UK_BLUE);
    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%d%%", g_progress_pct);
    uk_draw_text(&g_win, 450, 334, pct_str, UK_SUBTEXT1);

    /* Avatar and Badges */
    uk_draw_text(&g_win, 24, 380, "Avatars & Badges:", UK_SUBTEXT0);
    uk_draw_avatar(&g_win, 180, 396, 16, "AZ", UK_MAUVE);
    uk_draw_avatar(&g_win, 224, 396, 16, "OS", UK_SAPPHIRE);
    uk_draw_avatar(&g_win, 268, 396, 16, "UI", UK_GREEN);

    uk_draw_badge(&g_win, 310, 387, "ACTIVE", UK_GREEN, UK_BASE);
    uk_draw_badge(&g_win, 376, 387, "PIPELINE V2", UK_BLUE, UK_BASE);
    uk_draw_badge(&g_win, 482, 387, "ZERO-COPY", UK_MAUVE, UK_BASE);

    /* Animated Spinner */
    uk_draw_text(&g_win, 24, 432, "Background Spinner:", UK_SUBTEXT0);
    uk_draw_spinner(&g_win, 196, 440, 12, g_spinner_step, UK_PEACH);
    uk_draw_text(&g_win, 220, 432, "Rasterizer thread active...", UK_SUBTEXT1);

    /* Dropdown popup if open */
    if (g_dropdown_open) {
        int dpx = 360, dpy = 238, dpw = 296, dph = NUM_DROPDOWN * 26 + 8;
        uk_fill_rounded_rect(&g_win, dpx + 1, dpy + 2, dpw, dph, 6, 0x40000000);
        uk_fill_rounded_rect(&g_win, dpx, dpy, dpw, dph, 6, UK_SURFACE0);
        uk_draw_rounded_rect_outline(&g_win, dpx, dpy, dpw, dph, 6, UK_BLUE);
        for (int i = 0; i < NUM_DROPDOWN; i++) {
            int row_y = dpy + 4 + i * 26;
            bool row_hov = (g_mx >= dpx && g_mx < dpx + dpw && g_my >= row_y && g_my < row_y + 26);
            if (row_hov) {
                uk_fill_rounded_rect(&g_win, dpx + 4, row_y, dpw - 8, 24, 4, UK_SURFACE2);
            }
            unsigned int col = (i == g_dropdown_selected) ? UK_BLUE : (row_hov ? UK_TEXT : UK_SUBTEXT1);
            uk_draw_text(&g_win, dpx + 12, row_y + 4, k_dropdown_options[i], col);
        }
    }
}

static void draw_tab_compositor(void)
{
    /* Window Opacity control */
    uk_draw_panel(&g_win, 24, 76, WIN_W - 48, 100, UK_BASE);
    uk_draw_section_header(&g_win, 36, 88, WIN_W - 72, "Window Alpha & Transparency (Live IPC)", UK_BLUE);

    uk_draw_text(&g_win, 36, 120, "Window Opacity:", UK_TEXT);
    uk_draw_slider(&g_win, 170, 120, 360, g_opacity_pct, UK_BLUE);
    char op_buf[16];
    snprintf(op_buf, sizeof(op_buf), "%d%%", g_opacity_pct);
    uk_draw_text_bold(&g_win, 550, 120, op_buf, UK_TEXT);

    uk_draw_text_small(&g_win, 36, 148,
        "Changes window alpha instantly via AZ_WM_SET_OPACITY with background compositing.", UK_OVERLAY1);

    /* Dynamic Window Title */
    uk_draw_panel(&g_win, 24, 190, WIN_W - 48, 110, UK_BASE);
    uk_draw_section_header(&g_win, 36, 202, WIN_W - 72, "Dynamic Titlebar & Taskbar Sync", UK_MAUVE);

    uk_draw_text(&g_win, 36, 234, "Select Title Preset:", UK_TEXT);
    for (int i = 0; i < NUM_TITLES; i++) {
        int bx = 36 + i * 148;
        int by = 256;
        uk_btn_state_t st = (i == g_title_idx) ? UK_BTN_PRESSED :
                            ((g_mx >= bx && g_mx <= bx + 138 && g_my >= by && g_my <= by + 28) ? UK_BTN_HOVER : UK_BTN_NORMAL);
        uk_draw_button(&g_win, bx, by, 138, 28, k_titles[i], st);
    }

    /* 2D Graphics Pipeline Showcase: Radial & Diagonal Gradients */
    uk_draw_panel(&g_win, 24, 314, WIN_W - 48, 146, UK_BASE);
    uk_draw_section_header(&g_win, 36, 326, WIN_W - 72, "2D Pipeline Shaders: Radial, Diagonal & Glow", UK_PEACH);

    /* Construct temporary gfx_surface_t on window pixels */
    gfx_surface_t surf;
    gfx_surface_init(&surf, g_win.pixels, g_win.width, g_win.height, g_win.width * 4);
    surf.clip.x = 0; surf.clip.y = 0; surf.clip.w = g_win.width; surf.clip.h = g_win.height;

    /* Radial Gradient Card */
    uk_draw_text_small(&g_win, 36, 356, "Radial Gradient Shader", UK_SUBTEXT0);
    gfx_draw_gradient_radial(&surf, 110, 404, 40, 0xFFF38BA8, 0xFF1E1E2E);
    gfx_draw_rounded_rect_outline_thick(&surf, 40, 368, 140, 72, 8, 2, UK_SURFACE1);

    /* Diagonal Gradient Card */
    uk_draw_text_small(&g_win, 220, 356, "Diagonal Shader (TL->BR)", UK_SUBTEXT0);
    gfx_draw_gradient_diagonal(&surf, 220, 372, 160, 64, 0xFF89B4FA, 0xFFCBA6F7);
    gfx_draw_rounded_rect_outline_thick(&surf, 220, 372, 160, 64, 6, 2, UK_BLUE);

    /* Ambient Pill Glow */
    uk_draw_text_small(&g_win, 420, 356, "Ambient Pill Glow Primitive", UK_SUBTEXT0);
    gfx_draw_pill_glow(&surf, 440, 388, 170, 32, 0xFFA6E3A1, 6);
    uk_fill_rounded_rect(&g_win, 440, 388, 170, 32, 16, UK_GREEN);
    uk_draw_text_bold(&g_win, 476, 396, "GLOW BADGE", UK_BASE);
}

static void draw_tab_cursors(void)
{
    uk_draw_panel(&g_win, 24, 76, WIN_W - 48, 80, UK_BASE);
    uk_draw_section_header(&g_win, 36, 88, WIN_W - 72, "Hardware & Software Multi-Cursor Engine", UK_YELLOW);

    char cur_info[96];
    snprintf(cur_info, sizeof(cur_info), "Active Cursor: %s  (#%d) — %s",
             k_cursor_catalog[g_selected_cursor].name,
             g_selected_cursor,
             k_cursor_catalog[g_selected_cursor].desc);
    uk_draw_text_bold(&g_win, 36, 120, cur_info, UK_TEXT);
    uk_draw_text_small(&g_win, 36, 138,
        "Hover or click any cursor card below. Window automatically sets cursor via uk_set_cursor().", UK_OVERLAY1);

    /* 2x5 Grid of Cursor Cards */
    for (int i = 0; i < NUM_CURSORS; i++) {
        int col = i % 2;
        int row = i / 2;
        int cx = 24 + col * 318;
        int cy = 168 + row * 56;
        int cw = 306;
        int ch = 48;

        bool hovered = (g_mx >= cx && g_mx <= cx + cw && g_my >= cy && g_my <= cy + ch);
        bool selected = (g_selected_cursor == (int)k_cursor_catalog[i].id);

        unsigned int bg = selected ? UK_SURFACE1 : (hovered ? UK_SURFACE0 : UK_MANTLE);
        unsigned int border = selected ? UK_YELLOW : (hovered ? UK_LAVENDER : UK_SURFACE1);

        uk_fill_rounded_rect(&g_win, cx, cy, cw, ch, 6, bg);
        uk_draw_rounded_rect_outline(&g_win, cx, cy, cw, ch, 6, border);

        /* Cursor ID Badge */
        uk_fill_rounded_rect(&g_win, cx + 10, cy + 10, 28, 28, 6, selected ? UK_YELLOW : UK_SURFACE2);
        char id_str[4];
        snprintf(id_str, sizeof(id_str), "%d", i);
        uk_draw_text_bold(&g_win, cx + 20, cy + 16, id_str, selected ? UK_BASE : UK_TEXT);

        /* Title & Description */
        uk_draw_text_bold(&g_win, cx + 48, cy + 8, k_cursor_catalog[i].name, selected ? UK_YELLOW : UK_TEXT);
        uk_draw_text_small(&g_win, cx + 48, cy + 28, k_cursor_catalog[i].desc, UK_OVERLAY1);

        if (selected) {
            uk_draw_badge(&g_win, cx + cw - 70, cy + 14, "ACTIVE", UK_YELLOW, UK_BASE);
        }
    }
}

/* ── Auto-Accept Lab Tab ─────────────────────────────────────────────────── */
static bool g_aa_ipc = true;
static bool g_aa_admin = true;
static bool g_aa_dhcp = true;
static bool g_aa_trace = true;
static int  g_sim_events = 0;
static const char *g_sim_status = "Standby. Click 'Simulate Action' to test execution gate.";
static unsigned int g_sim_status_col = UK_SUBTEXT0;

static void draw_tab_autoaccept(void)
{
    /* Top: Policy Enforcement & Capability Matrix */
    uk_draw_panel(&g_win, 24, 76, WIN_W - 48, 148, UK_BASE);
    uk_draw_section_header(&g_win, 36, 88, WIN_W - 72, "Auto-Accept & Unattended Policy Matrix", UK_MAUVE);

    /* 4 Toggles in 2 columns */
    uk_draw_text(&g_win, 36, 122, "IPC Capability Passes (SCM_RIGHTS):", UK_TEXT);
    uk_draw_toggle_modern(&g_win, 280, 118, g_aa_ipc ? 1 : 0);

    uk_draw_text(&g_win, 36, 156, "Unattended Desktop Admin Tasks:", UK_TEXT);
    uk_draw_toggle_modern(&g_win, 280, 152, g_aa_admin ? 1 : 0);

    uk_draw_text(&g_win, 350, 122, "DHCP Network Transitions:", UK_TEXT);
    uk_draw_toggle_modern(&g_win, 590, 118, g_aa_dhcp ? 1 : 0);

    uk_draw_text(&g_win, 350, 156, "Kernel Tracing Probes (ktrace):", UK_TEXT);
    uk_draw_toggle_modern(&g_win, 590, 152, g_aa_trace ? 1 : 0);

    uk_draw_text_small(&g_win, 36, 192,
        "Policies dynamically control whether OS operations bypass interactive elevation blocks.", UK_OVERLAY1);

    /* Middle: Live Capability Test & Simulation Sandbox */
    uk_draw_panel(&g_win, 24, 236, WIN_W - 48, 130, UK_BASE);
    uk_draw_section_header(&g_win, 36, 248, WIN_W - 72, "Action Simulation & Execution Gate", UK_GREEN);

    int btn_x = 36, btn_y = 280, btn_w = 200, btn_h = 32;
    uk_btn_state_t bst = (g_mx >= btn_x && g_mx <= btn_x + btn_w && g_my >= btn_y && g_my <= btn_y + btn_h)
                         ? (g_mouse_down ? UK_BTN_PRESSED : UK_BTN_HOVER) : UK_BTN_NORMAL;
    uk_draw_button(&g_win, btn_x, btn_y, btn_w, btn_h, "Simulate Action", bst);

    /* Badge & status */
    if (g_aa_admin) {
        uk_draw_badge(&g_win, 250, 286, "AUTO-ACCEPT: ON", UK_GREEN, UK_BASE);
    } else {
        uk_draw_badge(&g_win, 250, 286, "AUTO-ACCEPT: OFF", UK_PEACH, UK_BASE);
    }

    char ev_buf[32];
    snprintf(ev_buf, sizeof(ev_buf), "Tested: %d runs", g_sim_events);
    uk_draw_text_bold(&g_win, 420, 288, ev_buf, UK_BLUE);

    uk_draw_text(&g_win, 36, 326, g_sim_status, g_sim_status_col);

    /* Bottom: Kernel Hardware Mitigations */
    uk_draw_panel(&g_win, 24, 378, WIN_W - 48, 80, UK_BASE);
    uk_draw_section_header(&g_win, 36, 390, WIN_W - 72, "Kernel Hardware Protection Telemetry", UK_SAPPHIRE);

    uk_draw_badge(&g_win, 36, 420, "SMEP", UK_GREEN, UK_BASE);
    uk_draw_badge(&g_win, 90, 420, "SMAP", UK_GREEN, UK_BASE);
    uk_draw_badge(&g_win, 144, 420, "UMIP", UK_GREEN, UK_BASE);
    uk_draw_badge(&g_win, 198, 420, "CANARY", UK_GREEN, UK_BASE);
    uk_draw_badge(&g_win, 268, 420, "YAMA", UK_GREEN, UK_BASE);
    uk_draw_badge(&g_win, 322, 420, "LINKGUARD", UK_GREEN, UK_BASE);
    uk_draw_badge(&g_win, 416, 420, "CFS PREEMPT", UK_MAUVE, UK_BASE);
}

static void render_studio(void)
{
    /* Background Canvas */
    uk_fill_rect(&g_win, 0, 0, WIN_W, WIN_H, UK_CRUST);

    /* Top Studio Navigation Bar */
    uk_fill_rect(&g_win, 0, 0, WIN_W, 64, UK_MANTLE);
    uk_fill_rect(&g_win, 0, 63, WIN_W, 1, UK_SURFACE1);

    /* Logo Avatar & Title */
    uk_draw_avatar(&g_win, 28, 32, 16, "AS", UK_MAUVE);
    uk_draw_text_bold(&g_win, 54, 18, "AzamiOS Studio", UK_TEXT);
    uk_draw_text_small(&g_win, 54, 36, "v2.5 Modern GFX", UK_OVERLAY1);

    /* Segmented Navigation Control */
    uk_draw_segmented_control(&g_win, 160, 16, 410, 32,
                             k_tab_names, NUM_TABS, g_tab, g_hover_tab);

    /* Pin / Always-on-top button */
    int pin_x = WIN_W - 96;
    int pin_y = 16;
    uk_btn_state_t pin_st = g_pinned ? UK_BTN_PRESSED :
                            ((g_mx >= pin_x && g_mx <= pin_x + 80 && g_my >= pin_y && g_my <= pin_y + 32)
                             ? UK_BTN_HOVER : UK_BTN_NORMAL);
    uk_draw_button(&g_win, pin_x, pin_y, 80, 32, g_pinned ? "Pinned" : "Pin Top", pin_st);

    /* Content Area based on Tab */
    switch (g_tab) {
    case 0: draw_tab_widgets();    break;
    case 1: draw_tab_compositor(); break;
    case 2: draw_tab_cursors();    break;
    case 3: draw_tab_autoaccept(); break;
    }

    uk_invalidate(&g_win);
}

/* ── Mouse Interaction Handling ────────────────────────────────────────────── */

static void handle_click(int mx, int my)
{
    /* Segmented Navigation Bar */
    if (mx >= 160 && mx <= 570 && my >= 16 && my <= 48) {
        int seg_w = (410 - 4) / NUM_TABS;
        int clicked = (mx - 162) / seg_w;
        if (clicked >= 0 && clicked < NUM_TABS) {
            g_tab = clicked;
            render_studio();
            return;
        }
    }

    /* Pin button */
    if (mx >= WIN_W - 96 && mx <= WIN_W - 16 && my >= 16 && my <= 48) {
        g_pinned = !g_pinned;
        uk_set_always_on_top(&g_win, g_pinned);
        render_studio();
        return;
    }

    if (g_tab == 0) {
        /* Search bar click */
        if (mx >= 24 && mx <= 340 && my >= 204 && my <= 236) {
            if (mx >= 340 - 24) {
                g_search_query[0] = '\0';
            }
            g_search_focused = true;
            uk_set_cursor(&g_win, AZ_CURSOR_IBEAM);
            render_studio();
            return;
        } else {
            if (g_search_focused) {
                g_search_focused = false;
                uk_set_cursor(&g_win, AZ_CURSOR_DEFAULT);
                render_studio();
            }
        }

        /* Dropdown Trigger */
        if (mx >= 360 && mx <= 656 && my >= 204 && my <= 236) {
            g_dropdown_open = !g_dropdown_open;
            render_studio();
            return;
        }

        /* Dropdown popup rows */
        if (g_dropdown_open && mx >= 360 && mx <= 656) {
            int dpy = 238;
            for (int i = 0; i < NUM_DROPDOWN; i++) {
                int row_y = dpy + 4 + i * 26;
                if (my >= row_y && my < row_y + 26) {
                    g_dropdown_selected = i;
                    g_dropdown_open = false;
                    render_studio();
                    return;
                }
            }
            g_dropdown_open = false;
            render_studio();
            return;
        } else if (g_dropdown_open) {
            g_dropdown_open = false;
            render_studio();
        }

        /* Dark mode toggle */
        if (mx >= 500 && mx <= 544 && my >= 285 && my <= 307) {
            g_toggle_state = !g_toggle_state;
            render_studio();
            return;
        }

        /* Progress Bar click */
        if (mx >= 160 && mx <= 440 && my >= 330 && my <= 354) {
            int val = (mx - 160) * 100 / 280;
            if (val < 0) val = 0;
            if (val > 100) val = 100;
            g_progress_pct = val;
            render_studio();
            return;
        }
    } else if (g_tab == 1) {
        /* Opacity Slider */
        if (mx >= 170 && mx <= 530 && my >= 110 && my <= 140) {
            int val = (mx - 170) * 100 / 360;
            if (val < 25) val = 25;
            if (val > 100) val = 100;
            g_opacity_pct = val;
            unsigned char raw_op = (unsigned char)((val * 255) / 100);
            uk_set_opacity(&g_win, raw_op);
            render_studio();
            return;
        }

        /* Title Presets */
        for (int i = 0; i < NUM_TITLES; i++) {
            int bx = 36 + i * 148;
            int by = 256;
            if (mx >= bx && mx <= bx + 138 && my >= by && my <= by + 28) {
                g_title_idx = i;
                uk_set_title(&g_win, k_titles[i]);
                render_studio();
                return;
            }
        }
    } else if (g_tab == 2) {
        /* Cursor Catalog click */
        for (int i = 0; i < NUM_CURSORS; i++) {
            int col = i % 2;
            int row = i / 2;
            int cx = 24 + col * 318;
            int cy = 168 + row * 56;
            int cw = 306;
            int ch = 48;
            if (mx >= cx && mx <= cx + cw && my >= cy && my <= cy + ch) {
                g_selected_cursor = k_cursor_catalog[i].id;
                uk_set_cursor(&g_win, g_selected_cursor);
                render_studio();
                return;
            }
        }
    } else if (g_tab == 3) {
        /* Toggle 1: IPC (280, 118) */
        if (mx >= 280 && mx <= 324 && my >= 118 && my <= 140) {
            g_aa_ipc = !g_aa_ipc;
            render_studio();
            return;
        }
        /* Toggle 2: Admin (280, 152) */
        if (mx >= 280 && mx <= 324 && my >= 152 && my <= 174) {
            g_aa_admin = !g_aa_admin;
            render_studio();
            return;
        }
        /* Toggle 3: DHCP (590, 118) */
        if (mx >= 590 && mx <= 634 && my >= 118 && my <= 140) {
            g_aa_dhcp = !g_aa_dhcp;
            render_studio();
            return;
        }
        /* Toggle 4: Trace (590, 152) */
        if (mx >= 590 && mx <= 634 && my >= 152 && my <= 174) {
            g_aa_trace = !g_aa_trace;
            render_studio();
            return;
        }
        /* Simulate Action Button (36, 280, w=200, h=32) */
        if (mx >= 36 && mx <= 236 && my >= 280 && my <= 312) {
            g_sim_events++;
            if (g_aa_admin) {
                g_sim_status = "Action AUTO-ACCEPTED! Executed seamlessly with zero interactive prompts (0.08ms).";
                g_sim_status_col = UK_GREEN;
            } else {
                g_sim_status = "Action BLOCKED: Interactive confirmation required (Policy unaccepted).";
                g_sim_status_col = UK_PEACH;
            }
            render_studio();
            return;
        }
    }
}

/* ── Main Entry Point ──────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (uk_window_connect(&g_win, "AzamiOS Studio", 120, 80, WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN) < 0) {
        puts("[gui_test] Failed to connect window to azwm server!");
        return -1;
    }

    render_studio();

    int loop_counter = 0;

    while (1) {
        az_wm_msg_t msg;
        int recv_ret = az_channel_recv_nb(g_win.client_chan, (az_ipc_msg_t *)&msg);
        bool needs_draw = false;

        if (recv_ret == 0) {
            switch (msg.type) {
            case AZ_WM_DESTROY_WINDOW:
                sys_exit(0);
                break;

            case AZ_WM_FOCUS_CHANGE:
                needs_draw = true;
                break;

            case AZ_WM_MOUSE_EVENT: {
                g_mx = msg.mouse.abs_x;
                g_my = msg.mouse.abs_y;
                bool lmb = (msg.mouse.buttons & AZ_MOUSE_BTN_LEFT);

                /* Hover tab detection */
                int prev_hov = g_hover_tab;
                if (g_mx >= 160 && g_mx <= 570 && g_my >= 16 && g_my <= 48) {
                    int seg_w = (410 - 4) / NUM_TABS;
                    g_hover_tab = (g_mx - 162) / seg_w;
                } else {
                    g_hover_tab = -1;
                }
                if (prev_hov != g_hover_tab) needs_draw = true;

                /* Click event */
                if (lmb && !g_mouse_down) {
                    g_mouse_down = true;
                    handle_click(g_mx, g_my);
                } else if (!lmb && g_mouse_down) {
                    g_mouse_down = false;
                    needs_draw = true;
                }

                /* Dragging opacity slider in Compositor tab */
                if (g_mouse_down && g_tab == 1 && g_mx >= 170 && g_mx <= 530 && g_my >= 110 && g_my <= 140) {
                    int val = (g_mx - 170) * 100 / 360;
                    if (val < 25) val = 25;
                    if (val > 100) val = 100;
                    if (g_opacity_pct != val) {
                        g_opacity_pct = val;
                        unsigned char raw_op = (unsigned char)((val * 255) / 100);
                        uk_set_opacity(&g_win, raw_op);
                        needs_draw = true;
                    }
                }
                break;
            }

            case AZ_WM_KEY_EVENT: {
                if (msg.key.pressed) {
                    if (g_search_focused) {
                        int len = uk_strlen(g_search_query);
                        if (msg.key.keycode == '\b') {
                            if (len > 0) {
                                g_search_query[len - 1] = '\0';
                                needs_draw = true;
                            }
                        } else if (msg.key.keycode >= 32 && msg.key.keycode <= 126) {
                            if (len < (int)sizeof(g_search_query) - 1) {
                                g_search_query[len] = (char)msg.key.keycode;
                                g_search_query[len + 1] = '\0';
                                needs_draw = true;
                            }
                        }
                    }
                }
                break;
            }

            default:
                break;
            }
        }

        /* Tick background animation step every ~100ms */
        loop_counter++;
        if (loop_counter % 6 == 0) {
            g_spinner_step = (g_spinner_step + 1) % 16;
            if (g_tab == 0) {
                needs_draw = true;
            }
        }

        if (needs_draw) {
            render_studio();
        }

        usleep(16000); /* 60 fps pacing */
    }

    return 0;
}
