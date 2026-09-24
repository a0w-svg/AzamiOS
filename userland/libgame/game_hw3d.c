/* ============================================================================
 * AzamiOS Game Framework — Hardware 3D (Virgl API wrapper)
 * File: userland/libgame/game_hw3d.c
 *
 * See game_hw3d.h for the sourcing notes on the protocol constants used
 * here (all confirmed against real Mesa/virglrenderer source, not recalled
 * from memory or guessed).
 * ============================================================================ */

#include "include/game/game_hw3d.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <azami/drm.h>

struct hw3d_context {
    int drm_fd;
    bool is_supported;
};

hw3d_context_t *hw3d_init(void)
{
    hw3d_context_t *ctx = malloc(sizeof(hw3d_context_t));
    if (!ctx) return NULL;

    /* Open the DRM render node or card */
    ctx->drm_fd = open("/dev/dri/card0", O_RDWR);
    if (ctx->drm_fd < 0) {
        free(ctx);
        return NULL;
    }

    /* Query 3D capabilities via GETPARAM */
    struct drm_virtgpu_getparam p = {0};
    p.param = VIRTGPU_PARAM_3D_FEATURES;

    if (ioctl(ctx->drm_fd, DRM_IOCTL_VIRTGPU_GETPARAM, &p) == 0 && p.value == 1) {
        ctx->is_supported = true;
    } else {
        ctx->is_supported = false;
    }

    return ctx;
}

bool hw3d_is_supported(hw3d_context_t *ctx)
{
    if (!ctx) return false;
    return ctx->is_supported;
}

int hw3d_context_create(hw3d_context_t *ctx, uint32_t ctx_id)
{
    if (!ctx || !ctx->is_supported) return -1;

    struct drm_virtgpu_context_init ci = {0};
    ci.ctx_id = ctx_id;

    return ioctl(ctx->drm_fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &ci);
}

int hw3d_resource_create(hw3d_context_t *ctx, uint32_t target, uint32_t format, uint32_t bind, uint32_t width, uint32_t height, uint32_t depth, uint32_t array_size, uint32_t *out_handle)
{
    if (!ctx || !ctx->is_supported || !out_handle) return -1;

    struct drm_virtgpu_resource_create rc = {0};
    rc.target = target;
    rc.format = format;
    rc.bind = bind;
    rc.width = width;
    rc.height = height;
    rc.depth = depth;
    rc.array_size = array_size;

    if (ioctl(ctx->drm_fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &rc) < 0) {
        return -1;
    }

    *out_handle = rc.bo_handle;
    return 0;
}

int hw3d_submit_cmd(hw3d_context_t *ctx, uint32_t ctx_id, void *cmd_buf, uint32_t size)
{
    if (!ctx || !ctx->is_supported) return -1;

    struct drm_virtgpu_execbuffer exec = {0};
    exec.command = (uintptr_t)cmd_buf;
    exec.size = size;
    exec.ring_idx = ctx_id;

    return ioctl(ctx->drm_fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &exec);
}

/* Common helper: map a GEM handle's backing pages for CPU access. Real path
 * (not a shortcut): DRM_IOCTL_VIRTGPU_MAP gives the fake mmap() offset for
 * the handle (assigned at GEM object creation, see drm_gem_object_create()),
 * then mmap() on the DRM fd at that offset actually walks the object's
 * individually-allocated physical pages into this process's address space
 * (see drm_mmap() in drivers/gpu/drm/drm_drv.c) — the same path any real
 * DRM/Mesa userspace client uses for a dumb or virtgpu buffer object. */
static void *hw3d_map_handle(hw3d_context_t *ctx, uint32_t handle, uint32_t size)
{
    struct drm_virtgpu_map m = {0};
    m.handle = handle;
    if (ioctl(ctx->drm_fd, DRM_IOCTL_VIRTGPU_MAP, &m) < 0) return NULL;

    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->drm_fd, (off_t)m.offset);
    if (p == MAP_FAILED) return NULL;
    return p;
}

int hw3d_transfer_to_host(hw3d_context_t *ctx, uint32_t res_handle, const void *data, uint32_t size)
{
    if (!ctx || !ctx->is_supported || !data || size == 0) return -1;

    void *p = hw3d_map_handle(ctx, res_handle, size);
    if (!p) return -1;
    memcpy(p, data, size);
    munmap(p, size);

    struct drm_virtgpu_3d_transfer xfer = {0};
    xfer.box.x = 0; xfer.box.y = 0; xfer.box.z = 0;
    xfer.box.w = size; xfer.box.h = 1; xfer.box.d = 1;
    xfer.bo_handle = res_handle;
    xfer.level = 0;
    xfer.stride = size;
    xfer.layer_stride = size;

    return ioctl(ctx->drm_fd, DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST, &xfer);
}

