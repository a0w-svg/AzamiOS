/* ============================================================================
 * AzamiOS Game Framework — Core Game Loop & Bootstrap
 * File: userland/libgame/game_core.c
 * ============================================================================ */

#include "include/game/game_core.h"
#include <string.h>

int game_init(game_t *g, const game_config_t *cfg, const game_callbacks_t *cb)
{
    if (!g || !cfg) return -1;
    memset(g, 0, sizeof(*g));

    if (cb) g->callbacks = *cb;
    g->user_data = cfg->user_data;

    g->target_fps = cfg->target_fps > 0 ? cfg->target_fps : GAME_DEFAULT_FPS;
    g->timer_interval_ms = 1000 / g->target_fps;
    if (g->timer_interval_ms < 1) g->timer_interval_ms = 1;
    g->dt = 1.0f / (float)g->target_fps;

    /* Detect screen resolution */
    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0 && fb.width > 0 && fb.height > 0) {
        sw = fb.width;
        sh = fb.height;
    }

    /* Window geometry */
    int win_w = cfg->width > 0 ? cfg->width : 640;
    int win_h = cfg->height > 0 ? cfg->height : 480;
    int win_x = (int)(sw / 2) - win_w / 2;
    int win_y = (int)(sh / 2) - win_h / 2;
    if (win_x < 0) win_x = 0;
    if (win_y < 0) win_y = 0;

    void *map_addr = cfg->map_addr ? cfg->map_addr : GAME_DEFAULT_MAP_ADDR;
    int server_chan = cfg->server_chan > 0 ? cfg->server_chan : GAME_DEFAULT_SERVER_CHAN;
    const char *title = cfg->title ? cfg->title : "AzamiOS Game";

    int ret = uk_window_connect(&g->win, title, win_x, win_y, win_w, win_h,
                                map_addr, server_chan);
    if (ret < 0) return ret;

    /* Start autonomous game timer */
    az_set_timer(g->win.client_chan, (unsigned int)g->timer_interval_ms, 0);

    /* Initialize subsystems */
    input_init(&g->input);
    ecs_init(&g->ecs);
    phys_init(&g->phys, cfg->gravity_x, cfg->gravity_y != 0.0f ? cfg->gravity_y : 980.0f);
    asset_init(&g->assets);
    scene_init(&g->scenes);
    camera_init(&g->camera, win_w, win_h);

    if (cfg->enable_audio) {
        audio_init(&g->audio);
    }

    g->running = true;

    if (g->callbacks.on_init) {
        g->callbacks.on_init(g);
    }

    return 0;
}

void game_step(game_t *g)
{
    if (!g || !g->running) return;

    /* Snapshot input */
    input_begin_frame(&g->input);

    /* Update camera */
    camera_update(&g->camera, g->dt);

    g->frame_count++;
    g->total_time += g->dt;

    /* Subsystem updates */
    if (!g->paused) {
        scene_update(&g->scenes, g, g->dt);
        if (g->callbacks.on_update) {
            g->callbacks.on_update(g, g->dt);
        }
        phys_step(&g->phys, &g->ecs, g->dt);
    }

    /* Render */
    scene_render(&g->scenes, g);
    if (g->callbacks.on_render) {
        g->callbacks.on_render(g);
    }

    /* Transition overlay */
    int trans_a = scene_transition_alpha(&g->scenes);
    if (trans_a > 0) {
        game_draw_overlay(g, g->scenes.trans_color, (uint8_t)trans_a);
    }

    /* Audio */
    if (g->audio.initialized) {
        audio_mix_and_flush(&g->audio);
    }

    /* Present backbuffer */
    uk_invalidate(&g->win);
}

void game_quit(game_t *g)
{
    if (g) g->running = false;
}

void game_set_fps(game_t *g, int fps)
{
    if (!g || fps <= 0) return;
    g->target_fps = fps;
    g->timer_interval_ms = 1000 / fps;
    if (g->timer_interval_ms < 1) g->timer_interval_ms = 1;
    g->dt = 1.0f / (float)fps;
    az_set_timer(g->win.client_chan, (unsigned int)g->timer_interval_ms, 0);
}

void game_clear(game_t *g, uint32_t color)
{
    if (!g || !g->win.pixels) return;
    uk_fill_rect(&g->win, 0, 0, (int)g->win.width, (int)g->win.height, (unsigned int)color);
}

void game_draw_overlay(game_t *g, uint32_t color, uint8_t alpha)
{
    if (!g || !g->win.pixels || alpha == 0) return;

    int total_pixels = (int)(g->win.width * g->win.height);
    uint32_t *p = (uint32_t *)g->win.pixels;
    uint32_t sa = alpha;
    uint32_t inv_a = 255 - sa;
    uint32_t crb = color & 0x00FF00FFu;
    uint32_t cg  = color & 0x0000FF00u;

    for (int i = 0; i < total_pixels; i++) {
        uint32_t d = p[i];
        uint32_t rb = (((crb * sa) + ((d & 0x00FF00FFu) * inv_a)) >> 8) & 0x00FF00FFu;
        uint32_t g_col = (((cg * sa) + ((d & 0x0000FF00u) * inv_a)) >> 8) & 0x0000FF00u;
        p[i] = 0xFF000000u | rb | g_col;
    }
}

int game_run(game_config_t *cfg, game_callbacks_t *cb)
{
    game_t g;
    if (game_init(&g, cfg, cb) < 0) return -1;

    /* Initial draw */
    game_step(&g);

    while (g.running) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g.win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) {
            g.running = false;
            break;
        }

        if (msg.type == AZ_WM_WINDOW_RESIZED) {
            if (!uk_handle_resize(&g.win, &msg)) {
                g.running = false;
                break;
            }
            g.camera.viewport_w = (float)g.win.width;
            g.camera.viewport_h = (float)g.win.height;
            if (g.callbacks.on_resize) {
                g.callbacks.on_resize(&g, (int)g.win.width, (int)g.win.height);
            }
            game_step(&g);
            continue;
        }

        if (msg.type == AZ_WM_TIMER_TICK) {
            game_step(&g);
        } else if (msg.type == AZ_WM_KEY_EVENT) {
            int key = (int)msg.key.keycode;
            bool pressed = msg.key.pressed != 0;
            int mods = (int)msg.key.modifiers;
            input_set_key(&g.input, key, pressed);
            input_set_modifiers(&g.input, (uint16_t)mods);
            if (g.callbacks.on_key) {
                g.callbacks.on_key(&g, key, pressed, mods);
            }
        } else if (msg.type == AZ_WM_MOUSE_EVENT) {
            int mx = msg.mouse.abs_x;
            int my = msg.mouse.abs_y;
            uint8_t btn = (uint8_t)msg.mouse.buttons;
            input_set_mouse(&g.input, mx, my, 0, 0, btn, 0);
            if (g.callbacks.on_mouse) {
                g.callbacks.on_mouse(&g, mx, my, (int)btn, btn != 0);
            }
        }
    }

    if (g.callbacks.on_destroy) {
        g.callbacks.on_destroy(&g);
    }

    if (g.audio.initialized) {
        audio_shutdown(&g.audio);
    }

    asset_free_all(&g.assets);

    return 0;
}
