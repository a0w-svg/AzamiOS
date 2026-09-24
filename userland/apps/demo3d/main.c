/* ============================================================================
 * AzamiOS 3D Graphics Studio — Real-time 3D Engine & Showcase
 * File: userland/apps/demo3d/main.c
 *
 * Demonstrates the AzamiOS 3D Graphics Engine & Hardware ISA capabilities:
 *  • Complete 3D software rasterization pipeline with Z-buffering
 *  • Interactive orbit and perspective camera controls
 *  • Shading modes: Wireframe, Flat, Gouraud, Textured, Normal Map, Depth Map
 *  • Dynamic directional lighting and Blinn-Phong specular highlights
 *  • Procedural 3D meshes: Cube, UV Sphere, Torus, Pyramid, Cylinder, Grid
 *  • CPU SIMD Vector acceleration indicators & performance telemetry
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <game/game.h>

#define WIN_W 720
#define WIN_H 540

#define NUM_MODELS 6
static const char *k_model_names[NUM_MODELS] = {
    "Torus (Donut)",
    "Textured Cube",
    "UV Sphere",
    "Pyramid",
    "Cylinder",
    "Scene (Grid + Torus)"
};

typedef struct {
    render3d_ctx_t *r3d;
    camera3d_t      camera;

    /* Meshes */
    mesh3d_t       *meshes[NUM_MODELS];
    mesh3d_t       *grid_mesh;
    int             active_model;

    /* Rotation state */
    float           model_rot_x;
    float           model_rot_y;
    float           model_rot_z;
    bool            auto_rotate;

    /* Light animation */
    float           light_angle;
    bool            rotate_light;

    /* Mouse interaction */
    int             last_mouse_x;
    int             last_mouse_y;
    bool            mouse_dragging;

    /* Performance & Telemetry */
    float           fps_timer;
    int             fps_counter;
    int             current_fps;
} demo3d_state_t;

static demo3d_state_t g_state;

/* ── UI Text Rendering Helper ─────────────────────────────────────────────── */

/* Draw a simple 8x16 bitmap character into the target buffer */
static void draw_char_8x16(uint32_t *dst, int w, int h, int cx, int cy, char c, uint32_t color)
{
    /* Built-in minimal 8x16 font glyph renderer (ASCII 32..126) */
    if (c < 32 || c > 126) c = '?';

    /* Fallback basic 5x7 scaled or line representation for readable HUD */
    for (int row = 0; row < 12; row++) {
        int py = cy + row;
        if (py < 0 || py >= h) continue;

        for (int col = 0; col < 8; col++) {
            int px = cx + col;
            if (px < 0 || px >= w) continue;

            /* Procedural simple block / stroke appearance */
            bool on = false;
            if (c >= 'A' && c <= 'Z') {
                if (row == 0 || row == 6 || col == 0 || (col == 7 && (row < 6 || c == 'B' || c == 'R'))) on = true;
            } else if (c >= '0' && c <= '9') {
                if (row == 0 || row == 11 || col == 0 || col == 7) on = true;
                if (row == 6 && (c == '2' || c == '3' || c == '4' || c == '5' || c == '6' || c == '8' || c == '9')) on = true;
            } else if (c == ':') {
                if ((row == 3 || row == 4 || row == 8 || row == 9) && (col >= 2 && col <= 4)) on = true;
            } else if (c == '-') {
                if (row == 6 && col >= 1 && col <= 6) on = true;
            } else if (c == '.') {
                if (row >= 10 && col >= 2 && col <= 4) on = true;
            } else if (c == '[' || c == ']') {
                if (col == (c == '[' ? 1 : 6) || row == 0 || row == 11) on = true;
            } else if (c == '(' || c == ')') {
                if (col == (c == '(' ? 2 : 5) || ((row == 0 || row == 11) && col >= 3 && col <= 4)) on = true;
            } else if (c == '+') {
                if ((row == 6 && col >= 1 && col <= 6) || (col == 3 && row >= 3 && row <= 9)) on = true;
            } else {
                if (row == 11 && col >= 1 && col <= 6) on = true;
            }

            if (on) {
                dst[py * w + px] = color;
            }
        }
    }
}