int hw3d_transfer_from_host(hw3d_context_t *ctx, uint32_t res_handle, void *out, uint32_t size)
{
    if (!ctx || !ctx->is_supported || !out || size == 0) return -1;

    struct drm_virtgpu_3d_transfer xfer = {0};
    xfer.box.x = 0; xfer.box.y = 0; xfer.box.z = 0;
    xfer.box.w = size; xfer.box.h = 1; xfer.box.d = 1;
    xfer.bo_handle = res_handle;
    xfer.level = 0;
    xfer.stride = size;
    xfer.layer_stride = size;

    if (ioctl(ctx->drm_fd, DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, &xfer) < 0)
        return -1;

    void *p = hw3d_map_handle(ctx, res_handle, size);
    if (!p) return -1;
    memcpy(out, p, size);
    munmap(p, size);
    return 0;
}

int hw3d_transfer_to_host_2d_box(hw3d_context_t *ctx, uint32_t res_handle,
                                 const void *data, uint32_t w, uint32_t h, uint32_t stride)
{
    if (!ctx || !ctx->is_supported || !data || w == 0 || h == 0) return -1;
    uint32_t size = stride * h;

    void *p = hw3d_map_handle(ctx, res_handle, size);
    if (!p) return -1;
    memcpy(p, data, size);
    munmap(p, size);

    struct drm_virtgpu_3d_transfer xfer = {0};
    xfer.box.x = 0; xfer.box.y = 0; xfer.box.z = 0;
    xfer.box.w = w; xfer.box.h = h; xfer.box.d = 1;
    xfer.bo_handle = res_handle;
    xfer.level = 0;
    xfer.stride = stride;
    xfer.layer_stride = size;

    return ioctl(ctx->drm_fd, DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST, &xfer);
}

int hw3d_transfer_from_host_2d_box(hw3d_context_t *ctx, uint32_t res_handle,
                                   void *out, uint32_t w, uint32_t h, uint32_t stride)
{
    if (!ctx || !ctx->is_supported || !out || w == 0 || h == 0) return -1;
    uint32_t size = stride * h;

    struct drm_virtgpu_3d_transfer xfer = {0};
    xfer.box.x = 0; xfer.box.y = 0; xfer.box.z = 0;
    xfer.box.w = w; xfer.box.h = h; xfer.box.d = 1;
    xfer.bo_handle = res_handle;
    xfer.level = 0;
    xfer.stride = stride;
    xfer.layer_stride = size;

    if (ioctl(ctx->drm_fd, DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, &xfer) < 0)
        return -1;

    void *p = hw3d_map_handle(ctx, res_handle, size);
    if (!p) return -1;
    memcpy(out, p, size);
    munmap(p, size);
    return 0;
}

void hw3d_shutdown(hw3d_context_t *ctx)
{
    if (!ctx) return;
    if (ctx->drm_fd >= 0) {
        close(ctx->drm_fd);
    }
    free(ctx);
}

/* ── Fixed-function Virgl command encoders ───────────────────────────────── */

#define VIRGL_BUILD_HDR(cmd, obj, len) \
    (((uint32_t)(len) << 16) | (((uint32_t)(obj) & 0xFF) << 8) | ((uint32_t)(cmd) & 0xFF))

/* enum virgl_context_cmd values (src/virtio/virtio-gpu/virgl_protocol.h) */
#define VIRGL_CCMD_CREATE_OBJECT        1
#define VIRGL_CCMD_BIND_OBJECT          2
#define VIRGL_CCMD_SET_VIEWPORT_STATE   4
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE 5
#define VIRGL_CCMD_SET_VERTEX_BUFFERS   6
#define VIRGL_CCMD_CLEAR                7
#define VIRGL_CCMD_DRAW_VBO             8
#define VIRGL_CCMD_BIND_SHADER          31

int virgl_build_clear(uint32_t *buf, uint32_t max_dwords, uint32_t buffers, float r, float g, float b, float a, double depth, uint32_t stencil)
{
    if (!buf || max_dwords < 9) return -1;

    buf[0] = VIRGL_BUILD_HDR(VIRGL_CCMD_CLEAR, 0, 8);
    buf[1] = buffers;
    *(float *)&buf[2] = r;
    *(float *)&buf[3] = g;
    *(float *)&buf[4] = b;
    *(float *)&buf[5] = a;

    union { double d; uint64_t u; } du;
    du.d = depth;
    buf[6] = (uint32_t)(du.u & 0xFFFFFFFFu);
    buf[7] = (uint32_t)(du.u >> 32);
    buf[8] = stencil;

    return 9;
}

