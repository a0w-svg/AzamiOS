/**
 * main.c — AzamiOS Secure & Modular Desktop Environment Main Loop
 *
 * EDUCATIONAL ARCHITECTURE & SECURITY EXPLANATIONS:
 * 1. Decoupled Frame Animation Triggering:
 *    Instead of hardcoding checks for specific window IDs (like `WIN_GLCUBE`),
 *    the loop checks if any visible window's service possesses the `WM_SRV_FLAG_ANIMATED`
 *    capability flag. This decouples core event loop logic from specific apps.
 * 2. Dynamic Start Menu Event Dispatching:
 *    Menu click resolution dynamically matches click offsets against registered service index
 *    entries (`wm_get_service_by_index`). Applications can be dynamically added or removed
 *    without modifying UI dispatch code.
 * 3. Safe Lifecycle Mutators & Input Validation:
 *    Closing windows triggers `wm_close_window` ensuring service cleanup hooks (`on_close`)
 *    execute safely. Keyboard inputs are clamped to standard ASCII bounds before dispatch.
 */

#include "wm.h"

void _start(void) {
    printf("AzamiOS Desktop Environment v3.0 starting...\n");
    init_graphics();

    /* Register modular services */
    welcome_service_init();
    terminal_service_init();
    notepad_service_init();
    sysmon_service_init();
    files_service_init();
    glcube_service_init();

    init_wins();

    bool dragging = false;
    int drag_x = 0, drag_y = 0;
    bool prev_left = false;
    bool prev_right = false;
    uint32_t frame_cnt = 0;
    int blink = 0;

    for (;;) {
        frame_cnt++;
        rtc_time_t t;
        rtc_get_time(&t);
        mouse_state_t ms;
        get_mouse_state(&ms);

        static int p_mx = -1, p_my = -1;
        static bool p_lb = false, p_rb = false;
        static int p_sec = -1;
        static int p_foc = -1;
        static bool p_sm = false, p_cm = false;

        bool dirty = false;
        if (ms.x != p_mx || ms.y != p_my || ms.left_btn != p_lb || ms.right_btn != p_rb) {
            dirty = true;
            p_mx = ms.x; p_my = ms.y; p_lb = ms.left_btn; p_rb = ms.right_btn;
        }
        if (t.second != p_sec) {
            dirty = true;
            p_sec = t.second;
        }
        if (g_focus != p_foc || start_menu != p_sm || ctx_menu_open != p_cm || dragging) {
            dirty = true;
            p_foc = g_focus; p_sm = start_menu; p_cm = ctx_menu_open;
        }
        if ((frame_cnt % 30) == 0) dirty = true;

        /* MODULARITY: Check capability flags instead of hardcoded app IDs */
        for (int i = 0; i < g_num_wins; i++) {
            if (g_wins[i].open && !g_wins[i].minimized) {
                const wm_service_t *srv = wm_get_service(g_wins[i].type);
                if (srv && (srv->flags & WM_SRV_FLAG_ANIMATED)) {
                    dirty = true;
                    break;
                }
            }
        }

        /* ── Mouse Input ─────────────────────────────────────────────── */
        bool left_click  = ms.left_btn && !prev_left;
        bool right_click = ms.right_btn && !prev_right;

        /* Right-click: open context menu on desktop */
        if (right_click) {
            bool on_window = false;
            for (int i = 0; i < g_num_wins; i++) {
                window_t *w = &g_wins[i];
                if (!w->open || w->minimized) continue;
                if (ms.x >= w->x && ms.x <= w->x + w->w &&
                    ms.y >= w->y && ms.y <= w->y + w->h) {
                    on_window = true;
                    break;
                }
            }
            if (!on_window && ms.y < DESKTOP_H) {
                ctx_menu_open = true;
                ctx_menu_x = ms.x;
                ctx_menu_y = ms.y;
                start_menu = false;
            }
        }

        if (left_click) {
            bool handled = false;

            /* 1. Context menu clicks */
            if (ctx_menu_open) {
                int mx = ctx_menu_x, my = ctx_menu_y;
                if (mx + CTX_MENU_W > SCREEN_W) mx = SCREEN_W - CTX_MENU_W;
                if (my + CTX_MENU_H > DESKTOP_H) my = DESKTOP_H - CTX_MENU_H;

                if (ms.x >= mx && ms.x <= mx + CTX_MENU_W &&
                    ms.y >= my && ms.y <= my + CTX_MENU_H) {
                    int rel_y = ms.y - my - 6;
                    int entry = rel_y / CTX_MENU_ENTRY_H;
                    if (entry == 0) { open_win_type(WIN_NOTEPAD); }
                    else if (entry == 1) { open_win_type(WIN_ABOUT); }
                    else if (entry == 2) { open_win_type(WIN_FILES); }
                    handled = true;
                }
                ctx_menu_open = false;
                if (handled) goto input_done;
            }

            /* 2. Start button */
            int tb_y = SCREEN_H - TASKBAR_H;
            if (ms.x >= 4 && ms.x <= 64 && ms.y >= tb_y + 4 && ms.y <= tb_y + 20) {
                start_menu = !start_menu;
                ctx_menu_open = false;
                handled = true;
            }

            /* 3. Taskbar window buttons */
            if (!handled && ms.y >= tb_y && ms.y <= SCREEN_H) {
                int btn_x = 72;
                for (int i = 0; i < g_num_wins; i++) {
                    if (!g_wins[i].open) continue;
                    if (ms.x >= btn_x && ms.x <= btn_x + 68 &&
                        ms.y >= tb_y + 4 && ms.y <= tb_y + 20) {
                        if (g_wins[i].minimized) {
                            g_wins[i].minimized = false;
                            g_focus = i;
                        } else if (g_focus == i) {
                            g_wins[i].minimized = true;
                        } else {
                            g_focus = i;
                        }
                        handled = true;
                        break;
                    }
                    btn_x += 72;
                }
            }

            /* 4. Start menu clicks (Dynamically Dispatched) */
            if (start_menu && !handled) {
                int srv_count = wm_get_service_count();
                int total_entries = srv_count + 1;
                int menu_h = START_HEADER_H + total_entries * START_ENTRY_H + 8;
                int mx = 4;
                int my = SCREEN_H - TASKBAR_H - menu_h;
                if (ms.x >= mx && ms.x <= mx + START_MENU_W &&
                    ms.y >= my && ms.y <= my + menu_h) {
                    int rel_y = ms.y - my - START_HEADER_H - 2;
                    if (rel_y >= 0) {
                        int entry = rel_y / START_ENTRY_H;
                        if (entry >= 0 && entry < srv_count) {
                            const wm_service_t *srv = wm_get_service_by_index(entry);
                            if (srv) open_win_type(srv->type);
                        } else if (entry == srv_count) {
                            exit(0);
                        }
                    }
                    start_menu = false;
                    handled = true;
                }
            }

            /* 5. Desktop icons */
            if (!handled && !start_menu) {
                for (int i = 0; i < NUM_ICONS; i++) {
                    if (ms.x >= icons[i].x && ms.x <= icons[i].x + 24 &&
                        ms.y >= icons[i].y && ms.y <= icons[i].y + 40) {
                        open_win_type(icons[i].win_type);
                        handled = true;
                        break;
                    }
                }
            }

            /* 6. Window interactions (top-down Z-order) */
            if (!handled) {
                int order[MAX_WINS];
                int oc = 0;
                for (int i = 0; i < g_num_wins; i++)
                    if (i != g_focus) order[oc++] = i;
                order[oc++] = g_focus;

                for (int z = oc - 1; z >= 0; z--) {
                    int idx = order[z];
                    window_t *w = &g_wins[idx];
                    if (!w->open || w->minimized) continue;

                    if (ms.x < w->x || ms.x > w->x + w->w ||
                        ms.y < w->y || ms.y > w->y + w->h) continue;

                    int bx = w->x + w->w;
                    int by_btn = w->y + 4;

                    /* Close button (Safe Lifecycle API) */
                    if (ms.x >= bx - 20 && ms.x <= bx - 4 &&
                        ms.y >= by_btn && ms.y <= by_btn + 14) {
                        wm_close_window(w);
                        handled = true;
                        break;
                    }
                    /* Maximize button */
                    if (ms.x >= bx - 40 && ms.x <= bx - 24 &&
                        ms.y >= by_btn && ms.y <= by_btn + 14) {
                        if (w->maximized) {
                            w->x = w->ox; w->y = w->oy;
                            w->w = w->ow; w->h = w->oh;
                            w->maximized = false;
                        } else {
                            w->ox = w->x; w->oy = w->y;
                            w->ow = w->w; w->oh = w->h;
                            wm_resize_window(w, SCREEN_W, DESKTOP_H);
                            w->x = 0; w->y = 0;
                            w->maximized = true;
                        }
                        g_focus = idx;
                        handled = true;
                        break;
                    }
                    /* Minimize button */
                    if (ms.x >= bx - 60 && ms.x <= bx - 44 &&
                        ms.y >= by_btn && ms.y <= by_btn + 14) {
                        w->minimized = true;
                        handled = true;
                        break;
                    }
                    /* Titlebar drag */
                    if (ms.y >= w->y && ms.y <= w->y + TITLEBAR_H) {
                        g_focus = idx;
                        dragging = true;
                        drag_x = ms.x - w->x;
                        drag_y = ms.y - w->y;
                        handled = true;
                        break;
                    }
                    /* Body click: just focus */
                    g_focus = idx;
                    handled = true;
                    break;
                }
            }

            if (!handled) {
                start_menu = false;
                ctx_menu_open = false;
            }
        }

        input_done:

        if (!ms.left_btn) dragging = false;
        if (dragging && g_focus >= 0 && g_focus < g_num_wins && !g_wins[g_focus].maximized) {
            g_wins[g_focus].x = ms.x - drag_x;
            g_wins[g_focus].y = ms.y - drag_y;
        }
        prev_left = ms.left_btn;
        prev_right = ms.right_btn;

        /* ── Keyboard Input (Validated) ──────────────────────────────── */
        while (has_char()) {
            dirty = true;
            int c = getchar();
            /* SECURITY ENFORCEMENT: Clamp input character to valid ASCII range */
            if (c < 0 || c > 255) continue;
            int ftype = (g_focus >= 0 && g_focus < g_num_wins) ? g_wins[g_focus].type : -1;
            const wm_service_t *srv = wm_get_service(ftype);
            if (srv && srv->on_key) {
                srv->on_key(&g_wins[g_focus], c, &t, frame_cnt);
            }
        }

        if (!dirty) {
            for (volatile int k = 0; k < 5000; k++);
            continue;
        }

        /* ── Render Frame ────────────────────────────────────────────── */
        blink = (blink + 1) % 40;

        draw_wallpaper();
        render_desktop_icons();

        {
            int order[MAX_WINS];
            int oc = 0;
            for (int i = 0; i < g_num_wins; i++)
                if (i != g_focus) order[oc++] = i;
            order[oc++] = g_focus;

            for (int z = 0; z < oc; z++) {
                render_window(&g_wins[order[z]], &t, frame_cnt, blink);
            }
        }

        if (start_menu) render_start_menu();
        if (ctx_menu_open) render_context_menu();
        render_taskbar(&t, start_menu);

        gfx_flip();

        for (volatile int k = 0; k < 150; k++);
    }

    for(;;);
}
