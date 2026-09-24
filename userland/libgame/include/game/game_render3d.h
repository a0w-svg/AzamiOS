/* ============================================================================
 * AzamiOS Game Framework — 3D Graphics Engine & Software Rasterizer
 * File: userland/libgame/include/game/game_render3d.h
 *
 * Full-featured 3D software rendering pipeline:
 *   • Z-buffer depth testing & writing
 *   • Orbit & first-person 3D cameras
 *   • Barycentric triangle rasterizer with sub-pixel precision
 *   • Shading models: Wireframe, Flat, Gouraud, Textured, Normal & Depth maps
 *   • Directional and ambient lighting with Blinn-Phong specular highlights
 *   • Procedural 3D mesh generators: Cube, UV Sphere, Torus, Pyramid, Cylinder, Grid
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "game_math3d.h"

/* ── Z-Buffer (Depth Buffer) ─────────────────────────────────────────────── */

typedef struct {
    float *data;
    int    width;
    int    height;
} zbuffer_t;

zbuffer_t *zbuffer_create(int width, int height);
void       zbuffer_destroy(zbuffer_t *zb);
void       zbuffer_resize(zbuffer_t *zb, int width, int height);
void       zbuffer_clear(zbuffer_t *zb, float clear_depth);

static inline bool zbuffer_test_and_set(zbuffer_t *zb, int x, int y, float depth)
{
    if (!zb || !zb->data || x < 0 || x >= zb->width || y < 0 || y >= zb->height)
        return false;
    int idx = y * zb->width + x;
    if (depth < zb->data[idx]) {
        zb->data[idx] = depth;
        return true;
    }
    return false;
}

/* ── Geometry & Primitives ────────────────────────────────────────────────── */

typedef struct {
    vec3_t   pos;       /* 3D local position            */
    vec3_t   normal;    /* Vertex normal                */
    vec2_t   uv;        /* Texture coordinate [0..1]    */
    uint32_t color;     /* ARGB32 base vertex color     */
} vertex3d_t;

typedef struct {
    vertex3d_t v[3];
} triangle3d_t;

/* ── Shading & Render Modes ──────────────────────────────────────────────── */

typedef enum {
    RENDER3D_WIREFRAME = 0, /* Wireframe edges only                       */
    RENDER3D_FLAT,          /* Flat-shaded with Lambertian diffuse light  */
    RENDER3D_GOURAUD,       /* Smooth Gouraud interpolated vertex normals */
    RENDER3D_TEXTURED,      /* Perspective-correct texture mapping        */
    RENDER3D_NORMALS,       /* Normal vectors mapped to RGB colors        */
    RENDER3D_DEPTH          /* Depth gradient heatmap                     */
} render3d_mode_t;

/* ── Material & Texture ──────────────────────────────────────────────────── */

typedef struct {
    uint32_t  diffuse_color;  /* ARGB32 solid base color                  */
    uint32_t *texture;        /* Optional ARGB32 texture buffer           */
    int       tex_w;          /* Texture width                            */
    int       tex_h;          /* Texture height                           */
    float     specular_exp;   /* Specular glossiness exponent             */
    float     specular_str;   /* Specular highlight strength [0..1]       */
} material3d_t;

static inline material3d_t material3d_default(uint32_t color)
{
    material3d_t m;
    memset(&m, 0, sizeof(m));
    m.diffuse_color = color;
    m.specular_exp  = 16.0f;
    m.specular_str  = 0.3f;
    return m;
}

/* ── Lighting ─────────────────────────────────────────────────────────────── */

typedef struct {
    vec3_t dir;         /* Normalized vector pointing towards the light */
    vec3_t color;       /* Light RGB intensity [0..1]                   */
    vec3_t ambient;     /* Ambient RGB intensity [0..1]                 */
} light3d_t;

