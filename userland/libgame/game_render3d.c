/* ============================================================================
 * AzamiOS Game Framework — 3D Graphics Engine & Software Rasterizer
 * File: userland/libgame/game_render3d.c
 * ============================================================================ */

#include "include/game/game_render3d.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── Z-Buffer Management ─────────────────────────────────────────────────── */

zbuffer_t *zbuffer_create(int width, int height)
{
    if (width <= 0 || height <= 0) return NULL;

    zbuffer_t *zb = (zbuffer_t *)malloc(sizeof(zbuffer_t));
    if (!zb) return NULL;

    zb->width  = width;
    zb->height = height;
    zb->data   = (float *)malloc(sizeof(float) * width * height);
    if (!zb->data) {
        free(zb);
        return NULL;
    }

    zbuffer_clear(zb, 1.0f);
    return zb;
}

void zbuffer_destroy(zbuffer_t *zb)
{
    if (!zb) return;
    if (zb->data) free(zb->data);
    free(zb);
}

void zbuffer_resize(zbuffer_t *zb, int width, int height)
{
    if (!zb || width <= 0 || height <= 0) return;
    if (zb->width == width && zb->height == height) return;

    free(zb->data);
    zb->width  = width;
    zb->height = height;
    zb->data   = (float *)malloc(sizeof(float) * width * height);
    if (zb->data) {
        zbuffer_clear(zb, 1.0f);
    }
}

void zbuffer_clear(zbuffer_t *zb, float clear_depth)
{
    if (!zb || !zb->data) return;
    int count = zb->width * zb->height;
    for (int i = 0; i < count; i++) {
        zb->data[i] = clear_depth;
    }
}

/* ── Camera Implementation ───────────────────────────────────────────────── */

camera3d_t camera3d_create(float aspect)
{
    camera3d_t cam;
    memset(&cam, 0, sizeof(cam));
    cam.target     = vec3(0.0f, 0.0f, 0.0f);
    cam.up         = vec3(0.0f, 1.0f, 0.0f);
    cam.fov_rad    = DEG2RAD(60.0f);
    cam.aspect     = aspect > 0.0f ? aspect : (4.0f / 3.0f);
    cam.near_z     = 0.1f;
    cam.far_z      = 100.0f;
    cam.yaw        = DEG2RAD(35.0f);
    cam.pitch      = DEG2RAD(25.0f);
    cam.orbit_dist = 4.5f;
    cam.is_ortho   = false;
    cam.ortho_size = 3.0f;

    camera3d_update_orbit_pos(&cam);
    return cam;
}

void camera3d_update_orbit_pos(camera3d_t *cam)
{
    if (!cam) return;
    float cp = cosf(cam->pitch);
    cam->pos.x = cam->target.x + cam->orbit_dist * sinf(cam->yaw) * cp;
    cam->pos.y = cam->target.y + cam->orbit_dist * sinf(cam->pitch);
    cam->pos.z = cam->target.z + cam->orbit_dist * cosf(cam->yaw) * cp;
}

mat4_t camera3d_get_view(const camera3d_t *cam)
{
    return mat4_lookat(cam->pos, cam->target, cam->up);
}

mat4_t camera3d_get_proj(const camera3d_t *cam)
{
    if (cam->is_ortho) {
        float h = cam->ortho_size;
        float w = h * cam->aspect;
        return mat4_ortho(-w, w, -h, h, cam->near_z, cam->far_z);
    }
    return mat4_perspective(cam->fov_rad, cam->aspect, cam->near_z, cam->far_z);
}

void camera3d_orbit(camera3d_t *cam, float delta_yaw, float delta_pitch)
{
    if (!cam) return;
    cam->yaw   += delta_yaw;
    cam->pitch += delta_pitch;

    float limit = DEG2RAD(89.0f);
    if (cam->pitch > limit)  cam->pitch = limit;
    if (cam->pitch < -limit) cam->pitch = -limit;

    camera3d_update_orbit_pos(cam);
}

void camera3d_zoom(camera3d_t *cam, float delta_dist)
{
    if (!cam) return;
    cam->orbit_dist += delta_dist;
    if (cam->orbit_dist < 0.5f) cam->orbit_dist = 0.5f;
    if (cam->orbit_dist > 50.0f) cam->orbit_dist = 50.0f;
    camera3d_update_orbit_pos(cam);
}

/* ── 3D Mesh Generators ──────────────────────────────────────────────────── */

void mesh3d_destroy(mesh3d_t *mesh)
{
    if (!mesh) return;
    if (mesh->vertices) free(mesh->vertices);
    if (mesh->indices)  free(mesh->indices);
    free(mesh);
}

