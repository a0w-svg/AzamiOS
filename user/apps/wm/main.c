/**
 * main.c — AzamiOS Modular Desktop Environment v5.0 Main Loop
 *
 * KEY DESIGN CHANGES IN v5.0:
 * 1. O(1) Algorithmic Time Complexity:
 *    Window and service lookups in the core event loop use direct table indexing
 *    for constant-time performance regardless of window count.
 * 2. -Ofast Compiler Optimization:
 *    Compiled with maximum optimization level for extreme rendering speed.
 *
 * KEY DESIGN CHANGES IN v4.0:
 * 1. Desktop Environment Abstraction:
 *    The DE is initialized via `wm_switch_de(WM_DE_CLASSIC)` before the render loop.
 *    All visual rendering routes through `g_de->*` function pointers. Switching the DE
 *    at runtime (e.g. from the Settings window) requires only a `g_de` pointer swap —
 *    zero restarts, zero recompilation.
 *
 * 2. Theme Engine:
 *    `wm_switch_theme(WM_THEME_OCEAN)` initializes `g_theme`. Color decisions across
 *    all rendering code read from `g_theme->*` rather than compile-time COL_* macros,
 *    enabling live palette changes.
 *
 * 3. Toast Notification System:
 *    `render_toast_notifications()` is called each dirty frame, after window rendering
 *    but before `gfx_flip()`. It both draws and ages the toast queue in one pass.
 *
 * 4. Top Status Bar (Modern DE):
 *    `g_de->render_status_bar()` is called before desktop icons, so it sits visually
 *    behind icons but above the wallpaper. Classic DE implements this as a no-op.
 *
 * 5. Dynamic Start Menu Dispatching (unchanged from v3.0):
 *    Menu entries are generated from the service registry at render time, so new
 *    services auto-appear in the menu without any change to this file.
 */

#include "wm.h"

