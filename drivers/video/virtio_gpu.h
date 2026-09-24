/* ============================================================================
 * AzamiOS — VirtIO-GPU Driver Header
 * File: drivers/video/virtio_gpu.h
 *
 * Implements the VirtIO-GPU control protocol and driver structures.
 * ============================================================================ */
#pragma once

#include "../../hal/virtio_pci.h"

#define VIRTIO_GPU_F_VIRGL 0 /* Host offers 3D (Virgl) contexts and commands */
#define VIRTIO_GPU_F_EDID   1 /* Device can report per-scanout EDID blobs */

/* VIRTIO_GPU Control Commands */
enum virtio_gpu_ctrl_type {
    /* 2D commands */
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,
    VIRTIO_GPU_CMD_RESOURCE_UNREF,
    VIRTIO_GPU_CMD_SET_SCANOUT,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING,
    VIRTIO_GPU_CMD_GET_CAPSET_INFO = 0x0108,
    VIRTIO_GPU_CMD_GET_CAPSET = 0x0109,
    VIRTIO_GPU_CMD_GET_EDID = 0x010a,

    /* 3D commands */
    VIRTIO_GPU_CMD_CTX_CREATE = 0x0200,
    VIRTIO_GPU_CMD_CTX_DESTROY = 0x0201,
    VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE = 0x0202,
    VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE = 0x0203,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_3D = 0x0204,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D = 0x0205,
    VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D = 0x0206,
    VIRTIO_GPU_CMD_SUBMIT_3D = 0x0207,

    /* cursor commands (submitted on the cursor queue, index 1) */
    VIRTIO_GPU_CMD_UPDATE_CURSOR = 0x0300,
    VIRTIO_GPU_CMD_MOVE_CURSOR,

    /* success responses */
    VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO,
    VIRTIO_GPU_RESP_OK_CAPSET_INFO = 0x1102,
    VIRTIO_GPU_RESP_OK_CAPSET = 0x1103,
    VIRTIO_GPU_RESP_OK_EDID = 0x1104,

    /* error responses */
    VIRTIO_GPU_RESP_ERR_UNSPEC = 0x1200,
    VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY,
    VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER,
};

/* Virgl capset ids (VIRTIO_GPU_CMD_GET_CAPSET_INFO / GET_CAPSET). Capset 1
 * ("virgl") is what every OpenGL-era virglrenderer host advertises; capset 2
 * ("virgl2") adds a handful of newer-GL-version fields on top of the same
 * base layout. Matches include/uapi/linux/virtio_gpu.h. */
#define VIRTIO_GPU_CAPSET_VIRGL  1
#define VIRTIO_GPU_CAPSET_VIRGL2 2

/* VIRTIO_GPU 2D Formats */
enum virtio_gpu_formats {
    VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM  = 1,
    VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM  = 2,
    VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM  = 3,
    VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM  = 4,
    VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM  = 67,
    VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM  = 68,
    VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM  = 121,
    VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM  = 134,
};

#define VIRTIO_GPU_FLAG_FENCE (1 << 0)

/* Base header for all commands and responses */
struct virtio_gpu_ctrl_hdr {
    u32 type;
    u32 flags;
    u64 fence_id;
    u32 ctx_id;
    u32 padding;
} __attribute__((packed));

/* GET_DISPLAY_INFO */
#define VIRTIO_GPU_MAX_SCANOUTS 16

struct virtio_gpu_rect {
    u32 x;
    u32 y;
    u32 width;
    u32 height;
} __attribute__((packed));

struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    u32 enabled;
    u32 flags;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed));

/* GET_EDID (VIRTIO_GPU_F_EDID). A monitor's EDID is a 128-byte block (plus
 * optional 128-byte extensions this driver does not need); the device hands
 * back up to 1024 bytes so a multi-extension EDID is not truncated. */
#define VIRTIO_GPU_EDID_MAX_SIZE 1024

struct virtio_gpu_get_edid {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 scanout_id;
    u32 padding;
} __attribute__((packed));

struct virtio_gpu_resp_edid {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 size;
    u32 padding;
    u8  edid[VIRTIO_GPU_EDID_MAX_SIZE];
} __attribute__((packed));

/* RESOURCE_CREATE_2D */
struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 resource_id;
    u32 format;
    u32 width;
    u32 height;
} __attribute__((packed));