static mesh3d_t *mesh3d_alloc(int num_verts, int num_indices, uint32_t color)
{
    mesh3d_t *m = (mesh3d_t *)calloc(1, sizeof(mesh3d_t));
    if (!m) return NULL;

    m->num_vertices = num_verts;
    m->vertices     = (vertex3d_t *)calloc(num_verts, sizeof(vertex3d_t));
    m->num_indices  = num_indices;
    m->indices      = (uint16_t *)calloc(num_indices, sizeof(uint16_t));
    m->material     = material3d_default(color);

    if (!m->vertices || !m->indices) {
        mesh3d_destroy(m);
        return NULL;
    }
    return m;
}

mesh3d_t *mesh3d_create_cube(float size, uint32_t color)
{
    float h = size * 0.5f;
    /* 24 vertices for 6 faces, so each face has independent perpendicular normals */
    mesh3d_t *m = mesh3d_alloc(24, 36, color);
    if (!m) return NULL;

    vertex3d_t verts[24] = {
        /* Front face (Z+) */
        { { -h, -h,  h }, { 0, 0, 1 }, { 0, 1 }, color },
        { {  h, -h,  h }, { 0, 0, 1 }, { 1, 1 }, color },
        { {  h,  h,  h }, { 0, 0, 1 }, { 1, 0 }, color },
        { { -h,  h,  h }, { 0, 0, 1 }, { 0, 0 }, color },

        /* Back face (Z-) */
        { {  h, -h, -h }, { 0, 0, -1 }, { 0, 1 }, color },
        { { -h, -h, -h }, { 0, 0, -1 }, { 1, 1 }, color },
        { { -h,  h, -h }, { 0, 0, -1 }, { 1, 0 }, color },
        { {  h,  h, -h }, { 0, 0, -1 }, { 0, 0 }, color },

        /* Top face (Y+) */
        { { -h,  h,  h }, { 0, 1, 0 }, { 0, 1 }, color },
        { {  h,  h,  h }, { 0, 1, 0 }, { 1, 1 }, color },
        { {  h,  h, -h }, { 0, 1, 0 }, { 1, 0 }, color },
        { { -h,  h, -h }, { 0, 1, 0 }, { 0, 0 }, color },

        /* Bottom face (Y-) */
        { { -h, -h, -h }, { 0, -1, 0 }, { 0, 1 }, color },
        { {  h, -h, -h }, { 0, -1, 0 }, { 1, 1 }, color },
        { {  h, -h,  h }, { 0, -1, 0 }, { 1, 0 }, color },
        { { -h, -h,  h }, { 0, -1, 0 }, { 0, 0 }, color },

        /* Right face (X+) */
        { {  h, -h,  h }, { 1, 0, 0 }, { 0, 1 }, color },
        { {  h, -h, -h }, { 1, 0, 0 }, { 1, 1 }, color },
        { {  h,  h, -h }, { 1, 0, 0 }, { 1, 0 }, color },
        { {  h,  h,  h }, { 1, 0, 0 }, { 0, 0 }, color },

        /* Left face (X-) */
        { { -h, -h, -h }, { -1, 0, 0 }, { 0, 1 }, color },
        { { -h, -h,  h }, { -1, 0, 0 }, { 1, 1 }, color },
        { { -h,  h,  h }, { -1, 0, 0 }, { 1, 0 }, color },
        { { -h,  h, -h }, { -1, 0, 0 }, { 0, 0 }, color },
    };
    memcpy(m->vertices, verts, sizeof(verts));

    int cur = 0;
    for (int f = 0; f < 6; f++) {
        uint16_t base = f * 4;
        m->indices[cur++] = base + 0;
        m->indices[cur++] = base + 1;
        m->indices[cur++] = base + 2;
        m->indices[cur++] = base + 0;
        m->indices[cur++] = base + 2;
        m->indices[cur++] = base + 3;
    }

    m->min_bounds = vec3(-h, -h, -h);
    m->max_bounds = vec3(h, h, h);
    return m;
}

mesh3d_t *mesh3d_create_sphere(float radius, int lat_segs, int lon_segs, uint32_t color)
{
    if (lat_segs < 3) lat_segs = 12;
    if (lon_segs < 3) lon_segs = 16;

    int num_verts = (lat_segs + 1) * (lon_segs + 1);
    int num_indices = lat_segs * lon_segs * 6;

    mesh3d_t *m = mesh3d_alloc(num_verts, num_indices, color);
    if (!m) return NULL;

    int v_idx = 0;
    for (int y = 0; y <= lat_segs; y++) {
        float v = (float)y / (float)lat_segs;
        float theta = v * (float)M_PI;
        float sin_t = sinf(theta);
        float cos_t = cosf(theta);

        for (int x = 0; x <= lon_segs; x++) {
            float u = (float)x / (float)lon_segs;
            float phi = u * 2.0f * (float)M_PI;
            float sin_p = sinf(phi);
            float cos_p = cosf(phi);

            vec3_t n = vec3(cos_p * sin_t, cos_t, sin_p * sin_t);
            vec3_t p = vec3_scale(n, radius);

            m->vertices[v_idx].pos    = p;
            m->vertices[v_idx].normal = n;
            m->vertices[v_idx].uv     = vec2(u, v);
            m->vertices[v_idx].color  = color;
            v_idx++;
        }
    }

    int i_idx = 0;
    for (int y = 0; y < lat_segs; y++) {
        for (int x = 0; x < lon_segs; x++) {
            uint16_t i0 = y * (lon_segs + 1) + x;
            uint16_t i1 = i0 + 1;
            uint16_t i2 = (y + 1) * (lon_segs + 1) + x;
            uint16_t i3 = i2 + 1;

            m->indices[i_idx++] = i0;
            m->indices[i_idx++] = i2;
            m->indices[i_idx++] = i1;

            m->indices[i_idx++] = i1;
            m->indices[i_idx++] = i2;
            m->indices[i_idx++] = i3;
        }
    }

    m->min_bounds = vec3(-radius, -radius, -radius);
    m->max_bounds = vec3(radius, radius, radius);
    return m;
}

