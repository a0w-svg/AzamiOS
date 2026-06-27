/**
 * gl.c — AzamiGL Software OpenGL 1.1 Implementation
 */
#include "include/GL/gl.h"
#include "include/math.h"
#include "include/gui.h"
#include "include/string.h"

/* Viewport state */
static int g_vp_x = 0;
static int g_vp_y = 0;
static int g_vp_w = 640;
static int g_vp_h = 480;

/* Clear color */
static uint32_t g_clear_color = 0x00000000;

/* Depth buffer (supports up to 640x480 resolution) */
static float g_z_buffer[640 * 480];
static bool  g_depth_test = true;

/* Matrix stacks */
static float g_modelview[16];
static float g_projection[16];
static float *g_current_matrix = g_modelview;

static float g_mv_stack[16 * 16];
static int   g_mv_top = 0;
static float g_proj_stack[16 * 16];
static int   g_proj_top = 0;

/* Primitive drawing state */
static GLenum g_prim_mode = GL_TRIANGLES;
static float  g_cur_r = 1.0f, g_cur_g = 1.0f, g_cur_b = 1.0f;

typedef struct {
    float x, y, z, w;
    float r, g, b;
} gl_vertex_t;

static gl_vertex_t g_vbuf[4];
static int g_vcount = 0;

void glViewport(int x, int y, int width, int height) {
    g_vp_x = x;
    g_vp_y = y;
    g_vp_w = width;
    g_vp_h = height;
}

void glClearColor(float red, float green, float blue, float alpha) {
    (void)alpha;
    int r = (int)(red * 255.0f);   if (r < 0) r = 0; if (r > 255) r = 255;
    int g = (int)(green * 255.0f); if (g < 0) g = 0; if (g > 255) g = 255;
    int b = (int)(blue * 255.0f);  if (b < 0) b = 0; if (b > 255) b = 255;
    g_clear_color = (r << 16) | (g << 8) | b;
}

void glClear(GLenum mask) {
    if (mask & GL_COLOR_BUFFER_BIT) {
        draw_rect(g_vp_x, g_vp_y, g_vp_w, g_vp_h, g_clear_color);
    }
    if (mask & GL_DEPTH_BUFFER_BIT) {
        int x1 = g_vp_x, y1 = g_vp_y;
        int x2 = g_vp_x + g_vp_w, y2 = g_vp_y + g_vp_h;
        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 > 640) x2 = 640;
        if (y2 > 480) y2 = 480;
        for (int y = y1; y < y2; y++) {
            for (int x = x1; x < x2; x++) {
                g_z_buffer[y * 640 + x] = 1e30f;
            }
        }
    }
}

void glEnable(GLenum cap) {
    if (cap == GL_DEPTH_TEST) g_depth_test = true;
}

void glDisable(GLenum cap) {
    if (cap == GL_DEPTH_TEST) g_depth_test = false;
}

void glMatrixMode(GLenum mode) {
    if (mode == GL_PROJECTION) g_current_matrix = g_projection;
    else g_current_matrix = g_modelview;
}

void glLoadIdentity(void) {
    for (int i = 0; i < 16; i++) g_current_matrix[i] = 0.0f;
    g_current_matrix[0] = 1.0f;
    g_current_matrix[5] = 1.0f;
    g_current_matrix[10] = 1.0f;
    g_current_matrix[15] = 1.0f;
}

static void mult_matrix(const float *b) {
    float res[16];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            res[c*4 + r] = 0.0f;
            for (int k = 0; k < 4; k++) {
                res[c*4 + r] += g_current_matrix[k*4 + r] * b[c*4 + k];
            }
        }
    }
    for (int i = 0; i < 16; i++) g_current_matrix[i] = res[i];
}

void glPushMatrix(void) {
    if (g_current_matrix == g_modelview) {
        if (g_mv_top < 16) {
            memcpy(&g_mv_stack[g_mv_top * 16], g_modelview, sizeof(g_modelview));
            g_mv_top++;
        }
    } else {
        if (g_proj_top < 16) {
            memcpy(&g_proj_stack[g_proj_top * 16], g_projection, sizeof(g_projection));
            g_proj_top++;
        }
    }
}