/* RESOURCE_CREATE_3D */
struct virtio_gpu_resource_create_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 resource_id;
    u32 target;
    u32 format;
    u32 bind;
    u32 width;
    u32 height;
    u32 depth;
    u32 array_size;
    u32 last_level;
    u32 nr_samples;
    u32 flags;
    u32 padding;
} __attribute__((packed));

/* RESOURCE_ATTACH_BACKING */
struct virtio_gpu_mem_entry {
    u64 addr;
    u32 length;
    u32 padding;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 resource_id;
    u32 nr_entries;
} __attribute__((packed));

/* SET_SCANOUT */
struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    u32 scanout_id;
    u32 resource_id;
} __attribute__((packed));

/* TRANSFER_TO_HOST_2D */
struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    u64 offset;
    u32 resource_id;
    u32 padding;
} __attribute__((packed));

/* RESOURCE_FLUSH */
struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    u32 resource_id;
    u32 padding;
} __attribute__((packed));

/* RESOURCE_UNREF — destroy a host resource.
 * The driver must RESOURCE_DETACH_BACKING first (or the resource was never
 * attached), then send this to free the host's copy.  Pairs with every
 * RESOURCE_CREATE_2D call made at bring-up or by the ioctl path. */
struct virtio_gpu_resource_unref {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 resource_id;
    u32 padding;
} __attribute__((packed));

/* RESOURCE_DETACH_BACKING — unregister guest backing pages for a resource.
 * Must be sent before RESOURCE_UNREF when the resource had attached backing. */
struct virtio_gpu_resource_detach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 resource_id;
    u32 padding;
} __attribute__((packed));

/* CTX_CREATE */
struct virtio_gpu_ctx_create {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 nlen;
    u32 padding;
    char debug_name[64];
} __attribute__((packed));

/* CTX_DESTROY */
struct virtio_gpu_ctx_destroy {
    struct virtio_gpu_ctrl_hdr hdr;
} __attribute__((packed));

/* CTX_ATTACH_RESOURCE / CTX_DETACH_RESOURCE */
struct virtio_gpu_ctx_resource {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 resource_id;
    u32 padding;
} __attribute__((packed));

/* SUBMIT_3D */
struct virtio_gpu_cmd_submit {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 size;
    u32 padding;
} __attribute__((packed));

/* GET_CAPSET_INFO */
struct virtio_gpu_get_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 capset_index;
    u32 padding;
} __attribute__((packed));

struct virtio_gpu_resp_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 capset_id;
    u32 capset_max_version;
    u32 capset_max_size;
    u32 padding;
} __attribute__((packed));

/* GET_CAPSET — request header; the response is a bare ctrl_hdr immediately
 * followed by capset_max_size bytes of opaque capability data (a struct
 * virgl_caps_v1/v2 on the host side, which this driver never has to parse —
 * it only has to shuttle the bytes to userspace so Mesa can). */
struct virtio_gpu_get_capset {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 capset_id;
    u32 capset_version;
} __attribute__((packed));

/* TRANSFER_TO_HOST_3D / TRANSFER_FROM_HOST_3D */
struct virtio_gpu_box {
    u32 x, y, z;
    u32 w, h, d;
} __attribute__((packed));

struct virtio_gpu_transfer_host_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_box box;
    u64 offset;
    u32 resource_id;
    u32 level;
    u32 stride;
    u32 layer_stride;
} __attribute__((packed));

/* UPDATE_CURSOR / MOVE_CURSOR (cursor queue). Both use this one struct; a
 * MOVE only reads .pos, an UPDATE also installs .resource_id as the cursor
 * image and applies the hotspot. resource_id 0 on an UPDATE hides the cursor. */
struct virtio_gpu_cursor_pos {
    u32 scanout_id;
    u32 x;
    u32 y;
    u32 padding;
} __attribute__((packed));

struct virtio_gpu_update_cursor {
    struct virtio_gpu_ctrl_hdr   hdr;
    struct virtio_gpu_cursor_pos pos;
    u32 resource_id;
    u32 hot_x;
    u32 hot_y;
    u32 padding;
} __attribute__((packed));

/* The virtio-gpu hardware cursor is a fixed 64x64 BGRA image. */
#define VIRTIO_GPU_CURSOR_W 64
#define VIRTIO_GPU_CURSOR_H 64

