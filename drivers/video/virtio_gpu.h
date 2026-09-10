/* ============================================================================
 * AzamiOS — VirtIO-GPU Driver Header
 * File: drivers/video/virtio_gpu.h
 *
 * Implements the VirtIO-GPU control protocol and driver structures.
 * ============================================================================ */
#pragma once

#include "../../hal/virtio_pci.h"

#define VIRTIO_GPU_F_VIRGL 0 /* We only support 2D for now */

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
    VIRTIO_GPU_CMD_GET_EDID = 0x010a,

    /* cursor commands (submitted on the cursor queue, index 1) */
    VIRTIO_GPU_CMD_UPDATE_CURSOR = 0x0300,
    VIRTIO_GPU_CMD_MOVE_CURSOR,

    /* success responses */
    VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO,
    VIRTIO_GPU_RESP_OK_EDID = 0x1104,

    /* error responses */
    VIRTIO_GPU_RESP_ERR_UNSPEC = 0x1200,
    VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY,
    VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER,
};

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

/* RESOURCE_CREATE_2D */
struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 resource_id;
    u32 format;
    u32 width;
    u32 height;
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
 * virtio_gpu_init - Initialize the VirtIO-GPU driver.
 * Returns 0 on success.
 */
int virtio_gpu_init(device_t *pci_dev);

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
int virtio_gpu_setup_framebuffer(void);

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

/* ── Hardware cursor plane (cursor queue) ───────────────────────────────────
 * The cursor is a host-side overlay: once its 64x64 image is uploaded, the
 * compositor never has to redraw or re-flush the scanout to move it — a
 * MOVE_CURSOR is one small message on a queue of its own. */

/**
 * virtio_gpu_cursor_define(bgra, hot_x, hot_y) — upload a new 64x64 cursor
 * image (VIRTIO_GPU_CURSOR_W * VIRTIO_GPU_CURSOR_H pixels, BGRA8888) and make
 * it the active cursor. Creates the dedicated cursor resource on first call.
 */
int virtio_gpu_cursor_define(const u32 *bgra, u32 hot_x, u32 hot_y);

/** virtio_gpu_cursor_move(x, y) — reposition the cursor hotspot on scanout 0. */
int virtio_gpu_cursor_move(u32 x, u32 y);

/** virtio_gpu_cursor_hide() — remove the cursor overlay. */
int virtio_gpu_cursor_hide(void);