static void draw_string(uint32_t *dst, int w, int h, int x, int y, const char *str, uint32_t color)
{
    if (!dst || !str) return;
    int cur_x = x;
    while (*str) {
        if (*str == '\n') {
            cur_x = x;
            y += 14;
        } else {
            draw_char_8x16(dst, w, h, cur_x, y, *str, color);
            cur_x += 8;
        }
        str++;
    }
}

/* ── Lifecycle Callbacks ─────────────────────────────────────────────────── */

static void on_init(game_t *g)
{
    memset(&g_state, 0, sizeof(g_state));

    /* Initialize 3D renderer */
    g_state.r3d = render3d_create(WIN_W, WIN_H);
    if (!g_state.r3d) {
        printf("[DEMO3D] Failed to create 3D render context!\n");
        return;
    }

    g_state.r3d->mode           = RENDER3D_FLAT;
    g_state.r3d->cull_backfaces = true;
    g_state.r3d->wire_overlay   = false;
    g_state.r3d->wire_color     = 0xFF10121A;

    /* Setup Camera */
    g_state.camera = camera3d_create((float)WIN_W / (float)WIN_H);
    g_state.camera.orbit_dist = 4.2f;
    g_state.camera.pitch = DEG2RAD(20.0f);
    g_state.camera.yaw   = DEG2RAD(30.0f);
    camera3d_update_orbit_pos(&g_state.camera);

    /* Setup Light */
    g_state.r3d->light.dir     = vec3_normalize(vec3(0.6f, 1.0f, 0.8f));
    g_state.r3d->light.color   = vec3(1.0f, 0.95f, 0.85f);
    g_state.r3d->light.ambient = vec3(0.22f, 0.24f, 0.28f);

    /* Create 3D Meshes */
    g_state.meshes[0] = mesh3d_create_torus(1.1f, 0.45f, 28, 18, 0xFF3B82F6); /* Torus */
    g_state.meshes[1] = mesh3d_create_cube(1.8f, 0xFFEAB308);                 /* Cube */
    g_state.meshes[2] = mesh3d_create_sphere(1.2f, 18, 24, 0xFF10B981);       /* Sphere */
    g_state.meshes[3] = mesh3d_create_pyramid(2.0f, 2.0f, 0xFFEC4899);        /* Pyramid */
    g_state.meshes[4] = mesh3d_create_cylinder(0.9f, 2.2f, 20, 0xFF8B5CF6);   /* Cylinder */
    g_state.meshes[5] = mesh3d_create_torus(0.8f, 0.35f, 24, 16, 0xFF06B6D4);/* Scene Torus */

    /* Floor Grid */
    g_state.grid_mesh = mesh3d_create_grid(12.0f, 16, 0xFF334155);

    g_state.active_model = 0;
    g_state.auto_rotate  = true;
    g_state.rotate_light = true;
    g_state.light_angle  = 0.0f;
}

static void on_update(game_t *g, float dt)
{
    /* FPS calculation */
    g_state.fps_timer += dt;
    g_state.fps_counter++;
    if (g_state.fps_timer >= 1.0f) {
        g_state.current_fps = g_state.fps_counter;
        g_state.fps_counter = 0;
        g_state.fps_timer   = 0.0f;
    }

    /* Model Auto-rotation */
    if (g_state.auto_rotate) {
        g_state.model_rot_y += dt * 0.8f;
        g_state.model_rot_x += dt * 0.35f;
    }

    /* Light rotation */
    if (g_state.rotate_light) {
        g_state.light_angle += dt * 1.2f;
        float lx = cosf(g_state.light_angle);
        float lz = sinf(g_state.light_angle);
        g_state.r3d->light.dir = vec3_normalize(vec3(lx, 1.2f, lz));
    }
}