/* Driver state */
typedef struct virtio_gpu_state {
    virtio_pci_device_t vpci;
    virtqueue_t *controlq;
    virtqueue_t *cursorq;
    
    u32 screen_width;
    u32 screen_height;
    u32 resource_id;
    
    void *framebuffer_virt;
    phys_addr_t framebuffer_phys;
    u32 framebuffer_size;

    /* Hardware cursor: a private 64x64 BGRA resource, created lazily. */
    u32         cursor_res_id;
    void       *cursor_virt;
    phys_addr_t cursor_phys;
} virtio_gpu_state_t;

/**
 * virtio_gpu_init(hal_dev) - Bring up the VirtIO-GPU transport (queues,
 * feature negotiation) against one already-matched PCI device. Not a
 * self-registering driver: drivers/gpu/drm/virtgpu_drm.c owns the PCI id
 * match for this device (1AF4:1050/1010) and calls this from its own probe()
 * the first time it runs, so this only ever executes when that hardware is
 * actually present. Returns 0 on success, or -EBUSY if the transport is
 * already up.
 */
int virtio_gpu_init(device_t *hal_dev);

/**
 * virtio_gpu_send_command - Submit on the control queue and wait for response.
 * virtio_gpu_send_cursor  - Submit on the cursor queue and wait for response.
 */
int virtio_gpu_send_command(virtio_gpu_state_t *gpu, void *cmd, u32 cmd_size, void *resp, u32 resp_size);
int virtio_gpu_send_cursor(virtio_gpu_state_t *gpu, void *cmd, u32 cmd_size, void *resp, u32 resp_size);

/* ── 2D command helpers (drivers/video/virtio_gpu_cmd.c) ─────────────────── */

int virtio_gpu_get_display_info(u32 *width, u32 *height);
int virtio_gpu_resource_create_2d(u32 resource_id, u32 format, u32 width, u32 height);
int virtio_gpu_resource_attach_backing(u32 resource_id, phys_addr_t ptr, u32 length);
int virtio_gpu_set_scanout(u32 scanout_id, u32 resource_id, u32 width, u32 height);
int virtio_gpu_set_scanout_offset(u32 scanout_id, u32 resource_id, u32 x, u32 y, u32 width, u32 height);
int virtio_gpu_setup_framebuffer(void);

/* ── 3D (Virgl) command helpers ──────────────────────────────────────────── */

int virtio_gpu_cmd_context_create(u32 ctx_id, const char *name);
int virtio_gpu_cmd_context_destroy(u32 ctx_id);
int virtio_gpu_cmd_context_attach_resource(u32 ctx_id, u32 res_id);
int virtio_gpu_cmd_resource_create_3d(u32 res_id, u32 target, u32 format, u32 bind, u32 width, u32 height, u32 depth, u32 array_size);
int virtio_gpu_cmd_submit_3d(u32 ctx_id, void *buf, u32 size);

/**
 * virtio_gpu_resource_attach_backing_pages() — like
 * virtio_gpu_resource_attach_backing(), but for a resource backed by
 * @npages individually allocated (not necessarily physically contiguous)
 * pages, e.g. a GEM object's obj->pages[] array. Sends one
 * RESOURCE_ATTACH_BACKING mem-entry per page — the command already supports
 * an arbitrary scatter-gather list, the 2D path just never needed more than
 * one entry because its framebuffer is one pmm_alloc_pages() allocation.
 */
int virtio_gpu_resource_attach_backing_pages(u32 resource_id, const phys_addr_t *pages,
                                             u32 npages, u32 page_size);

/**
 * virtio_gpu_cmd_get_capset_info(capset_index, ...) — VIRTIO_GPU_CMD_GET_CAPSET_INFO.
 * Queries the @capset_index'th capset the host offers (0-based, unrelated to
 * the capset's own id). Out params receive the capset's id (e.g.
 * VIRTIO_GPU_CAPSET_VIRGL), its highest supported version, and the byte size
 * of its capability data at that version. A host with fewer than
 * @capset_index+1 capsets responds with capset_id 0.
 */
int virtio_gpu_cmd_get_capset_info(u32 capset_index, u32 *out_id,
                                   u32 *out_max_version, u32 *out_max_size);

