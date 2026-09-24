/* ============================================================================
 * AzamiOS Game Framework — Hardware 3D (Virgl API wrapper)
 * File: userland/libgame/include/game/game_hw3d.h
 *
 * Wrapper over the VirtIO-GPU DRM ioctls to submit Virgl 3D commands from
 * userland. Requires a VirtIO-GPU device with 3D capabilities enabled.
 *
 * The command encoders below build real Virgl (virglrenderer) protocol
 * packets — verified against virglrenderer's own src/virgl_protocol.h and
 * Mesa's src/gallium/drivers/virgl/virgl_encode.c, not guessed. A few
 * points worth knowing before touching this file:
 *
 *   - VIRGL_CCMD_CREATE_OBJECT of type VIRGL_OBJECT_SHADER does NOT carry a
 *     binary TGSI token stream on the wire. Mesa's own virgl driver
 *     converts its internal TGSI tokens to TGSI *text* assembly
 *     (tgsi_dump_str()) and sends that text, NUL-terminated, as the shader
 *     "binary". virglrenderer's host side (vrend_decode_create_shader,
 *     src/vrend/vrend_decode.c) parses it right back with
 *     tgsi_text_translate(). So the shaders below are authored directly as
 *     TGSI text — there is no separate binary encoding step to get right.
 *   - The wire's shader-type dword (VIRGL_OBJ_SHADER_TYPE /
 *     VIRGL_BIND_SHADER_TYPE) is `enum virgl_shader_stage` from
 *     src/virtio/virtio-gpu/virgl_protocol.h (VERTEX=0, FRAGMENT=1, ...) —
 *     NOT Mesa's newer internal `mesa_shader_stage` (which numbers
 *     FRAGMENT=4). Mesa converts between the two at the wire boundary
 *     (virgl_shader_stage_convert()); this file targets the wire enum
 *     directly.
 *   - Resource/vertex-element formats are full Gallium `enum pipe_format`
 *     values (hundreds of entries, generated from
 *     src/util/format/u_format.yaml), not the small ~8-entry
 *     VIRTIO_GPU_FORMAT_* enum the 2D path uses. The two values used here
 *     (PIPE_FORMAT_R32G32B32A32_FLOAT=24, PIPE_FORMAT_B8G8R8A8_UNORM=105)
 *     were confirmed by actually running Mesa's real
 *     u_format_table.py/u_format.yaml generator, not recalled from memory —
 *     the naive "well known" values for a couple of these are wrong.
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Opaque context handle */
typedef struct hw3d_context hw3d_context_t;

/* Initialize the hardware 3D subsystem (opens DRM node).
 * This initializes the connection to the DRM device, but doesn't create a 3D context. */
hw3d_context_t *hw3d_init(void);

/* Check if the hardware supports 3D (queries VIRTGPU_GETPARAM) */
bool hw3d_is_supported(hw3d_context_t *ctx);

/* Create a 3D context on the host */
int hw3d_context_create(hw3d_context_t *ctx, uint32_t ctx_id);

/* Create a 3D resource (like a texture or buffer). Must be called after
 * hw3d_context_create() if the resource should be usable by that context —
 * the kernel auto-attaches every resource created on this fd to whichever
 * context this fd most recently initialized. */
int hw3d_resource_create(hw3d_context_t *ctx, uint32_t target, uint32_t format, uint32_t bind, uint32_t width, uint32_t height, uint32_t depth, uint32_t array_size, uint32_t *out_handle);

/*
 * Submit a raw Virgl command buffer to the hardware context.
 * `cmd_buf` should contain Virgl opcode and arguments.
 */
int hw3d_submit_cmd(hw3d_context_t *ctx, uint32_t ctx_id, void *cmd_buf, uint32_t size);

/* hw3d_transfer_to_host(ctx, res_handle, data, size) — upload @size bytes
 * from @data into resource @res_handle's guest backing pages, then tell the
 * host to copy them in (DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST ->
 * TRANSFER_TO_HOST_3D). This is how vertex/index/uniform data written by
 * the guest actually becomes visible to the host's Gallium driver — a
 * RESOURCE_CREATE_3D resource is otherwise just empty host-side storage.
 * @data must be at most the resource's backing size; the whole resource
 * (box {0,0,0, size,1,1}, level 0) is transferred. */
int hw3d_transfer_to_host(hw3d_context_t *ctx, uint32_t res_handle, const void *data, uint32_t size);