void glPopMatrix(void) {
    if (g_current_matrix == g_modelview) {
        if (g_mv_top > 0) {
            g_mv_top--;
            memcpy(g_modelview, &g_mv_stack[g_mv_top * 16], sizeof(g_modelview));
        }
    } else {
        if (g_proj_top > 0) {
            g_proj_top--;
            memcpy(g_projection, &g_proj_stack[g_proj_top * 16], sizeof(g_projection));
        }
    }
}

void glTranslatef(float x, float y, float z) {
    float t[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        x,    y,    z,    1.0f
    };
    mult_matrix(t);
}

void glScalef(float x, float y, float z) {
    float s[16] = {
        x,    0.0f, 0.0f, 0.0f,
        0.0f, y,    0.0f, 0.0f,
        0.0f, 0.0f, z,    0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    mult_matrix(s);
}

void glRotatef(float angle, float x, float y, float z) {
    float rad = angle * M_PI / 180.0f;
    float len = sqrt(x*x + y*y + z*z);
    if (len == 0.0f) return;
    x /= len; y /= len; z /= len;

    float c = cos(rad);
    float s = sin(rad);
    float t = 1.0f - c;

    float r[16] = {
        t*x*x + c,   t*x*y + s*z, t*x*z - s*y, 0.0f,
        t*x*y - s*z, t*y*y + c,   t*y*z + s*x, 0.0f,
        t*x*z + s*y, t*y*z - s*x, t*z*z + c,   0.0f,
        0.0f,        0.0f,        0.0f,        1.0f
    };
    mult_matrix(r);
}

void glOrtho(float left, float right, float bottom, float top, float zNear, float zFar) {
    float o[16] = {
        2.0f / (right - left), 0.0f, 0.0f, 0.0f,
        0.0f, 2.0f / (top - bottom), 0.0f, 0.0f,
        0.0f, 0.0f, -2.0f / (zFar - zNear), 0.0f,
        -(right + left) / (right - left), -(top + bottom) / (top - bottom), -(zFar + zNear) / (zFar - zNear), 1.0f
    };
    mult_matrix(o);
}

void gluPerspective(float fovy, float aspect, float zNear, float zFar) {
    float f = 1.0f / tan(fovy * M_PI / 360.0f);
    float p[16] = {
        f / aspect, 0.0f, 0.0f, 0.0f,
        0.0f, f, 0.0f, 0.0f,
        0.0f, 0.0f, (zFar + zNear) / (zNear - zFar), -1.0f,
        0.0f, 0.0f, (2.0f * zFar * zNear) / (zNear - zFar), 0.0f
    };
    mult_matrix(p);
}

void glBegin(GLenum mode) {
    g_prim_mode = mode;
    g_vcount = 0;
}

void glColor3f(float red, float green, float blue) {
    g_cur_r = red;
    g_cur_g = green;
    g_cur_b = blue;
}

/* Internal rasterization helper for triangle (v0, v1, v2) */
static void rasterize_triangle(const gl_vertex_t *v0, const gl_vertex_t *v1, const gl_vertex_t *v2) {
    /* Bounding box */
    int min_x = (int)floor(fmin(v0->x, fmin(v1->x, v2->x)));
    int max_x = (int)ceil(fmax(v0->x, fmax(v1->x, v2->x)));
    int min_y = (int)floor(fmin(v0->y, fmin(v1->y, v2->y)));
    int max_y = (int)ceil(fmax(v0->y, fmax(v1->y, v2->y)));

    int vp_x2 = g_vp_x + g_vp_w - 1;
    int vp_y2 = g_vp_y + g_vp_h - 1;

    if (min_x < g_vp_x) min_x = g_vp_x;
    if (max_x > vp_x2)  max_x = vp_x2;
    if (min_y < g_vp_y) min_y = g_vp_y;
    if (max_y > vp_y2)  max_y = vp_y2;

    if (min_x > max_x || min_y > max_y) return;

    float dx12 = v1->x - v2->x;
    float dy12 = v1->y - v2->y;
    float dx20 = v2->x - v0->x;
    float dy20 = v2->y - v0->y;

    float area = (v2->x - v0->x) * (v1->y - v0->y) - (v2->y - v0->y) * (v1->x - v0->x);
    if (fabs(area) < 0.0001f) return;
    float inv_area = 1.0f / area;

    for (int y = min_y; y <= max_y; y++) {
        if (y < 0 || y >= 480) continue;
        float py = (float)y + 0.5f;
        for (int x = min_x; x <= max_x; x++) {
            if (x < 0 || x >= 640) continue;
            float px = (float)x + 0.5f;

            float w0 = (dx12 * (py - v2->y) - dy12 * (px - v2->x)) * inv_area;
            float w1 = (dx20 * (py - v0->y) - dy20 * (px - v0->x)) * inv_area;
            float w2 = 1.0f - w0 - w1;

            if (w0 >= -0.001f && w1 >= -0.001f && w2 >= -0.001f) {
                float z = w0 * v0->z + w1 * v1->z + w2 * v2->z;
                int idx = y * 640 + x;
                if (!g_depth_test || z < g_z_buffer[idx]) {
                    g_z_buffer[idx] = z;
                    float r = w0 * v0->r + w1 * v1->r + w2 * v2->r;
                    float g = w0 * v0->g + w1 * v1->g + w2 * v2->g;
                    float b = w0 * v0->b + w1 * v1->b + w2 * v2->b;
                    int ir = (int)(r * 255.0f); if (ir < 0) ir = 0; if (ir > 255) ir = 255;
                    int ig = (int)(g * 255.0f); if (ig < 0) ig = 0; if (ig > 255) ig = 255;
                    int ib = (int)(b * 255.0f); if (ib < 0) ib = 0; if (ib > 255) ib = 255;
                    uint32_t col = (ir << 16) | (ig << 8) | ib;
                    draw_pixel(x, y, col);
                }
            }
        }
    }
}

static void transform_vertex(float in_x, float in_y, float in_z, gl_vertex_t *out_v) {
    /* 1. Modelview transformation */
    float mv_x = g_modelview[0]*in_x + g_modelview[4]*in_y + g_modelview[8]*in_z  + g_modelview[12];
    float mv_y = g_modelview[1]*in_x + g_modelview[5]*in_y + g_modelview[9]*in_z  + g_modelview[13];
    float mv_z = g_modelview[2]*in_x + g_modelview[6]*in_y + g_modelview[10]*in_z + g_modelview[14];
    float mv_w = g_modelview[3]*in_x + g_modelview[7]*in_y + g_modelview[11]*in_z + g_modelview[15];

    /* 2. Projection transformation */
    float pr_x = g_projection[0]*mv_x + g_projection[4]*mv_y + g_projection[8]*mv_z  + g_projection[12]*mv_w;
    float pr_y = g_projection[1]*mv_x + g_projection[5]*mv_y + g_projection[9]*mv_z  + g_projection[13]*mv_w;
    float pr_z = g_projection[2]*mv_x + g_projection[6]*mv_y + g_projection[10]*mv_z + g_projection[14]*mv_w;
    float pr_w = g_projection[3]*mv_x + g_projection[7]*mv_y + g_projection[11]*mv_z + g_projection[15]*mv_w;

    if (fabs(pr_w) < 0.00001f) pr_w = 0.00001f;
    float ndc_x = pr_x / pr_w;
    float ndc_y = pr_y / pr_w;
    float ndc_z = pr_z / pr_w;

    out_v->x = (float)g_vp_x + (ndc_x + 1.0f) * 0.5f * (float)g_vp_w;
    out_v->y = (float)g_vp_y + (1.0f - ndc_y) * 0.5f * (float)g_vp_h;
    out_v->z = ndc_z;
    out_v->w = pr_w;
    out_v->r = g_cur_r;
    out_v->g = g_cur_g;
    out_v->b = g_cur_b;
}

void glVertex3f(float x, float y, float z) {
    if (g_vcount < 4) {
        transform_vertex(x, y, z, &g_vbuf[g_vcount]);
        g_vcount++;
    }

    if (g_prim_mode == GL_TRIANGLES && g_vcount == 3) {
        rasterize_triangle(&g_vbuf[0], &g_vbuf[1], &g_vbuf[2]);
        g_vcount = 0;
    } else if (g_prim_mode == GL_QUADS && g_vcount == 4) {
        rasterize_triangle(&g_vbuf[0], &g_vbuf[1], &g_vbuf[2]);
        rasterize_triangle(&g_vbuf[0], &g_vbuf[2], &g_vbuf[3]);
        g_vcount = 0;
    }
}

void glEnd(void) {
    g_vcount = 0;
}