/**
 * virtio_gpu_cmd_get_capset(capset_id, capset_version, out_buf, buf_size, out_size)
 * — VIRTIO_GPU_CMD_GET_CAPSET. Fetches up to @buf_size bytes of the given
 * capset's capability data (a host-defined struct, e.g. virgl_caps_v2) into
 * @out_buf. @out_size receives the number of bytes actually written.
 */
int virtio_gpu_cmd_get_capset(u32 capset_id, u32 capset_version,
                              void *out_buf, u32 buf_size, u32 *out_size);

/**
 * virtio_gpu_cmd_transfer_to_host_3d() — VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D.
 * Copies @offset..@offset+extent bytes of @resource_id's attached guest
 * backing pages into the host's copy of the resource, within the 3D box
 * (x,y,z,w,h,d) at mip @level. For a linear buffer resource (vertex/index/
 * uniform data, VIRGL_BIND_VERTEX_BUFFER and friends) the box is simply
 * {0,0,0, size,1,1} and @stride/@layer_stride are 0 — this is what makes
 * data written into a RESOURCE_CREATE_3D buffer by the guest actually
 * visible to the host's Gallium driver; without it the resource exists on
 * the host but is never filled in.
 */
int virtio_gpu_cmd_transfer_to_host_3d(u32 ctx_id, u32 resource_id,
                                       u32 x, u32 y, u32 z, u32 w, u32 h, u32 d,
                                       u64 offset, u32 level, u32 stride,
                                       u32 layer_stride);

/**
 * virtio_gpu_cmd_transfer_from_host_3d() — VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D.
 * The read-back counterpart of virtio_gpu_cmd_transfer_to_host_3d(): copies
 * the host's copy of @resource_id within the given box back into the
 * resource's attached guest backing pages. What makes a rendered frame (or
 * any other host-side result — a query, a computed buffer) actually
 * inspectable from the guest instead of only existing on the host's GPU.
 */
int virtio_gpu_cmd_transfer_from_host_3d(u32 ctx_id, u32 resource_id,
                                         u32 x, u32 y, u32 z, u32 w, u32 h, u32 d,
                                         u64 offset, u32 level, u32 stride,
                                         u32 layer_stride);

/**
 * virtio_gpu_get_display_info_all(out_modes, max, out_count) — like
 * virtio_gpu_get_display_info(), but returns every scanout the host reports
 * as enabled (up to @max, capped at VIRTIO_GPU_MAX_SCANOUTS), not just
 * scanout 0. @out_count receives how many entries of @out_modes were filled
 * in; out_modes[i].r gives that scanout's geometry, indexed by real scanout
 * id (a gap — id 0 and 2 enabled, 1 not — leaves that slot's .enabled at 0
 * rather than compacting the array, so the array index is always the
 * scanout id to pass to virtio_gpu_set_scanout() and friends).
 */
int virtio_gpu_get_display_info_all(struct virtio_gpu_display_one *out_modes,
                                    u32 max, u32 *out_count);

/**
 * virtio_gpu_setup_scanout_resource() — bring up an independent 2D resource
 * and backing store for one scanout, the multi-monitor counterpart of
 * virtio_gpu_setup_framebuffer() (which only ever drives scanout 0, for
 * backward compatibility with the fbdev/compositor code that already
 * depends on its exact fields). @resource_id must not collide with another
 * live resource (scanout 0's framebuffer uses 1, the hardware cursor uses
 * 0xC0). On success @out_phys/@out_virt/@out_pitch describe the backing this
 * scanout's owner should render into before calling
 * virtio_gpu_transfer_to_host_2d()/virtio_gpu_resource_flush() on it.
 */
int virtio_gpu_setup_scanout_resource(u32 scanout_id, u32 resource_id,
                                      u32 width, u32 height,
                                      phys_addr_t *out_phys, void **out_virt,
                                      u32 *out_pitch);

/**
 * virtio_gpu_edid_supported() — true once feature negotiation has confirmed
 * the device offers VIRTIO_GPU_F_EDID (checked once at init, so this is a
 * plain flag read, not a fresh feature-bit query).
 */
bool virtio_gpu_edid_supported(void);

/**
 * virtio_gpu_get_edid(scanout_id, out, out_len) — fetch a scanout's raw EDID
 * block (up to VIRTIO_GPU_EDID_MAX_SIZE bytes) into @out. Returns the number
 * of bytes written, or a negative errno. Only meaningful when
 * virtio_gpu_edid_supported() is true; the command still round-trips
 * otherwise but the host has nothing meaningful to report.
 */