static inline light3d_t light3d_default(void)
{
    light3d_t l;
    l.dir     = vec3_normalize(vec3(0.5f, 1.0f, 0.7f));
    l.color   = vec3(1.0f, 0.95f, 0.9f);
    l.ambient = vec3(0.2f, 0.22f, 0.25f);
    return l;
}

/* ── 3D Camera ───────────────────────────────────────────────────────────── */

typedef struct {
    vec3_t pos;
    vec3_t target;
    vec3_t up;
    float  fov_rad;
    float  aspect;
    float  near_z;
    float  far_z;
    float  yaw;         /* Orbit horizontal angle in radians */
    float  pitch;       /* Orbit vertical angle in radians   */
    float  orbit_dist;  /* Distance from target              */
    bool   is_ortho;
    float  ortho_size;
} camera3d_t;

camera3d_t camera3d_create(float aspect);
mat4_t     camera3d_get_view(const camera3d_t *cam);
mat4_t     camera3d_get_proj(const camera3d_t *cam);
void       camera3d_orbit(camera3d_t *cam, float delta_yaw, float delta_pitch);
void       camera3d_zoom(camera3d_t *cam, float delta_dist);
void       camera3d_update_orbit_pos(camera3d_t *cam);

/* ── 3D Mesh ─────────────────────────────────────────────────────────────── */

typedef struct {
    vertex3d_t   *vertices;
    int           num_vertices;
    uint16_t     *indices;
    int           num_indices;
    material3d_t  material;
    vec3_t        min_bounds;
    vec3_t        max_bounds;
} mesh3d_t;

void     mesh3d_destroy(mesh3d_t *mesh);
mesh3d_t *mesh3d_create_cube(float size, uint32_t color);
mesh3d_t *mesh3d_create_sphere(float radius, int lat_segs, int lon_segs, uint32_t color);
mesh3d_t *mesh3d_create_torus(float r_major, float r_minor, int segs_u, int segs_v, uint32_t color);
mesh3d_t *mesh3d_create_pyramid(float base_size, float height, uint32_t color);
mesh3d_t *mesh3d_create_cylinder(float radius, float height, int segs, uint32_t color);
mesh3d_t *mesh3d_create_grid(float size, int divisions, uint32_t color);

/* ── 3D Render Context ───────────────────────────────────────────────────── */

typedef struct {
    uint32_t       *target_buf;       /* ARGB32 destination pixel buffer    */
    int             buf_w;            /* Buffer width in pixels             */
    int             buf_h;            /* Buffer height in pixels            */
    zbuffer_t      *zbuf;             /* Depth buffer                       */
    render3d_mode_t mode;             /* Active rendering / shading mode    */
    light3d_t       light;            /* Scene light parameters             */
    mat4_t          view_matrix;      /* Current view matrix                */
    mat4_t          proj_matrix;      /* Current projection matrix          */
    mat4_t          vp_matrix;        /* View * Projection cached           */
    bool            cull_backfaces;   /* Cull triangles facing away         */
    bool            wire_overlay;     /* Overlay wireframe on shaded faces  */
    uint32_t        wire_color;       /* Wireframe edge color               */
    int             tri_count_frame;  /* Triangle draw count this frame     */
} render3d_ctx_t;

render3d_ctx_t *render3d_create(int width, int height);
void            render3d_destroy(render3d_ctx_t *ctx);
void            render3d_resize(render3d_ctx_t *ctx, int new_w, int new_h);
void            render3d_set_target(render3d_ctx_t *ctx, uint32_t *target_buf, int w, int h);
void            render3d_clear(render3d_ctx_t *ctx, uint32_t clear_color);
void            render3d_set_camera(render3d_ctx_t *ctx, const camera3d_t *cam);

/* Draw primitives */
void render3d_draw_line(render3d_ctx_t *ctx, vec3_t p0, vec3_t p1, uint32_t color);
void render3d_draw_mesh(render3d_ctx_t *ctx, const mesh3d_t *mesh, mat4_t model_matrix);