mesh3d_t *mesh3d_create_torus(float r_major, float r_minor, int segs_u, int segs_v, uint32_t color)
{
    if (segs_u < 4) segs_u = 24;
    if (segs_v < 4) segs_v = 16;

    int num_verts = (segs_u + 1) * (segs_v + 1);
    int num_indices = segs_u * segs_v * 6;

    mesh3d_t *m = mesh3d_alloc(num_verts, num_indices, color);
    if (!m) return NULL;

    int v_idx = 0;
    for (int i = 0; i <= segs_u; i++) {
        float u = (float)i / (float)segs_u;
        float theta = u * 2.0f * (float)M_PI;
        float cos_t = cosf(theta);
        float sin_t = sinf(theta);

        for (int j = 0; j <= segs_v; j++) {
            float v = (float)j / (float)segs_v;
            float phi = v * 2.0f * (float)M_PI;
            float cos_p = cosf(phi);
            float sin_p = sinf(phi);

            float r = r_major + r_minor * cos_p;
            vec3_t p = vec3(r * cos_t, r_minor * sin_p, r * sin_t);
            vec3_t n = vec3_normalize(vec3(cos_p * cos_t, sin_p, cos_p * sin_t));

            m->vertices[v_idx].pos    = p;
            m->vertices[v_idx].normal = n;
            m->vertices[v_idx].uv     = vec2(u, v);
            m->vertices[v_idx].color  = color;
            v_idx++;
        }
    }

    int i_idx = 0;
    for (int i = 0; i < segs_u; i++) {
        for (int j = 0; j < segs_v; j++) {
            uint16_t i0 = i * (segs_v + 1) + j;
            uint16_t i1 = i0 + 1;
            uint16_t i2 = (i + 1) * (segs_v + 1) + j;
            uint16_t i3 = i2 + 1;

            m->indices[i_idx++] = i0;
            m->indices[i_idx++] = i2;
            m->indices[i_idx++] = i1;

            m->indices[i_idx++] = i1;
            m->indices[i_idx++] = i2;
            m->indices[i_idx++] = i3;
        }
    }

    float max_r = r_major + r_minor;
    m->min_bounds = vec3(-max_r, -r_minor, -max_r);
    m->max_bounds = vec3(max_r, r_minor, max_r);
    return m;
}

mesh3d_t *mesh3d_create_pyramid(float base_size, float height, uint32_t color)
{
    float h = base_size * 0.5f;
    float top_y = height * 0.5f;
    float bot_y = -height * 0.5f;

    /* 4 side triangles + 2 base triangles = 16 vertices for distinct normals */
    mesh3d_t *m = mesh3d_alloc(16, 18, color);
    if (!m) return NULL;

    /* Front side */
    vec3_t n_f = vec3_normalize(vec3(0, h, top_y - bot_y));
    /* Back side */
    vec3_t n_b = vec3_normalize(vec3(0, h, -(top_y - bot_y)));
    /* Right side */
    vec3_t n_r = vec3_normalize(vec3(top_y - bot_y, h, 0));
    /* Left side */
    vec3_t n_l = vec3_normalize(vec3(-(top_y - bot_y), h, 0));
    vec3_t n_down = vec3(0, -1, 0);

    vertex3d_t verts[16] = {
        /* Front */
        { {  0,  top_y,  0 }, n_f, { 0.5f, 0 }, color },
        { { -h,  bot_y,  h }, n_f, { 0.0f, 1 }, color },
        { {  h,  bot_y,  h }, n_f, { 1.0f, 1 }, color },

        /* Right */
        { {  0,  top_y,  0 }, n_r, { 0.5f, 0 }, color },
        { {  h,  bot_y,  h }, n_r, { 0.0f, 1 }, color },
        { {  h,  bot_y, -h }, n_r, { 1.0f, 1 }, color },

        /* Back */
        { {  0,  top_y,  0 }, n_b, { 0.5f, 0 }, color },
        { {  h,  bot_y, -h }, n_b, { 0.0f, 1 }, color },
        { { -h,  bot_y, -h }, n_b, { 1.0f, 1 }, color },

        /* Left */
        { {  0,  top_y,  0 }, n_l, { 0.5f, 0 }, color },
        { { -h,  bot_y, -h }, n_l, { 0.0f, 1 }, color },
        { { -h,  bot_y,  h }, n_l, { 1.0f, 1 }, color },

        /* Base */
        { { -h,  bot_y, -h }, n_down, { 0, 0 }, color },
        { {  h,  bot_y, -h }, n_down, { 1, 0 }, color },
        { {  h,  bot_y,  h }, n_down, { 1, 1 }, color },
        { { -h,  bot_y,  h }, n_down, { 0, 1 }, color },
    };
    memcpy(m->vertices, verts, sizeof(verts));

    uint16_t idx[18] = {
        0, 1, 2,     /* Front */
        3, 4, 5,     /* Right */
        6, 7, 8,     /* Back  */
        9, 10, 11,   /* Left  */
        12, 13, 14,  12, 14, 15 /* Base */
    };
    memcpy(m->indices, idx, sizeof(idx));

    m->min_bounds = vec3(-h, bot_y, -h);
    m->max_bounds = vec3(h, top_y, h);
    return m;
}

