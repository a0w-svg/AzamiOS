/* ============================================================================
 * AzamiOS — 3d_test: end-to-end VirtIO-GPU/Virgl hardware triangle test
 *
 * Drives the complete real 3D pipeline: context creation, real capset
 * query, TGSI shader creation, vertex-buffer upload via a real 3D transfer,
 * pipeline state object binding, a draw call, and a real read-back of the
 * rendered pixels so this program can print concrete evidence of what the
 * host GPU actually drew — not just "the ioctl returned 0".
 * ============================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <game/game_hw3d.h>

#define RT_W 64
#define RT_H 64
#define RT_STRIDE (RT_W * 4)

/* Interleaved vertex: position (vec4) + color (vec4) = 32 bytes/vertex. */
typedef struct {
    float pos[4];
    float col[4];
} hw3d_vertex_t;

static void dump_pixel(const uint8_t *fb, uint32_t x, uint32_t y, const char *label)
{
    const uint8_t *p = fb + (size_t)y * RT_STRIDE + (size_t)x * 4;
    /* PIPE_FORMAT_B8G8R8A8_UNORM: bytes are B,G,R,A in memory order. */
    printf("  %-18s (%2u,%2u): B=%3u G=%3u R=%3u A=%3u\n",
           label, x, y, p[0], p[1], p[2], p[3]);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("=== AzamiOS VirtIO-GPU/Virgl hardware triangle test ===\n");
    printf("Initializing Hardware 3D context...\n");

    hw3d_context_t *ctx = hw3d_init();
    if (!ctx) {
        printf("Failed to open /dev/dri/card0.\n");
        return 1;
    }

    if (!hw3d_is_supported(ctx)) {
        printf("FAILURE: 3D hardware acceleration is NOT supported or negotiated.\n");
        printf("(This is the expected, correct result when QEMU wasn't started with\n"
               " -device virtio-gpu-gl-pci — the software rasterizer path is what\n"
               " every app should keep using in that case.)\n");
        hw3d_shutdown(ctx);
        return 1;
    }

    printf("SUCCESS: VirtIO-GPU Virgl 3D is SUPPORTED on this system.\n");

    uint32_t ctx_id = 42;
    int ret = hw3d_context_create(ctx, ctx_id);
    printf("hw3d_context_create -> %d\n", ret);
    if (ret != 0) { hw3d_shutdown(ctx); return 1; }

    /* ── Shaders ─────────────────────────────────────────────────────── */
    uint32_t cmd[512];
    uint32_t n = 0;

    const uint32_t VS_HANDLE = 1, FS_HANDLE = 2;
    n = 0;
    int w = virgl_build_create_shader(cmd, 512, VS_HANDLE, VIRGL_SHADER_VERTEX, HW3D_VS_PASSTHROUGH);
    if (w < 0) { printf("VS encode failed\n"); return 1; }
    n += w;
    w = virgl_build_create_shader(cmd + n, 512 - n, FS_HANDLE, VIRGL_SHADER_FRAGMENT, HW3D_FS_PASSTHROUGH);
    if (w < 0) { printf("FS encode failed\n"); return 1; }
    n += w;
    w = virgl_build_bind_shader(cmd + n, 512 - n, VS_HANDLE, VIRGL_SHADER_VERTEX);
    n += w;
    w = virgl_build_bind_shader(cmd + n, 512 - n, FS_HANDLE, VIRGL_SHADER_FRAGMENT);
    n += w;
    ret = hw3d_submit_cmd(ctx, ctx_id, cmd, n * sizeof(uint32_t));
    printf("submit(create+bind shaders, %u dwords) -> %d\n", n, ret);

    /* ── Pipeline state objects ─────────────────────────────────────── */
    const uint32_t BLEND_HANDLE = 3, RAST_HANDLE = 4, DSA_HANDLE = 5;
    n = 0;
    n += virgl_build_create_blend_disabled(cmd + n, 512 - n, BLEND_HANDLE);
    n += virgl_build_create_rasterizer_default(cmd + n, 512 - n, RAST_HANDLE);
    n += virgl_build_create_dsa_disabled(cmd + n, 512 - n, DSA_HANDLE);
    n += virgl_build_bind_object(cmd + n, 512 - n, BLEND_HANDLE, VIRGL_OBJECT_BLEND);
    n += virgl_build_bind_object(cmd + n, 512 - n, RAST_HANDLE, VIRGL_OBJECT_RASTERIZER);
    n += virgl_build_bind_object(cmd + n, 512 - n, DSA_HANDLE, VIRGL_OBJECT_DSA);
    ret = hw3d_submit_cmd(ctx, ctx_id, cmd, n * sizeof(uint32_t));
    printf("submit(create+bind blend/rast/dsa, %u dwords) -> %d\n", n, ret);

    /* ── Vertex elements (position + color, both vec4 float) ───────── */
    const uint32_t VE_HANDLE = 6;
    /* hw3d_vertex_t is { float pos[4]; float col[4]; } with no padding
     * between the two vec4 members, so these offsets are fixed and simple
     * enough to just state directly rather than pull in <stddef.h> for
     * offsetof() (not available in this libc's default include set). */
    uint32_t offsets[2] = { 0, 16 };
    uint32_t formats[2] = { PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R32G32B32A32_FLOAT };
    n = 0;
    n += virgl_build_create_vertex_elements(cmd + n, 512 - n, VE_HANDLE, offsets, formats, 2);
    n += virgl_build_bind_object(cmd + n, 512 - n, VE_HANDLE, VIRGL_OBJECT_VERTEX_ELEMENTS);
    ret = hw3d_submit_cmd(ctx, ctx_id, cmd, n * sizeof(uint32_t));
    printf("submit(vertex elements, %u dwords) -> %d\n", n, ret);

    /* ── Vertex buffer: one real triangle, uploaded via a real 3D transfer ── */
    uint32_t vb_handle = 0;
    ret = hw3d_resource_create(ctx, PIPE_BUFFER, PIPE_FORMAT_R32G32B32A32_FLOAT,
                               VIRGL_BIND_VERTEX_BUFFER, sizeof(hw3d_vertex_t) * 3, 1, 1, 1,
                               &vb_handle);
    printf("hw3d_resource_create(vertex buffer) -> %d (handle %u)\n", ret, vb_handle);

    hw3d_vertex_t verts[3] = {
        { {  0.0f,  0.6f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } }, /* top, red    */
        { { -0.6f, -0.6f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } }, /* left, green */
        { {  0.6f, -0.6f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } }, /* right, blue */
    };
    ret = hw3d_transfer_to_host(ctx, vb_handle, verts, sizeof(verts));
    printf("hw3d_transfer_to_host(vertex data, %zu bytes) -> %d\n", sizeof(verts), ret);

    n = 0;
    n += virgl_build_set_vertex_buffers(cmd + n, 512 - n, sizeof(hw3d_vertex_t), 0, vb_handle);
    ret = hw3d_submit_cmd(ctx, ctx_id, cmd, n * sizeof(uint32_t));
    printf("submit(set vertex buffers, %u dwords) -> %d\n", n, ret);

    /* ── Render target: a real 2D texture resource + a surface view of it ── */
    uint32_t rt_handle = 0;
    ret = hw3d_resource_create(ctx, PIPE_TEXTURE_2D, PIPE_FORMAT_B8G8R8A8_UNORM,
                               VIRGL_BIND_RENDER_TARGET, RT_W, RT_H, 1, 1, &rt_handle);
    printf("hw3d_resource_create(render target %ux%u) -> %d (handle %u)\n", RT_W, RT_H, ret, rt_handle);

    const uint32_t SURF_HANDLE = 7;
    n = 0;
    n += virgl_build_create_surface(cmd + n, 512 - n, SURF_HANDLE, rt_handle, PIPE_FORMAT_B8G8R8A8_UNORM);
    ret = hw3d_submit_cmd(ctx, ctx_id, cmd, n * sizeof(uint32_t));
    printf("submit(create surface, %u dwords) -> %d\n", n, ret);

    /* ── Bind framebuffer + viewport, clear, draw ───────────────────── */
    n = 0;
    n += virgl_build_set_framebuffer(cmd + n, 512 - n, 1, SURF_HANDLE, 0);
    n += virgl_build_set_viewport(cmd + n, 512 - n,
                                  (float)RT_W / 2.0f, (float)RT_H / 2.0f, 0.5f,
                                  (float)RT_W / 2.0f, (float)RT_H / 2.0f, 0.5f);
    n += virgl_build_clear(cmd + n, 512 - n, 0x1 /* COLOR */, 0.1f, 0.1f, 0.1f, 1.0f, 1.0, 0);
    n += virgl_build_draw_vbo(cmd + n, 512 - n, PIPE_PRIM_TRIANGLES, 0, 3);
    ret = hw3d_submit_cmd(ctx, ctx_id, cmd, n * sizeof(uint32_t));
    printf("submit(framebuffer+viewport+clear+draw, %u dwords) -> %d\n", n, ret);

    /* ── Read back the rendered pixels: real evidence, not just a return code ── */
    static uint8_t framebuf[RT_STRIDE * RT_H];
    memset(framebuf, 0xAA, sizeof(framebuf)); /* poison: prove the transfer wrote real data */
    ret = hw3d_transfer_from_host_2d_box(ctx, rt_handle, framebuf, RT_W, RT_H, RT_STRIDE);
    printf("hw3d_transfer_from_host_2d_box(readback) -> %d\n", ret);

    if (ret == 0) {
        printf("Rendered pixel samples (BGRA):\n");
        dump_pixel(framebuf, 2, 2, "corner (clear)");
        dump_pixel(framebuf, RT_W / 2, RT_H / 2, "center (triangle)");
        dump_pixel(framebuf, RT_W / 2, RT_H - 4, "bottom-mid (triangle)");

        /* Simple pass/fail: the corner should be the clear color (dark
         * gray), the center should NOT be the clear color (the triangle
         * covers it) if the draw actually executed and produced fragments. */
        const uint8_t *corner = framebuf + 2 * RT_STRIDE + 2 * 4;
        const uint8_t *center = framebuf + (RT_H / 2) * RT_STRIDE + (RT_W / 2) * 4;
        bool corner_is_clear = (corner[0] > 15 && corner[0] < 40) && (corner[3] == 255 || corner[3] == 0);
        bool center_differs = (center[0] != corner[0] || center[1] != corner[1] || center[2] != corner[2]);
        printf("corner looks like clear color: %s\n", corner_is_clear ? "yes" : "no (unexpected)");
        printf("center differs from corner (triangle drawn): %s\n", center_differs ? "YES" : "NO");
        if (center_differs) {
            printf("=== RESULT: real hardware-rendered pixels observed. ===\n");
        } else {
            printf("=== RESULT: no visible difference — draw may not have produced fragments. ===\n");
        }
    }

    hw3d_shutdown(ctx);
    return 0;
}