int virtio_gpu_get_edid(u32 scanout_id, u8 *out, u32 out_len);

/**
 * virtio_gpu_transfer_to_host_2d_rect / virtio_gpu_resource_flush_rect —
 * push and present a single rectangle of the scanout resource.
 *
 * The host holds the authoritative copy of every 2D resource; a guest write
 * to the backing pages is invisible until a TRANSFER_TO_HOST_2D copies that
 * region into the host resource and a RESOURCE_FLUSH tells the compositor to
 * repaint it. Naming only the damaged rectangle keeps a small update — a
 * blinking cursor, a moved window edge — from moving the whole framebuffer
 * across the virtqueue every frame.
 *
 * @offset is the byte offset of (@x, @y) within the backing, i.e.
 * @y * resource_stride + @x * bytes_per_pixel. The full-screen wrappers
 * below pass the whole scanout with @offset 0 and keep their old ABI.
 */
int virtio_gpu_transfer_to_host_2d_rect(u32 resource_id, u32 x, u32 y,
                                        u32 width, u32 height, u64 offset);
int virtio_gpu_resource_flush_rect(u32 resource_id, u32 x, u32 y,
                                   u32 width, u32 height);
int virtio_gpu_transfer_to_host_2d(u32 resource_id, u32 width, u32 height);
int virtio_gpu_resource_flush(u32 resource_id, u32 width, u32 height);

/**
 * virtio_gpu_resource_detach_backing(resource_id) — unregister the guest
 * physical pages that back @resource_id.  Call this before
 * virtio_gpu_resource_unref() whenever the resource was created with
 * virtio_gpu_resource_attach_backing().  Sending RESOURCE_UNREF without
 * first detaching backing is technically valid per the spec but some host
 * implementations handle it poorly; detaching first is always safe.
 */
int virtio_gpu_resource_detach_backing(u32 resource_id);

/**
 * virtio_gpu_resource_unref(resource_id) — destroy a host 2D resource.
 *
 * Frees the host's copy of the resource.  The guest backing pages are
 * returned to the physical allocator by the caller separately (the device
 * does not own guest pages).  Only call after
 * virtio_gpu_resource_detach_backing() and after making sure no scanout
 * still references this resource (virtio_gpu_set_scanout(scanout, 0, ...)).
 * resource_id 0 is the invalid sentinel; this function is a no-op for it.
 */
int virtio_gpu_resource_unref(u32 resource_id);

/* ── Hardware cursor plane (cursor queue) ───────────────────────────────────
 * The cursor is a host-side overlay: once its 64x64 image is uploaded, the
 * compositor never has to redraw or re-flush the scanout to move it — a
 * MOVE_CURSOR is one small message on a queue of its own.
 *
 * QEMU's virtio-gpu device tracks cursor state (image + position) per
 * scanout, keyed by the pos.scanout_id every UPDATE_CURSOR/MOVE_CURSOR
 * carries — a second monitor is not limited to the same on-screen position
 * as the first just because they share one host device. The resource itself
 * (the 64x64 image) is still one shared allocation: every scanout shows the
 * same cursor shape, only independently positioned, which is what every
 * caller here actually needs — a per-scanout image would mean a second
 * cursor_res_id and backing store per head for a shape that is, in every
 * desktop this compositor draws, identical across monitors anyway. */

/**
 * virtio_gpu_cursor_define(bgra, hot_x, hot_y, scanout_id) — upload a new
 * 64x64 cursor image (VIRTIO_GPU_CURSOR_W * VIRTIO_GPU_CURSOR_H pixels,
 * BGRA8888) and make it the active cursor on @scanout_id. Creates the shared
 * cursor resource on first call (from any scanout).
 */
int virtio_gpu_cursor_define(const u32 *bgra, u32 hot_x, u32 hot_y, u32 scanout_id);

/** virtio_gpu_cursor_move(x, y, scanout_id) — reposition the cursor hotspot
 *  on @scanout_id. */
int virtio_gpu_cursor_move(u32 x, u32 y, u32 scanout_id);

/** virtio_gpu_cursor_hide(scanout_id) — remove the cursor overlay from
 *  @scanout_id. */
int virtio_gpu_cursor_hide(u32 scanout_id);