static void on_key(game_t *g, int key, bool pressed, int mods)
{
    if (!pressed) return;

    switch (key) {
    case '1': g_state.r3d->mode = RENDER3D_WIREFRAME; break;
    case '2': g_state.r3d->mode = RENDER3D_FLAT; break;
    case '3': g_state.r3d->mode = RENDER3D_GOURAUD; break;
    case '4': g_state.r3d->mode = RENDER3D_TEXTURED; break;
    case '5': g_state.r3d->mode = RENDER3D_NORMALS; break;
    case '6': g_state.r3d->mode = RENDER3D_DEPTH; break;

    case '\t':
    case ' ':
        /* Cycle active model */
        g_state.active_model = (g_state.active_model + 1) % NUM_MODELS;
        break;

    case 'l':
    case 'L':
        g_state.rotate_light = !g_state.rotate_light;
        break;

    case 'p':
    case 'P':
        g_state.auto_rotate = !g_state.auto_rotate;
        break;

    case 'o':
    case 'O':
        g_state.r3d->wire_overlay = !g_state.r3d->wire_overlay;
        break;

    case 'c':
    case 'C':
        g_state.r3d->cull_backfaces = !g_state.r3d->cull_backfaces;
        break;

    case 'r':
    case 'R':
        /* Reset view */
        g_state.camera.orbit_dist  = 4.2f;
        g_state.camera.pitch       = DEG2RAD(20.0f);
        g_state.camera.yaw         = DEG2RAD(30.0f);
        g_state.model_rot_x        = 0.0f;
        g_state.model_rot_y        = 0.0f;
        g_state.model_rot_z        = 0.0f;
        camera3d_update_orbit_pos(&g_state.camera);
        break;

    case '+':
    case '=':
    case 'e':
    case 'E':
        camera3d_zoom(&g_state.camera, -0.4f);
        break;

    case '-':
    case '_':
    case 'q':
    case 'Q':
        camera3d_zoom(&g_state.camera, 0.4f);
        break;

    case 'a':
    case 'A':
        camera3d_orbit(&g_state.camera, -DEG2RAD(6.0f), 0.0f);
        break;

    case 'd':
    case 'D':
        camera3d_orbit(&g_state.camera, DEG2RAD(6.0f), 0.0f);
        break;

    case 'w':
    case 'W':
        camera3d_orbit(&g_state.camera, 0.0f, DEG2RAD(4.0f));
        break;

    case 's':
    case 'S':
        camera3d_orbit(&g_state.camera, 0.0f, -DEG2RAD(4.0f));
        break;
    }
}

static void on_mouse(game_t *g, int x, int y, int btn, bool pressed)
{
    if (pressed) {
        g_state.mouse_dragging = true;
        g_state.last_mouse_x   = x;
        g_state.last_mouse_y   = y;
    } else {
        g_state.mouse_dragging = false;
    }
}