mesh3d_t *mesh3d_create_cylinder(float radius, float height, int segs, uint32_t color)
{
    if (segs < 6) segs = 16;
    float hh = height * 0.5f;

    int num_verts = (segs + 1) * 2 + (segs + 1) * 2; /* sides + caps */
    int num_indices = segs * 6 + segs * 3 * 2;

    mesh3d_t *m = mesh3d_alloc(num_verts, num_indices, color);
    if (!m) return NULL;

    int v = 0;
    /* Side vertices */
    for (int i = 0; i <= segs; i++) {
        float u = (float)i / (float)segs;
        float a = u * 2.0f * (float)M_PI;
        float ca = cosf(a), sa = sinf(a);
        vec3_t n = vec3(ca, 0, sa);

        m->vertices[v++] = (vertex3d_t){ { ca * radius,  hh, sa * radius }, n, { u, 0 }, color };
        m->vertices[v++] = (vertex3d_t){ { ca * radius, -hh, sa * radius }, n, { u, 1 }, color };
    }

    int i_idx = 0;
    for (int i = 0; i < segs; i++) {
        uint16_t i0 = i * 2;
        uint16_t i1 = i0 + 1;
        uint16_t i2 = (i + 1) * 2;
        uint16_t i3 = i2 + 1;

        m->indices[i_idx++] = i0;
        m->indices[i_idx++] = i1;
        m->indices[i_idx++] = i2;

        m->indices[i_idx++] = i2;
        m->indices[i_idx++] = i1;
        m->indices[i_idx++] = i3;
    }

    /* Top cap */
    uint16_t top_center = v++;
    m->vertices[top_center] = (vertex3d_t){ { 0, hh, 0 }, { 0, 1, 0 }, { 0.5f, 0.5f }, color };
    uint16_t top_start = v;
    for (int i = 0; i <= segs; i++) {
        float a = ((float)i / (float)segs) * 2.0f * (float)M_PI;
        m->vertices[v++] = (vertex3d_t){ { cosf(a) * radius, hh, sinf(a) * radius }, { 0, 1, 0 }, { 0.5f + 0.5f * cosf(a), 0.5f + 0.5f * sinf(a) }, color };
    }
    for (int i = 0; i < segs; i++) {
        m->indices[i_idx++] = top_center;
        m->indices[i_idx++] = top_start + i + 1;
        m->indices[i_idx++] = top_start + i;
    }

    /* Bottom cap */
    uint16_t bot_center = v++;
    m->vertices[bot_center] = (vertex3d_t){ { 0, -hh, 0 }, { 0, -1, 0 }, { 0.5f, 0.5f }, color };
    uint16_t bot_start = v;
    for (int i = 0; i <= segs; i++) {
        float a = ((float)i / (float)segs) * 2.0f * (float)M_PI;
        m->vertices[v++] = (vertex3d_t){ { cosf(a) * radius, -hh, sinf(a) * radius }, { 0, -1, 0 }, { 0.5f + 0.5f * cosf(a), 0.5f + 0.5f * sinf(a) }, color };
    }
    for (int i = 0; i < segs; i++) {
        m->indices[i_idx++] = bot_center;
        m->indices[i_idx++] = bot_start + i;
        m->indices[i_idx++] = bot_start + i + 1;
    }

    m->min_bounds = vec3(-radius, -hh, -radius);
    m->max_bounds = vec3(radius, hh, radius);
    return m;
}