/* hw3d_transfer_from_host(ctx, res_handle, out, size) — the read-back
 * counterpart: ask the host to copy @size bytes of resource @res_handle's
 * current content back into the guest backing pages (TRANSFER_FROM_HOST_3D),
 * then copy them into @out. For a 2D texture resource, @size should be
 * width*height*bytes_per_pixel and the transfer covers the whole level-0
 * image (box {0,0,0, size,1,1} is wrong for a texture — see
 * hw3d_transfer_from_host_2d_box() for a real 2D box transfer). */
int hw3d_transfer_from_host(hw3d_context_t *ctx, uint32_t res_handle, void *out, uint32_t size);

/* hw3d_transfer_to_host_2d_box / hw3d_transfer_from_host_2d_box — like the
 * above but with an explicit 2D box (x,y,w,h) and row stride, for texture
 * (PIPE_TEXTURE_2D) resources rather than flat PIPE_BUFFER ones. */
int hw3d_transfer_to_host_2d_box(hw3d_context_t *ctx, uint32_t res_handle,
                                 const void *data, uint32_t w, uint32_t h, uint32_t stride);
int hw3d_transfer_from_host_2d_box(hw3d_context_t *ctx, uint32_t res_handle,
                                   void *out, uint32_t w, uint32_t h, uint32_t stride);

/* Shutdown the 3D subsystem */
void hw3d_shutdown(hw3d_context_t *ctx);

/*
 * Virgl 3D Command Stream Builders (Encodes Virgl CCMD packets)
 * Returns the number of uint32_t DWORDs written to buf, or negative on error.
 */
int virgl_build_clear(uint32_t *buf, uint32_t max_dwords, uint32_t buffers, float r, float g, float b, float a, double depth, uint32_t stencil);
int virgl_build_set_viewport(uint32_t *buf, uint32_t max_dwords, float scale_x, float scale_y, float scale_z, float trans_x, float trans_y, float trans_z);
int virgl_build_set_framebuffer(uint32_t *buf, uint32_t max_dwords, uint32_t nr_cbufs, uint32_t surf_handle, uint32_t zsurf_handle);
int virgl_build_draw_vbo(uint32_t *buf, uint32_t max_dwords, uint32_t mode, uint32_t start, uint32_t count);

/* ── Gallium/pipe enum values actually needed by this file ──────────────────
 * Confirmed against real Mesa/virglrenderer source (see file header comment)
 * rather than recalled from memory. */
#define PIPE_BUFFER                       0   /* pipe_texture_target: flat buffer */
#define PIPE_TEXTURE_2D                   2   /* pipe_texture_target: 2D texture */

#define PIPE_FORMAT_R32G32B32A32_FLOAT    24  /* vertex attribute format (vec4) */
#define PIPE_FORMAT_B8G8R8A8_UNORM        105 /* render target / framebuffer format */

#define VIRGL_BIND_RENDER_TARGET   (1u << 1)
#define VIRGL_BIND_VERTEX_BUFFER  (1u << 4)

/* enum virgl_shader_stage (wire value — NOT mesa_shader_stage) */
#define VIRGL_SHADER_VERTEX    0
#define VIRGL_SHADER_FRAGMENT  1

/* enum virgl_object_type */
#define VIRGL_OBJECT_BLEND            1
#define VIRGL_OBJECT_RASTERIZER       2
#define VIRGL_OBJECT_DSA              3
#define VIRGL_OBJECT_SHADER           4
#define VIRGL_OBJECT_VERTEX_ELEMENTS  5
#define VIRGL_OBJECT_SURFACE          8

/* PIPE_PRIM_TRIANGLES (pipe_prim_type) — the only primitive mode this file
 * draws with. Value is stable/ancient in Gallium (POINTS=0, LINES=1,
 * LINE_LOOP=2, LINE_STRIP=3, TRIANGLES=4, ...). */
#define PIPE_PRIM_TRIANGLES 4

/* ── Real Virgl pipeline-state object builders ──────────────────────────────
 * A draw call needs a bound shader pair, vertex-elements layout, vertex
 * buffer, framebuffer *surface* (not a raw resource handle) and — for
 * well-defined output — blend/rasterizer/depth-stencil-alpha state objects.
 * All of these were unimplemented before; game_hw3d.c had only
 * clear/viewport/framebuffer(raw handle)/draw_vbo, which cannot actually
 * produce a correct triangle on its own. */