void _start(void) {
    printf("AzamiOS Desktop Environment v6.0 starting...\n");
    /* Allow disabling graphics initialization by creating a file named
     * "/DISABLE_GFX" in the root of the filesystem. Useful for running
     * the WM in environments without a framebuffer or while debugging. */
    int no_gfx_fd = open("/DISABLE_GFX", 0);
    if (no_gfx_fd < 0) {
        init_graphics();
    } else {
        close(no_gfx_fd);
        printf("AzamiOS WM: graphics init skipped (DISABLE_GFX present)\n");
    }

    /* ── Drain keyboard buffer (avoids replaying keystrokes buffered during
     *    a previous exec() child process execution) ──────────────────── */
    while (has_char()) getchar();

    /* ── Load themes + wm.conf from filesystem (must happen before switch_de)─ */
    wm_config_init();

    /* ── Register modular services ────────────────────────────────────── */
    welcome_service_init();
    terminal_service_init();
    notepad_service_init();
    sysmon_service_init();
    files_service_init();
    glcube_service_init();
    pong_service_init();
    settings_service_init();
    snake_service_init();
    minesweeper_service_init();
    tetris_service_init();
    python_repl_service_init();
    debugger_service_init();
    browser_service_init();
    netmon_service_init();

    /* ── Apply DE and Theme from config (falls back to Classic/Ocean if fresh) */
    wm_switch_de(wm_config_get_de());
    wm_switch_theme(wm_config_get_theme_idx());

    /* ── Initialize window slots and service on_init callbacks ────────────── */
    init_wins();

    /* ── Try to restore a full WM state snapshot (saved before last exec()) ───
     *    This restores all window positions and open-states seamlessly.
     *    If no snapshot exists (fresh boot), fall through to welcome screen. */
    bool restored = wm_load_full_state();
    if (restored) {
        wm_delete_full_state();
        /* Terminal state is independently restored by terminal_init via .wm_term_state */
    } else {
        /* Fresh boot: open terminal if a previous session was saved, else welcome */
        int ts_fd = open(".wm_term_state", 0);
        if (ts_fd >= 0) {
            close(ts_fd);
            int w_idx = find_win_by_type(WIN_WELCOME);
            if (w_idx >= 0) wm_close_window(&g_wins[w_idx]);
            open_win_type(WIN_TERMINAL);
        }
        wm_push_notification("Welcome to AzamiOS v5.0", COL_TEXT_CYAN);
    }

    /* ── Event loop state ───────────────────────────────────────────────── */
    bool dragging  = false;
    int  drag_x    = 0, drag_y = 0;
    bool prev_left = false, prev_right = false;
    uint32_t frame_cnt = 0;
    int  blink = 0;

    for (;;) {
        frame_cnt++;
        rtc_time_t t;
        rtc_get_time(&t);
        mouse_state_t ms;
        get_mouse_state(&ms);

        /* ── Dirty detection ─────────────────────────────────────────── */
        static int  p_mx = -1, p_my = -1;
        static bool p_lb = false, p_rb = false;
        static int  p_sec = -1, p_foc = -1;
        static bool p_sm = false, p_cm = false;

        bool dirty = false;
        if (ms.x != p_mx || ms.y != p_my || ms.left_btn != p_lb || ms.right_btn != p_rb) {
            dirty = true;
            p_mx = ms.x; p_my = ms.y; p_lb = ms.left_btn; p_rb = ms.right_btn;
        }
        if (t.second != p_sec) { dirty = true; p_sec = t.second; }
        if (g_focus != p_foc || start_menu != p_sm || ctx_menu_open != p_cm || dragging) {
            dirty = true;
            p_foc = g_focus; p_sm = start_menu; p_cm = ctx_menu_open;
        }
        /* Toast system needs per-tick updates */
        for (int i = 0; i < WM_NOTIF_MAX; i++) {
            if (g_notif_queue[i].active) { dirty = true; break; }
        }
        if ((frame_cnt % 30) == 0) dirty = true;

        /* Check ANIMATED flag for continuous redraw (3D / game windows) */
        for (int i = 0; i < g_num_wins; i++) {
            if (g_wins[i].open && !g_wins[i].minimized) {
                const wm_service_t *srv = wm_get_service(g_wins[i].type);
                if (srv && (srv->flags & WM_SRV_FLAG_ANIMATED)) { dirty = true; break; }
            }
        }

        /* ── Mouse Input ─────────────────────────────────────────────── */
        bool left_click  = ms.left_btn  && !prev_left;
        bool right_click = ms.right_btn && !prev_right;

        /* Right-click → context menu on empty desktop */
        if (right_click) {
            bool on_window = false;
            for (int i = 0; i < g_num_wins; i++) {
                window_t *w = &g_wins[i];
                if (!w->open || w->minimized) continue;
                if (ms.x >= w->x && ms.x <= w->x + w->w &&
                    ms.y >= w->y && ms.y <= w->y + w->h) {
                    on_window = true; break;
                }
            }
            if (!on_window && ms.y < DESKTOP_H) {
                ctx_menu_open = true;
                ctx_menu_x = ms.x; ctx_menu_y = ms.y;
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
                    if (entry == 0) open_win_type(WIN_NOTEPAD);
                    else if (entry == 1) open_win_type(WIN_ABOUT);
                    else if (entry == 2) open_win_type(WIN_FILES);
                }
                ctx_menu_open = false;
                handled = true;
                goto input_done;
            }

            /* 2. Start button */
            int tb_y = SCREEN_H - TASKBAR_H;
            if (ms.x >= 4 && ms.x <= 68 && ms.y >= tb_y + 3 && ms.y <= tb_y + 22) {
                start_menu = !start_menu;
                ctx_menu_open = false;
                handled = true;
            }

            /* 3. Taskbar window buttons */
            if (!handled && ms.y >= tb_y && ms.y <= SCREEN_H) {
                int btn_x = 76;
                for (int i = 0; i < g_num_wins; i++) {
                    if (!g_wins[i].open) continue;
                    if (ms.x >= btn_x && ms.x <= btn_x + 68 &&
                        ms.y >= tb_y + 3 && ms.y <= tb_y + 22) {
                        if (g_wins[i].minimized) {
                            g_wins[i].minimized = false;
                            g_focus = i;
                        } else if (g_focus == i) {
                            g_wins[i].minimized = true;
                        } else {
                            g_focus = i;
                        }
                        handled = true; break;
                    }
                    btn_x += 72;
                }
            }

            /* 4. Start menu clicks (Dynamically Dispatched) */
            if (start_menu && !handled) {
                int srv_count = wm_get_service_count();
                int total_entries = srv_count + 1;
                int menu_h = START_HEADER_H + total_entries * START_ENTRY_H + 12;
                int mx = 4;
                int my = SCREEN_H - TASKBAR_H - menu_h;
                if (ms.x >= mx && ms.x <= mx + START_MENU_W &&
                    ms.y >= my && ms.y <= my + menu_h) {
                    int rel_y = ms.y - my - START_HEADER_H - 4;
                    if (rel_y >= 0) {
                        int entry = rel_y / START_ENTRY_H;
                        if (entry >= 0 && entry < srv_count) {
                            const wm_service_t *srv = wm_get_service_by_index(entry);
                            if (srv) open_win_type(srv->type);
                        } else if (entry == srv_count) {
                            exit(0);
                        }
                    }
                }
                start_menu = false;
                handled = true;
                goto input_done;
            }

            /* 5. Desktop icon clicks */
            if (!handled && !start_menu) {
                static const int icon_ys[NUM_ICONS] = {18, 83, 148, 213, 278, 343, 373, 408};
                for (int i = 0; i < NUM_ICONS; i++) {
                    int ix = 14, iy = icon_ys[i];
                    if (ms.x >= ix && ms.x <= ix + 24 &&
                        ms.y >= iy && ms.y <= iy + 40) {
                        open_win_type(icons[i].win_type);
                        handled = true; break;
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
                    int by_btn = w->y + 5;

                    /* Close */
                    if (ms.x >= bx - 22 && ms.x <= bx - 6 &&
                        ms.y >= by_btn  && ms.y <= by_btn + 12) {
                        wm_close_window(w); handled = true; break;
                    }
                    /* Maximize */
                    if (ms.x >= bx - 42 && ms.x <= bx - 26 &&
                        ms.y >= by_btn   && ms.y <= by_btn + 12) {
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
                        g_focus = idx; handled = true; break;
                    }
                    /* Minimize */
                    if (ms.x >= bx - 62 && ms.x <= bx - 46 &&
                        ms.y >= by_btn   && ms.y <= by_btn + 12) {
                        w->minimized = true; handled = true; break;
                    }
                    /* Titlebar drag */
                    if (ms.y >= w->y && ms.y <= w->y + TITLEBAR_H) {
                        g_focus = idx;
                        dragging = true;
                        drag_x = ms.x - w->x;
                        drag_y = ms.y - w->y;
                        handled = true; break;
                    }
                    /* Body click — dispatch to service if it handles clicks */
                    if (w->type == WIN_SETTINGS) {
                        settings_handle_mouse_click(ms.x, ms.y);
                    }
                    g_focus = idx; handled = true; break;
                }
            }

            if (!handled) {
                start_menu = false;
                ctx_menu_open = false;
            }
        }

        input_done:

        if (!ms.left_btn) {
            if (dragging && g_focus >= 0 && g_focus < g_num_wins && !g_wins[g_focus].maximized) {
                window_t *w = &g_wins[g_focus];
                /* Aero Snap edge detection on release */
                if (ms.x <= 10) {
                    /* Snap left half */
                    w->ox = w->x; w->oy = w->y; w->ow = w->w; w->oh = w->h;
                    wm_resize_window(w, SCREEN_W / 2, DESKTOP_H);
                    w->x = 0; w->y = 0;
                    w->snap_state = 1;
                } else if (ms.x >= SCREEN_W - 10) {
                    /* Snap right half */
                    w->ox = w->x; w->oy = w->y; w->ow = w->w; w->oh = w->h;
                    wm_resize_window(w, SCREEN_W / 2, DESKTOP_H);
                    w->x = SCREEN_W / 2; w->y = 0;
                    w->snap_state = 2;
                } else if (ms.y <= 10) {
                    /* Maximize */
                    w->ox = w->x; w->oy = w->y; w->ow = w->w; w->oh = w->h;
                    wm_resize_window(w, SCREEN_W, DESKTOP_H);
                    w->x = 0; w->y = 0;
                    w->maximized = true; w->snap_state = 3;
                }
            }
            dragging = false;
        }
        if (dragging && g_focus >= 0 && g_focus < g_num_wins && !g_wins[g_focus].maximized) {
            int nx = ms.x - drag_x;
            int ny = ms.y - drag_y;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx + g_wins[g_focus].w > SCREEN_W && SCREEN_W >= g_wins[g_focus].w) nx = SCREEN_W - g_wins[g_focus].w;
            if (ny + g_wins[g_focus].h > DESKTOP_H && DESKTOP_H >= g_wins[g_focus].h) ny = DESKTOP_H - g_wins[g_focus].h;
            g_wins[g_focus].x = nx;
            g_wins[g_focus].y = ny;
            g_wins[g_focus].snap_state = 0;
        }
        prev_left  = ms.left_btn;
        prev_right = ms.right_btn;

        /* ── Keyboard Input (Validated) ──────────────────────────────── */
        while (has_char()) {
            dirty = true;
            int c = getchar();
            if (c < 0 || c > 255) continue;
            int ftype = (g_focus >= 0 && g_focus < g_num_wins) ? g_wins[g_focus].type : -1;
            const wm_service_t *srv = wm_get_service(ftype);
            if (srv && srv->on_key) {
                srv->on_key(&g_wins[g_focus], c, &t, frame_cnt);
            }
        }

        if (!dirty) {
            /* Yield CPU to kernel rather than spin-wait — prevents flooding
             * the scheduler and serial port with thousands of empty iterations. */
            sys_yield();
            continue;
        }

        /* ── Render Frame ────────────────────────────────────────────── */
        blink = (blink + 1) % 40;

        /* Wallpaper (routed through g_de) */
        draw_wallpaper();

        /* Top status bar (Modern DE: thin info bar; Classic DE: no-op) */
        if (g_de && g_de->render_status_bar) g_de->render_status_bar(&t);

        /* Desktop icons */
        render_desktop_icons();

        /* Windows in Z-order */
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

        /* Menus */
        if (start_menu)    render_start_menu();
        if (ctx_menu_open) render_context_menu();

        /* Toasts (rendered above everything except taskbar) */
        render_toast_notifications();

        /* Taskbar (topmost UI layer) */
        render_taskbar(&t, start_menu);

        gfx_flip();

        /* Rate-limit: vsync caps the render loop so we don't burn CPU and
         * flood the kernel with hundreds of syscalls per second. */
        sys_gfx_vsync();
    }

    /* Should never reach here. Yield instead of busy-spin. */
    for (;;) sys_yield();
}
