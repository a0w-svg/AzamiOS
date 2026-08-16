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

    /* success responses */
    VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO,

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
} virtio_gpu_state_t;

/**
 * virtio_gpu_init - Initialize the VirtIO-GPU driver.
 * Returns 0 on success.
 */
int virtio_gpu_init(device_t *pci_dev);

/**
 * virtio_gpu_send_command - Submits a command and waits for response.
 */
int virtio_gpu_send_command(virtio_gpu_state_t *gpu, void *cmd, u32 cmd_size, void *resp, u32 resp_size);