int virgl_build_set_viewport(uint32_t *buf, uint32_t max_dwords, float scale_x, float scale_y, float scale_z, float trans_x, float trans_y, float trans_z)
{
    if (!buf || max_dwords < 8) return -1;

    buf[0] = VIRGL_BUILD_HDR(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7);
    buf[1] = 0; /* start_slot */
    *(float *)&buf[2] = scale_x;
    *(float *)&buf[3] = scale_y;
    *(float *)&buf[4] = scale_z;
    *(float *)&buf[5] = trans_x;
    *(float *)&buf[6] = trans_y;
    *(float *)&buf[7] = trans_z;

    return 8;
}

int virgl_build_set_framebuffer(uint32_t *buf, uint32_t max_dwords, uint32_t nr_cbufs, uint32_t surf_handle, uint32_t zsurf_handle)
{
    if (!buf || max_dwords < 4) return -1;

    buf[0] = VIRGL_BUILD_HDR(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
    buf[1] = nr_cbufs;
    buf[2] = zsurf_handle;
    buf[3] = surf_handle;

    return 4;
}

int virgl_build_draw_vbo(uint32_t *buf, uint32_t max_dwords, uint32_t mode, uint32_t start, uint32_t count)
{
    if (!buf || max_dwords < 13) return -1;

    buf[0] = VIRGL_BUILD_HDR(VIRGL_CCMD_DRAW_VBO, 0, 12);
    buf[1] = start;
    buf[2] = count;
    buf[3] = mode;
    buf[4] = 0;  /* indexed */
    buf[5] = 1;  /* instance count */
    buf[6] = 0;  /* index bias */
    buf[7] = 0;  /* start instance */
    buf[8] = 0;  /* primitive restart */
    buf[9] = 0;  /* restart index */
    buf[10] = 0; /* min index */
    buf[11] = ~0u; /* max index */
    buf[12] = 0; /* count from stream output */

    return 13;
}

/* VIRGL_CCMD_CREATE_OBJECT / VIRGL_OBJECT_SHADER.
 * Layout (src/virtio/virtio-gpu/virgl_protocol.h VIRGL_OBJ_SHADER_*, and
 * cross-checked against vrend_decode_create_shader()):
 *   word0: CMD0(CREATE_OBJECT, SHADER, len)
 *   word1: handle
 *   word2: shader stage (virgl_shader_stage: VERTEX=0, FRAGMENT=1)
 *   word3: offlen — for a single (non-continuation) packet, the *total byte
 *          length* of the TGSI text including its NUL terminator (top bit,
 *          VIRGL_OBJ_SHADER_OFFSET_CONT, clear)
 *   word4: num_tokens — a generous *upper bound* on the equivalent binary
 *          TGSI token count; the host allocates its parse buffer sized to
 *          this (calloc(num_tokens+10, ...)) and tgsi_text_translate()
 *          fails if it's too small, so this deliberately overestimates
 *          rather than trying to count exactly.
 *   word5: stream-out output count (0 — this file never uses stream-out)
 *   word6..: the TGSI text bytes, NUL-padded to a 4-byte boundary
 */
#define HW3D_SHADER_NUM_TOKENS_HINT 128

int virgl_build_create_shader(uint32_t *buf, uint32_t max_dwords, uint32_t handle,
                              uint32_t shader_stage, const char *tgsi_text)
{
    if (!buf || !tgsi_text) return -1;

    uint32_t text_bytes = (uint32_t)strlen(tgsi_text) + 1; /* include NUL */
    uint32_t text_dwords = (text_bytes + 3) / 4;
    uint32_t hdr_dwords = 5; /* handle,type,offlen,num_tokens,so_num_outputs */
    uint32_t total = 1 + hdr_dwords + text_dwords; /* +1 for the CMD0 word */

    if (max_dwords < total) return -1;

    buf[0] = VIRGL_BUILD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER, hdr_dwords + text_dwords);
    buf[1] = handle;
    buf[2] = shader_stage;
    buf[3] = text_bytes; /* offlen, CONT bit clear: this is the total length */
    buf[4] = HW3D_SHADER_NUM_TOKENS_HINT;
    buf[5] = 0; /* so_num_outputs */

    uint8_t *dst = (uint8_t *)&buf[6];
    memcpy(dst, tgsi_text, text_bytes);
    /* Zero-pad the final partial dword so no stack/heap garbage leaks into
     * the command stream. */
    uint32_t pad = text_dwords * 4 - text_bytes;
    if (pad) memset(dst + text_bytes, 0, pad);

    return (int)total;
}