mesh3d_t *mesh3d_create_grid(float size, int divisions, uint32_t color)
{
    if (divisions < 1) divisions = 10;
    int num_verts = (divisions + 1) * (divisions + 1);
    int num_indices = divisions * divisions * 6;

    mesh3d_t *m = mesh3d_alloc(num_verts, num_indices, color);
    if (!m) return NULL;

    float step = size / (float)divisions;
    float start = -size * 0.5f;

    int v = 0;
    for (int z = 0; z <= divisions; z++) {
        float pz = start + (float)z * step;
        float tz = (float)z / (float)divisions;
        for (int x = 0; x <= divisions; x++) {
            float px = start + (float)x * step;
            float tx = (float)x / (float)divisions;
            m->vertices[v++] = (vertex3d_t){ { px, 0.0f, pz }, { 0.0f, 1.0f, 0.0f }, { tx, tz }, color };
        }
    }

    int idx = 0;
    for (int z = 0; z < divisions; z++) {
        for (int x = 0; x < divisions; x++) {
            uint16_t i0 = z * (divisions + 1) + x;
            uint16_t i1 = i0 + 1;
            uint16_t i2 = (z + 1) * (divisions + 1) + x;
            uint16_t i3 = i2 + 1;

            m->indices[idx++] = i0;
            m->indices[idx++] = i1;
            m->indices[idx++] = i2;

            m->indices[idx++] = i1;
            m->indices[idx++] = i3;
            m->indices[idx++] = i2;
        }
    }

    m->min_bounds = vec3(start, 0, start);
    m->max_bounds = vec3(-start, 0, -start);
    return m;
}

/* ── 3D Render Context Implementation ────────────────────────────────────── */

render3d_ctx_t *render3d_create(int width, int height)
{
    if (width <= 0 || height <= 0) return NULL;

    render3d_ctx_t *ctx = (render3d_ctx_t *)calloc(1, sizeof(render3d_ctx_t));
    if (!ctx) return NULL;

    ctx->buf_w           = width;
    ctx->buf_h           = height;
    ctx->zbuf            = zbuffer_create(width, height);
    ctx->mode            = RENDER3D_FLAT;
    ctx->light           = light3d_default();
    ctx->cull_backfaces  = true;
    ctx->wire_overlay    = false;
    ctx->wire_color      = 0xFF000000;
    ctx->view_matrix     = mat4_identity();
    ctx->proj_matrix     = mat4_identity();
    ctx->vp_matrix       = mat4_identity();

    if (!ctx->zbuf) {
        free(ctx);
        return NULL;
    }
    return ctx;
}

void render3d_destroy(render3d_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->zbuf) zbuffer_destroy(ctx->zbuf);
    free(ctx);
}

void render3d_resize(render3d_ctx_t *ctx, int new_w, int new_h)
{
    if (!ctx || new_w <= 0 || new_h <= 0) return;
    ctx->buf_w = new_w;
    ctx->buf_h = new_h;
    if (ctx->zbuf) zbuffer_resize(ctx->zbuf, new_w, new_h);
}

void render3d_set_target(render3d_ctx_t *ctx, uint32_t *target_buf, int w, int h)
{
    if (!ctx) return;
    ctx->target_buf = target_buf;
    if (w != ctx->buf_w || h != ctx->buf_h) {
        render3d_resize(ctx, w, h);
    }
}

void render3d_clear(render3d_ctx_t *ctx, uint32_t clear_color)
{
    if (!ctx) return;
    ctx->tri_count_frame = 0;

    if (ctx->target_buf) {
        int total_px = ctx->buf_w * ctx->buf_h;
        for (int i = 0; i < total_px; i++) {
            ctx->target_buf[i] = clear_color;
        }
    }
    if (ctx->zbuf) {
        zbuffer_clear(ctx->zbuf, 1.0f);
    }
}

void render3d_set_camera(render3d_ctx_t *ctx, const camera3d_t *cam)
{
    if (!ctx || !cam) return;
    ctx->view_matrix = camera3d_get_view(cam);
    ctx->proj_matrix = camera3d_get_proj(cam);
    ctx->vp_matrix   = mat4_mul(ctx->proj_matrix, ctx->view_matrix);
}

/* ── Drawing Primitives ──────────────────────────────────────────────────── */

static inline void put_pixel_depth(render3d_ctx_t *ctx, int x, int y, float depth, uint32_t color)
{
    if (x < 0 || x >= ctx->buf_w || y < 0 || y >= ctx->buf_h) return;
    if (zbuffer_test_and_set(ctx->zbuf, x, y, depth)) {
        ctx->target_buf[y * ctx->buf_w + x] = color;
    }
}