/* virgl_build_create_shader() — VIRGL_CCMD_CREATE_OBJECT / VIRGL_OBJECT_SHADER.
 * @tgsi_text is a NUL-terminated TGSI assembly string (see game_hw3d.c's
 * HW3D_VS_PASSTHROUGH / HW3D_FS_PASSTHROUGH for real examples). Only
 * single-packet (non-continuation) shaders are supported — plenty for the
 * tiny fixed shaders this file ships. */
int virgl_build_create_shader(uint32_t *buf, uint32_t max_dwords, uint32_t handle,
                              uint32_t shader_stage, const char *tgsi_text);

/* virgl_build_bind_shader() — VIRGL_CCMD_BIND_SHADER. Makes @handle (created
 * with virgl_build_create_shader()) the active shader for @shader_stage. */
int virgl_build_bind_shader(uint32_t *buf, uint32_t max_dwords, uint32_t handle, uint32_t shader_stage);

/* virgl_build_bind_object() — VIRGL_CCMD_BIND_OBJECT, generic: binds any
 * previously CREATE_OBJECT'd object (blend/rasterizer/dsa/...) as current
 * state of its type. */
int virgl_build_bind_object(uint32_t *buf, uint32_t max_dwords, uint32_t handle, uint32_t obj_type);

/* virgl_build_create_blend_disabled() — a minimal VIRGL_OBJECT_BLEND state:
 * blending off, full RGBA color-mask on color buffer 0 (so drawn fragments
 * are actually written rather than masked out). */
int virgl_build_create_blend_disabled(uint32_t *buf, uint32_t max_dwords, uint32_t handle);

/* virgl_build_create_rasterizer_default() — a minimal VIRGL_OBJECT_RASTERIZER
 * state: solid fill, no culling, depth clipping on — the sane default for a
 * single untextured triangle regardless of winding order. */
int virgl_build_create_rasterizer_default(uint32_t *buf, uint32_t max_dwords, uint32_t handle);

/* virgl_build_create_dsa_disabled() — a minimal VIRGL_OBJECT_DSA (depth/
 * stencil/alpha) state with depth test, stencil test and alpha test all
 * off, so a fragment shader's output is written unconditionally. */
int virgl_build_create_dsa_disabled(uint32_t *buf, uint32_t max_dwords, uint32_t handle);

/* virgl_build_create_surface() — VIRGL_CCMD_CREATE_OBJECT / VIRGL_OBJECT_SURFACE.
 * A surface is a *view* of a resource (level/layer + format) — what
 * SET_FRAMEBUFFER_STATE actually needs bound as a color buffer, not the raw
 * resource handle the old virgl_build_set_framebuffer() caller used to pass. */
int virgl_build_create_surface(uint32_t *buf, uint32_t max_dwords, uint32_t handle,
                               uint32_t res_handle, uint32_t format);

/* virgl_build_create_vertex_elements() — VIRGL_CCMD_CREATE_OBJECT /
 * VIRGL_OBJECT_VERTEX_ELEMENTS. Describes up to @num_elements interleaved
 * vertex attributes read from vertex buffer slot 0. @src_offsets[i] is the
 * byte offset of attribute i within one vertex; @formats[i] is a
 * PIPE_FORMAT_* value (PIPE_FORMAT_R32G32B32A32_FLOAT for a vec4 float
 * attribute, the only case this file needs). */
int virgl_build_create_vertex_elements(uint32_t *buf, uint32_t max_dwords, uint32_t handle,
                                       const uint32_t *src_offsets, const uint32_t *formats,
                                       uint32_t num_elements);

/* virgl_build_set_vertex_buffers() — VIRGL_CCMD_SET_VERTEX_BUFFERS. Binds
 * one vertex buffer resource at slot 0 with the given per-vertex stride. */
int virgl_build_set_vertex_buffers(uint32_t *buf, uint32_t max_dwords,
                                   uint32_t stride, uint32_t offset, uint32_t res_handle);

/* ── Fixed minimal shaders (TGSI text) ──────────────────────────────────────
 * See game_hw3d.c for the actual strings and the reasoning behind them:
 * a passthrough vertex shader (clip-space position + interpolated color)
 * and a passthrough fragment shader (writes the interpolated color).
 * Exposed here so callers (and the 3d_test app) can create/bind them
 * without duplicating the TGSI text. */
extern const char *const HW3D_VS_PASSTHROUGH;
extern const char *const HW3D_FS_PASSTHROUGH;