int virgl_build_bind_shader(uint32_t *buf, uint32_t max_dwords, uint32_t handle, uint32_t shader_stage)
{
    if (!buf || max_dwords < 3) return -1;
    buf[0] = VIRGL_BUILD_HDR(VIRGL_CCMD_BIND_SHADER, 0, 2);
    buf[1] = handle;
    buf[2] = shader_stage;
    return 3;
}

int virgl_build_bind_object(uint32_t *buf, uint32_t max_dwords, uint32_t handle, uint32_t obj_type)
{
    if (!buf || max_dwords < 2) return -1;
    buf[0] = VIRGL_BUILD_HDR(VIRGL_CCMD_BIND_OBJECT, obj_type, 1);
    buf[1] = handle;
    return 2;
}

/* VIRGL_OBJECT_BLEND. VIRGL_OBJ_BLEND_SIZE = MAX_COLOR_BUFS(8) + 3 = 11
 * payload words, always the full 8-cbuf-slot shape regardless of how many
 * color buffers are actually bound. Blending itself stays off (word S2(0)
 * bit0 = RT_BLEND_ENABLE = 0); RT_COLORMASK is set to 0xF (RGBA) on slot 0
 * so the fragment shader's output is actually written to the framebuffer —
 * leaving the colormask at its zero default would silently discard every
 * pixel the draw call produces. */
int virgl_build_create_blend_disabled(uint32_t *buf, uint32_t max_dwords, uint32_t handle)
{
    const uint32_t payload = 11;
    if (!buf || max_dwords < 1 + payload) return -1;

    buf[0] = VIRGL_BUILD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_BLEND, payload);
    buf[1] = handle;
    buf[2] = 0; /* S0: independent_blend_enable=0, logicop_enable=0, ... */
    buf[3] = 0; /* S1: logicop_func */
    buf[4] = (0xFu << 27); /* S2(cbuf0): RT_COLORMASK=0xF, blend disabled */
    for (uint32_t cbuf = 1; cbuf < 8; cbuf++) buf[4 + cbuf] = 0; /* S2(cbuf1..7), unused */
    return (int)(1 + payload);
}

/* VIRGL_OBJECT_RASTERIZER. VIRGL_OBJ_RS_SIZE = 9 payload words. Solid fill
 * (PIPE_POLYGON_MODE_FILL = 0 for both front/back), no face culling
 * (PIPE_FACE_NONE = 0 — a hand-authored triangle's winding order relative
 * to this host's clip-space convention is not something worth getting
 * wrong for a first triangle), depth clipping enabled. */
int virgl_build_create_rasterizer_default(uint32_t *buf, uint32_t max_dwords, uint32_t handle)
{
    const uint32_t payload = 9;
    if (!buf || max_dwords < 1 + payload) return -1;

    uint32_t s0 = (1u << 1); /* DEPTH_CLIP = 1; CULL_FACE=0 (NONE), FILL_FRONT/BACK=0 (FILL) */

    buf[0] = VIRGL_BUILD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_RASTERIZER, payload);
    buf[1] = handle;
    buf[2] = s0;                       /* S0 */
    *(float *)&buf[3] = 1.0f;          /* point_size */
    buf[4] = 0;                        /* sprite_coord_enable */
    buf[5] = 0;                        /* S3: line stipple / clip planes */
    *(float *)&buf[6] = 1.0f;          /* line_width */
    *(float *)&buf[7] = 0.0f;          /* offset_units */
    *(float *)&buf[8] = 0.0f;          /* offset_scale */
    *(float *)&buf[9] = 0.0f;          /* offset_clamp */
    return (int)(1 + payload);
}

/* VIRGL_OBJECT_DSA. VIRGL_OBJ_DSA_SIZE = 5 payload words. Depth test,
 * stencil test and alpha test all disabled (every field 0) — a fragment is
 * written unconditionally, which is exactly right for a single opaque
 * triangle with no depth buffer bound. */
int virgl_build_create_dsa_disabled(uint32_t *buf, uint32_t max_dwords, uint32_t handle)
{
    const uint32_t payload = 5;
    if (!buf || max_dwords < 1 + payload) return -1;

    buf[0] = VIRGL_BUILD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_DSA, payload);
    buf[1] = handle;
    buf[2] = 0; /* S0: depth test off */
    buf[3] = 0; /* S1: stencil off */
    buf[4] = 0; /* S2: back-face stencil off */
    *(float *)&buf[5] = 0.0f; /* alpha_ref */
    return (int)(1 + payload);
}