static void on_render(game_t *g)
{
    uint32_t *pixels = (uint32_t *)g->win.pixels;
    int w = g->win.width;
    int h = g->win.height;

    render3d_set_target(g_state.r3d, pixels, w, h);
    /* Clean dark slate background */
    render3d_clear(g_state.r3d, 0xFF0F172A);

    /* Update camera aspect ratio & view */
    g_state.camera.aspect = (float)w / (float)h;
    render3d_set_camera(g_state.r3d, &g_state.camera);

    /* Track mouse drag orbit */
    if (g_state.mouse_dragging) {
        int dx = g->input.mouse_x - g_state.last_mouse_x;
        int dy = g->input.mouse_y - g_state.last_mouse_y;
        if (dx != 0 || dy != 0) {
            camera3d_orbit(&g_state.camera, (float)dx * 0.008f, -(float)dy * 0.008f);
            render3d_set_camera(g_state.r3d, &g_state.camera);
            g_state.last_mouse_x = g->input.mouse_x;
            g_state.last_mouse_y = g->input.mouse_y;
        }
    }

    /* ── Render 3D Scene ─────────────────────────────────────────────────── */

    /* 1. Floor Grid (for Scene mode or as floor) */
    if (g_state.active_model == 5 && g_state.grid_mesh) {
        mat4_t grid_mat = mat4_translation(0.0f, -1.4f, 0.0f);
        render3d_draw_mesh(g_state.r3d, g_state.grid_mesh, grid_mat);
    }

    /* 2. Main Object Transformation */
    mat4_t t_mat = mat4_translation(0.0f, 0.0f, 0.0f);
    mat4_t rx_mat = mat4_rotation_x(g_state.model_rot_x);
    mat4_t ry_mat = mat4_rotation_y(g_state.model_rot_y);
    mat4_t r_mat = mat4_mul(ry_mat, rx_mat);
    mat4_t model_mat = mat4_mul(t_mat, r_mat);

    mesh3d_t *mesh = g_state.meshes[g_state.active_model];
    if (mesh) {
        render3d_draw_mesh(g_state.r3d, mesh, model_mat);
    }

    /* ── Overlay 2D HUD ──────────────────────────────────────────────────── */

    /* Top Banner Card */
    int banner_h = 34;
    for (int by = 0; by < banner_h; by++) {
        for (int bx = 0; bx < w; bx++) {
            pixels[by * w + bx] = 0xEE1E293B;
        }
    }
    draw_string(pixels, w, h, 14, 10, "AZAMIOS 3D GRAPHICS STUDIO", 0xFF38BDF8);

    char perf_buf[64];
    snprintf(perf_buf, sizeof(perf_buf), "FPS:%d  TRI:%d", g_state.current_fps, g_state.r3d->tri_count_frame);
    draw_string(pixels, w, h, w - 160, 10, perf_buf, 0xFF4ADE80);

    /* Left Info Card */
    const char *mode_str = "FLAT";
    switch (g_state.r3d->mode) {
        case RENDER3D_WIREFRAME: mode_str = "WIREFRAME"; break;
        case RENDER3D_FLAT:      mode_str = "FLAT"; break;
        case RENDER3D_GOURAUD:   mode_str = "GOURAUD"; break;
        case RENDER3D_TEXTURED:  mode_str = "TEXTURED"; break;
        case RENDER3D_NORMALS:   mode_str = "NORMALS"; break;
        case RENDER3D_DEPTH:     mode_str = "DEPTH"; break;
    }

    char model_buf[64];
    snprintf(model_buf, sizeof(model_buf), "MODEL: %s", k_model_names[g_state.active_model]);
    draw_string(pixels, w, h, 14, 46, model_buf, 0xFFF1F5F9);

    char mode_buf[64];
    snprintf(mode_buf, sizeof(mode_buf), "SHADING: %s  CULL:%s  WIRE:%s",
             mode_str,
             g_state.r3d->cull_backfaces ? "ON" : "OFF",
             g_state.r3d->wire_overlay ? "ON" : "OFF");
    draw_string(pixels, w, h, 14, 62, mode_buf, 0xFF94A3B8);

    draw_string(pixels, w, h, 14, 78, "HARDWARE ACCEL: x86_64 SSE/AVX SIMD + VIRTIO-GPU", 0xFFA78BFA);

    /* Bottom Control Bar */
    int bar_y = h - 26;
    for (int by = bar_y; by < h; by++) {
        for (int bx = 0; bx < w; bx++) {
            pixels[by * w + bx] = 0xEE0F172A;
        }
    }
    draw_string(pixels, w, h, 12, bar_y + 6,
                "[1-6] SHADING   [TAB] MODEL   [L] LIGHT   [O] WIRE   [WASD/DRAG] ORBIT   [+/-] ZOOM",
                0xFFCBD5E1);
}

static void on_destroy(game_t *g)
{
    for (int i = 0; i < NUM_MODELS; i++) {
        if (g_state.meshes[i]) mesh3d_destroy(g_state.meshes[i]);
    }
    if (g_state.grid_mesh) mesh3d_destroy(g_state.grid_mesh);
    if (g_state.r3d) render3d_destroy(g_state.r3d);
}

/* ── Main Entry ───────────────────────────────────────────────────────────── */

int main(void)
{
    game_config_t cfg = {
        .title = "AzamiOS 3D Graphics Studio",
        .width = WIN_W,
        .height = WIN_H,
        .target_fps = 60,
        .enable_audio = false
    };

    game_callbacks_t cb = {
        .on_init    = on_init,
        .on_update  = on_update,
        .on_key     = on_key,
        .on_mouse   = on_mouse,
        .on_render  = on_render,
        .on_destroy = on_destroy
    };

    return game_run(&cfg, &cb);
}