/* Bresenham 3D Line with Z-Buffer testing */
void render3d_draw_line(render3d_ctx_t *ctx, vec3_t p0, vec3_t p1, uint32_t color)
{
    if (!ctx || !ctx->target_buf) return;

    /* Transform to clip space */
    vec4_t c0 = mat4_mul_vec4(ctx->vp_matrix, vec4_from_vec3(p0, 1.0f));
    vec4_t c1 = mat4_mul_vec4(ctx->vp_matrix, vec4_from_vec3(p1, 1.0f));

    /* Simple near clipping */
    if (c0.w < 0.1f && c1.w < 0.1f) return;
    if (c0.w < 0.1f) c0.w = 0.1f;
    if (c1.w < 0.1f) c1.w = 0.1f;

    float inv_w0 = 1.0f / c0.w;
    float inv_w1 = 1.0f / c1.w;

    int x0 = (int)((c0.x * inv_w0 + 1.0f) * 0.5f * (float)ctx->buf_w);
    int y0 = (int)((1.0f - c0.y * inv_w0) * 0.5f * (float)ctx->buf_h);
    float z0 = (c0.z * inv_w0 + 1.0f) * 0.5f;

    int x1 = (int)((c1.x * inv_w1 + 1.0f) * 0.5f * (float)ctx->buf_w);
    int y1 = (int)((1.0f - c1.y * inv_w1) * 0.5f * (float)ctx->buf_h);
    float z1 = (c1.z * inv_w1 + 1.0f) * 0.5f;

    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int total_steps = dx > -dy ? dx : -dy;
    if (total_steps == 0) total_steps = 1;
    int step = 0;

    while (1) {
        float t = (float)step / (float)total_steps;
        float z = z0 + (z1 - z0) * t;
        put_pixel_depth(ctx, x0, y0, z, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        step++;
    }
}

/* Pixel shading calculation */
static inline uint32_t shade_pixel(render3d_ctx_t *ctx,
                                   const material3d_t *mat,
                                   vec3_t normal,
                                   vec2_t uv,
                                   float depth,
                                   uint32_t base_color)
{
    switch (ctx->mode) {
    case RENDER3D_WIREFRAME:
        return base_color;

    case RENDER3D_NORMALS: {
        /* Map normal [-1..1] to [0..255] */
        uint8_t r = (uint8_t)((normal.x * 0.5f + 0.5f) * 255.0f);
        uint8_t g = (uint8_t)((normal.y * 0.5f + 0.5f) * 255.0f);
        uint8_t b = (uint8_t)((normal.z * 0.5f + 0.5f) * 255.0f);
        return 0xFF000000 | (r << 16) | (g << 8) | b;
    }

    case RENDER3D_DEPTH: {
        /* Heatmap / distance falloff */
        float d = depth;
        if (d < 0.0f) d = 0.0f;
        if (d > 1.0f) d = 1.0f;
        uint8_t v = (uint8_t)((1.0f - d) * 255.0f);
        return 0xFF000000 | (v << 16) | (v << 8) | v;
    }

    case RENDER3D_TEXTURED: {
        /* Sample texture buffer or generate procedural pattern */
        uint32_t tex_color = base_color;
        if (mat && mat->texture && mat->tex_w > 0 && mat->tex_h > 0) {
            float fu = uv.x - floorf(uv.x);
            float fv = uv.y - floorf(uv.y);
            int tx = (int)(fu * (float)mat->tex_w) % mat->tex_w;
            int ty = (int)(fv * (float)mat->tex_h) % mat->tex_h;
            if (tx < 0) tx += mat->tex_w;
            if (ty < 0) ty += mat->tex_h;
            tex_color = mat->texture[ty * mat->tex_w + tx];
        } else {
            /* Procedural checkerboard texture pattern */
            int check_x = (int)(uv.x * 12.0f);
            int check_y = (int)(uv.y * 12.0f);
            bool dark = ((check_x + check_y) & 1) != 0;
            tex_color = dark ? 0xFF2A2E3D : base_color;
        }

        /* Modulate with lighting */
        float ndotl = vec3_dot(normal, ctx->light.dir);
        if (ndotl < 0.0f) ndotl = 0.0f;
        float r = ((tex_color >> 16) & 0xFF) / 255.0f;
        float g = ((tex_color >> 8)  & 0xFF) / 255.0f;
        float b = ( tex_color        & 0xFF) / 255.0f;

        float lit_r = r * (ctx->light.ambient.x + ctx->light.color.x * ndotl);
        float lit_g = g * (ctx->light.ambient.y + ctx->light.color.y * ndotl);
        float lit_b = b * (ctx->light.ambient.z + ctx->light.color.z * ndotl);

        uint8_t ur = (uint8_t)(lit_r > 1.0f ? 255 : (lit_r * 255.0f));
        uint8_t ug = (uint8_t)(lit_g > 1.0f ? 255 : (lit_g * 255.0f));
        uint8_t ub = (uint8_t)(lit_b > 1.0f ? 255 : (lit_b * 255.0f));
        return 0xFF000000 | (ur << 16) | (ug << 8) | ub;
    }

    case RENDER3D_FLAT:
    case RENDER3D_GOURAUD:
    default: {
        float ndotl = vec3_dot(normal, ctx->light.dir);
        if (ndotl < 0.0f) ndotl = 0.0f;

        float r = ((base_color >> 16) & 0xFF) / 255.0f;
        float g = ((base_color >> 8)  & 0xFF) / 255.0f;
        float b = ( base_color        & 0xFF) / 255.0f;

        /* Ambient + Diffuse */
        float lit_r = r * (ctx->light.ambient.x + ctx->light.color.x * ndotl);
        float lit_g = g * (ctx->light.ambient.y + ctx->light.color.y * ndotl);
        float lit_b = b * (ctx->light.ambient.z + ctx->light.color.z * ndotl);

        /* Blinn-Phong Specular Highlight */
        vec3_t view_dir = vec3(0, 0, 1); /* approx view dir in screen space */
        vec3_t half_vec = vec3_normalize(vec3_add(ctx->light.dir, view_dir));
        float ndoth = vec3_dot(normal, half_vec);
        if (ndoth > 0.0f && ndotl > 0.0f) {
            float spec = powf(ndoth, mat ? mat->specular_exp : 16.0f) * (mat ? mat->specular_str : 0.3f);
            lit_r += spec * ctx->light.color.x;
            lit_g += spec * ctx->light.color.y;
            lit_b += spec * ctx->light.color.z;
        }

        uint8_t ur = (uint8_t)(lit_r > 1.0f ? 255 : (lit_r * 255.0f));
        uint8_t ug = (uint8_t)(lit_g > 1.0f ? 255 : (lit_g * 255.0f));
        uint8_t ub = (uint8_t)(lit_b > 1.0f ? 255 : (lit_b * 255.0f));
        return 0xFF000000 | (ur << 16) | (ug << 8) | ub;
    }
    }
}

/* Transformed screen-space vertex */
typedef struct {
    float x, y, z, w;
    float inv_w;
    vec3_t normal;
    vec2_t uv;
    uint32_t color;
} s_vertex_t;

/* Triangle Barycentric Rasterizer */
static void rasterize_triangle(render3d_ctx_t *ctx,
                               const s_vertex_t *v0,
                               const s_vertex_t *v1,
                               const s_vertex_t *v2,
                               const material3d_t *mat)
{
    /* Calculate 2D screen bounding box */
    float min_x_f = fminf(v0->x, fminf(v1->x, v2->x));
    float max_x_f = fmaxf(v0->x, fmaxf(v1->x, v2->x));
    float min_y_f = fminf(v0->y, fminf(v1->y, v2->y));
    float max_y_f = fmaxf(v0->y, fmaxf(v1->y, v2->y));

    int min_x = (int)floorf(min_x_f);
    int max_x = (int)ceilf(max_x_f);
    int min_y = (int)floorf(min_y_f);
    int max_y = (int)ceilf(max_y_f);

    /* Viewport clamp */
    if (min_x < 0) min_x = 0;
    if (max_x >= ctx->buf_w) max_x = ctx->buf_w - 1;
    if (min_y < 0) min_y = 0;
    if (max_y >= ctx->buf_h) max_y = ctx->buf_h - 1;

    if (min_x > max_x || min_y > max_y) return;

    /* 2D Cross product signed area */
    float area = (v1->x - v0->x) * (v2->y - v0->y) - (v1->y - v0->y) * (v2->x - v0->x);
    if (fabsf(area) < 1e-5f) return;
    float inv_area = 1.0f / area;

    /* Flat shading face normal */
    vec3_t face_normal = vec3_normalize(vec3_add(v0->normal, vec3_add(v1->normal, v2->normal)));

    for (int py = min_y; py <= max_y; py++) {
        float y = (float)py + 0.5f;
        for (int px = min_x; px <= max_x; px++) {
            float x = (float)px + 0.5f;

            /* Barycentric coordinates */
            float w0 = ((v1->x - x) * (v2->y - y) - (v1->y - y) * (v2->x - x)) * inv_area;
            float w1 = ((v2->x - x) * (v0->y - y) - (v2->y - y) * (v0->x - x)) * inv_area;
            float w2 = 1.0f - w0 - w1;

            if (w0 >= -1e-4f && w1 >= -1e-4f && w2 >= -1e-4f) {
                /* Depth interpolation */
                float z = w0 * v0->z + w1 * v1->z + w2 * v2->z;
                if (z < 0.0f || z > 1.0f) continue;

                if (zbuffer_test_and_set(ctx->zbuf, px, py, z)) {
                    /* Perspective-correct attributes */
                    float inv_w = w0 * v0->inv_w + w1 * v1->inv_w + w2 * v2->inv_w;
                    float w_real = inv_w > 1e-7f ? (1.0f / inv_w) : 1.0f;

                    vec2_t uv = vec2(
                        (w0 * v0->uv.x * v0->inv_w + w1 * v1->uv.x * v1->inv_w + w2 * v2->uv.x * v2->inv_w) * w_real,
                        (w0 * v0->uv.y * v0->inv_w + w1 * v1->uv.y * v1->inv_w + w2 * v2->uv.y * v2->inv_w) * w_real
                    );

                    vec3_t norm;
                    if (ctx->mode == RENDER3D_FLAT) {
                        norm = face_normal;
                    } else {
                        norm = vec3_normalize(vec3(
                            w0 * v0->normal.x + w1 * v1->normal.x + w2 * v2->normal.x,
                            w0 * v0->normal.y + w1 * v1->normal.y + w2 * v2->normal.y,
                            w0 * v0->normal.z + w1 * v1->normal.z + w2 * v2->normal.z
                        ));
                    }

                    uint32_t color = shade_pixel(ctx, mat, norm, uv, z, v0->color);
                    ctx->target_buf[py * ctx->buf_w + px] = color;
                }
            }
        }
    }
}

void render3d_draw_mesh(render3d_ctx_t *ctx, const mesh3d_t *mesh, mat4_t model_matrix)
{
    if (!ctx || !mesh || !ctx->target_buf || mesh->num_indices < 3) return;

    /* Model-View-Projection Matrix */
    mat4_t mvp = mat4_mul(ctx->vp_matrix, model_matrix);
    /* Normal transformation matrix: transpose of inverse of model matrix */
    mat4_t normal_mat = mat4_transpose(mat4_inverse(model_matrix));

    s_vertex_t *s_verts = (s_vertex_t *)malloc(sizeof(s_vertex_t) * mesh->num_vertices);
    if (!s_verts) return;

    /* Transform all vertices */
    float half_w = (float)ctx->buf_w * 0.5f;
    float half_h = (float)ctx->buf_h * 0.5f;

    for (int i = 0; i < mesh->num_vertices; i++) {
        const vertex3d_t *v = &mesh->vertices[i];
        vec4_t clip = mat4_mul_vec4(mvp, vec4_from_vec3(v->pos, 1.0f));

        s_verts[i].w = clip.w;
        if (fabsf(clip.w) > 1e-6f) {
            s_verts[i].inv_w = 1.0f / clip.w;
            s_verts[i].x = (clip.x * s_verts[i].inv_w + 1.0f) * half_w;
            s_verts[i].y = (1.0f - clip.y * s_verts[i].inv_w) * half_h;
            s_verts[i].z = (clip.z * s_verts[i].inv_w + 1.0f) * 0.5f;
        } else {
            s_verts[i].inv_w = 1.0f;
            s_verts[i].x = 0; s_verts[i].y = 0; s_verts[i].z = 0;
        }

        s_verts[i].normal = vec3_normalize(mat4_mul_dir(normal_mat, v->normal));
        s_verts[i].uv     = v->uv;
        s_verts[i].color  = v->color ? v->color : mesh->material.diffuse_color;
    }

    /* Process each triangle */
    int num_triangles = mesh->num_indices / 3;
    for (int t = 0; t < num_triangles; t++) {
        uint16_t i0 = mesh->indices[t * 3 + 0];
        uint16_t i1 = mesh->indices[t * 3 + 1];
        uint16_t i2 = mesh->indices[t * 3 + 2];

        if (i0 >= mesh->num_vertices || i1 >= mesh->num_vertices || i2 >= mesh->num_vertices)
            continue;

        const s_vertex_t *v0 = &s_verts[i0];
        const s_vertex_t *v1 = &s_verts[i1];
        const s_vertex_t *v2 = &s_verts[i2];

        /* Near plane clipping */
        if (v0->w < 0.1f && v1->w < 0.1f && v2->w < 0.1f) continue;

        /* Backface culling in screen space (signed area) */
        float area = (v1->x - v0->x) * (v2->y - v0->y) - (v1->y - v0->y) * (v2->x - v0->x);
        if (ctx->cull_backfaces && area <= 0.0f) continue;

        ctx->tri_count_frame++;

        if (ctx->mode == RENDER3D_WIREFRAME) {
            /* Wireframe line edges */
            vec3_t p0 = mesh->vertices[i0].pos;
            vec3_t p1 = mesh->vertices[i1].pos;
            vec3_t p2 = mesh->vertices[i2].pos;

            vec3_t wp0 = mat4_mul_point(model_matrix, p0);
            vec3_t wp1 = mat4_mul_point(model_matrix, p1);
            vec3_t wp2 = mat4_mul_point(model_matrix, p2);

            render3d_draw_line(ctx, wp0, wp1, v0->color);
            render3d_draw_line(ctx, wp1, wp2, v1->color);
            render3d_draw_line(ctx, wp2, wp0, v2->color);
        } else {
            rasterize_triangle(ctx, v0, v1, v2, &mesh->material);

            if (ctx->wire_overlay) {
                vec3_t p0 = mesh->vertices[i0].pos;
                vec3_t p1 = mesh->vertices[i1].pos;
                vec3_t p2 = mesh->vertices[i2].pos;

                vec3_t wp0 = mat4_mul_point(model_matrix, p0);
                vec3_t wp1 = mat4_mul_point(model_matrix, p1);
                vec3_t wp2 = mat4_mul_point(model_matrix, p2);

                render3d_draw_line(ctx, wp0, wp1, ctx->wire_color);
                render3d_draw_line(ctx, wp1, wp2, ctx->wire_color);
                render3d_draw_line(ctx, wp2, wp0, ctx->wire_color);
            }
        }
    }

    free(s_verts);
}