/* VIRGL_OBJECT_SURFACE. VIRGL_OBJ_SURFACE_SIZE = 5 payload words: handle,
 * res_handle, format, texture_level, texture_layers. level=0, layers=0
 * (first/only layer) is correct for a flat, non-mipmapped, non-array 2D
 * render target — the only kind this file creates. */
int virgl_build_create_surface(uint32_t *buf, uint32_t max_dwords, uint32_t handle,
                               uint32_t res_handle, uint32_t format)
{
    const uint32_t payload = 5;
    if (!buf || max_dwords < 1 + payload) return -1;

    buf[0] = VIRGL_BUILD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, payload);
    buf[1] = handle;
    buf[2] = res_handle;
    buf[3] = format;
    buf[4] = 0; /* texture_level */
    buf[5] = 0; /* texture_layers */
    return (int)(1 + payload);
}

/* VIRGL_OBJECT_VERTEX_ELEMENTS. VIRGL_OBJ_VERTEX_ELEMENTS_SIZE(n) = n*4 + 1.
 * Per element: src_offset, instance_divisor, vertex_buffer_index, src_format.
 * Every element here reads from vertex buffer slot 0 (interleaved
 * attributes in one buffer) with instance_divisor 0 (per-vertex, not
 * per-instance). */
int virgl_build_create_vertex_elements(uint32_t *buf, uint32_t max_dwords, uint32_t handle,
                                       const uint32_t *src_offsets, const uint32_t *formats,
                                       uint32_t num_elements)
{
    if (!buf || !src_offsets || !formats || num_elements == 0) return -1;
    uint32_t payload = num_elements * 4 + 1;
    if (max_dwords < 1 + payload) return -1;

    buf[0] = VIRGL_BUILD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, payload);
    buf[1] = handle;
    for (uint32_t i = 0; i < num_elements; i++) {
        uint32_t *e = &buf[2 + i * 4];
        e[0] = src_offsets[i];
        e[1] = 0;              /* instance_divisor */
        e[2] = 0;              /* vertex_buffer_index (slot 0) */
        e[3] = formats[i];
    }
    return (int)(1 + payload);
}

/* VIRGL_CCMD_SET_VERTEX_BUFFERS. One buffer at slot 0. */
int virgl_build_set_vertex_buffers(uint32_t *buf, uint32_t max_dwords,
                                   uint32_t stride, uint32_t offset, uint32_t res_handle)
{
    const uint32_t payload = 3;
    if (!buf || max_dwords < 1 + payload) return -1;

    buf[0] = VIRGL_BUILD_HDR(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, payload);
    buf[1] = stride;
    buf[2] = offset;
    buf[3] = res_handle;
    return (int)(1 + payload);
}

/* ── Fixed minimal shaders (TGSI text) ───────────────────────────────────────
 *
 * Virgl's CREATE_OBJECT/SHADER command carries TGSI *text* assembly, not a
 * hand-encoded binary token stream (see the file header comment and
 * virgl_build_create_shader() above for how this was confirmed against
 * real Mesa/virglrenderer source). This is the one part of the whole
 * pipeline that turned out to be much simpler than expected — these two
 * shaders are genuinely just text.
 *
 * Vertex shader: passes clip-space position through unchanged (the guest
 * supplies already-transformed vertices — no MVP multiply here, matching
 * how the software rasterizer this is meant to sit alongside also expects
 * pre-transformed input) and passes the per-vertex color through as a
 * varying.
 *
 * Fragment shader: outputs the interpolated color unchanged.
 *
 * IN[0]/OUT[0] carry position; IN[1]/OUT[1] (VS) <-> IN[0] (FS) carry color
 * — TGSI links separately-compiled VS/FS stages by matching
 * (semantic name, semantic index) pairs, not by slot number, which is why
 * the FS's color input is IN[0] even though the VS's color output is OUT[1].
 */
const char *const HW3D_VS_PASSTHROUGH =
    "VERT\n"
    "DCL IN[0]\n"
    "DCL IN[1]\n"
    "DCL OUT[0], POSITION\n"
    "DCL OUT[1], COLOR\n"
    "MOV OUT[0], IN[0]\n"
    "MOV OUT[1], IN[1]\n"
    "END\n";

const char *const HW3D_FS_PASSTHROUGH =
    "FRAG\n"
    "DCL IN[0], COLOR, PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "MOV OUT[0], IN[0]\n"
    "END\n";
