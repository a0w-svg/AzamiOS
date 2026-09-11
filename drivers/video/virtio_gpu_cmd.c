/* ============================================================================
 * AzamiOS — VirtIO-GPU Driver Commands
 * File: drivers/video/virtio_gpu_cmd.c
 *
 * Implements 2D command submission and resource management.
 * ============================================================================ */

#include "virtio_gpu.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/mm/pmm.h"
#include "../../arch/x86_64/mm/vmm.h"
#include <azami/debug.h>
#include "../../kernel/lib/string.h"

extern virtio_gpu_state_t g_gpu;

int virtio_gpu_get_display_info(u32 *width, u32 *height)
{
    struct virtio_gpu_ctrl_hdr cmd;
    struct virtio_gpu_resp_display_info resp;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    memset(&resp, 0, sizeof(resp));

    if (virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp)) < 0) {
        return -1;
    }

    if (resp.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        return -1;
    }

    *width = resp.pmodes[0].r.width;
    *height = resp.pmodes[0].r.height;
    return 0;
}

int virtio_gpu_resource_create_2d(u32 resource_id, u32 format, u32 width, u32 height)
{
    struct virtio_gpu_resource_create_2d cmd;
    struct virtio_gpu_ctrl_hdr resp;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd.resource_id = resource_id;
    cmd.format = format;
    cmd.width = width;
    cmd.height = height;

    memset(&resp, 0, sizeof(resp));

    if (virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp)) < 0) {
        return -1;
    }

    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_resource_attach_backing(u32 resource_id, phys_addr_t ptr, u32 length)
{
    /* The command requires an array of entries appended to the end of the command struct */
    struct virtio_gpu_resource_attach_backing *cmd;
    struct virtio_gpu_mem_entry *ents;
    struct virtio_gpu_ctrl_hdr resp;
    
    u32 cmd_size = sizeof(*cmd) + sizeof(*ents);
    cmd = kzalloc(cmd_size);
    if (!cmd) return -1;

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd->resource_id = resource_id;
    cmd->nr_entries = 1;

    ents = (struct virtio_gpu_mem_entry *)(cmd + 1);
    ents->addr = ptr;
    ents->length = length;
    ents->padding = 0;

    memset(&resp, 0, sizeof(resp));

    int ret = virtio_gpu_send_command(&g_gpu, cmd, cmd_size, &resp, sizeof(resp));
    kfree(cmd);

    if (ret < 0) return -1;
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_set_scanout_offset(u32 scanout_id, u32 resource_id, u32 x, u32 y, u32 width, u32 height)
{
    struct virtio_gpu_set_scanout cmd;
    struct virtio_gpu_ctrl_hdr resp;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd.resource_id = resource_id;
    cmd.scanout_id = scanout_id;
    cmd.r.x = x;
    cmd.r.y = y;
    cmd.r.width = width;
    cmd.r.height = height;

    memset(&resp, 0, sizeof(resp));

    if (virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp)) < 0) {
        return -1;
    }

    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_set_scanout(u32 scanout_id, u32 resource_id, u32 width, u32 height)
{
    return virtio_gpu_set_scanout_offset(scanout_id, resource_id, 0, 0, width, height);
}

int virtio_gpu_transfer_to_host_2d_rect(u32 resource_id, u32 x, u32 y,
                                        u32 width, u32 height, u64 offset)
{
    struct virtio_gpu_transfer_to_host_2d cmd;
    struct virtio_gpu_ctrl_hdr resp;

    if (width == 0 || height == 0) return 0;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd.resource_id = resource_id;
    cmd.offset = offset;
    cmd.r.x = x;
    cmd.r.y = y;
    cmd.r.width = width;
    cmd.r.height = height;

    memset(&resp, 0, sizeof(resp));

    if (virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp)) < 0) {
        return -1;
    }

    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_resource_flush_rect(u32 resource_id, u32 x, u32 y,
                                   u32 width, u32 height)
{
    struct virtio_gpu_resource_flush cmd;
    struct virtio_gpu_ctrl_hdr resp;

    if (width == 0 || height == 0) return 0;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd.resource_id = resource_id;
    cmd.r.x = x;
    cmd.r.y = y;
    cmd.r.width = width;
    cmd.r.height = height;

    memset(&resp, 0, sizeof(resp));

    if (virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp)) < 0) {
        return -1;
    }

    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

/* Whole-scanout wrappers: unchanged ABI for callers that repaint everything
 * (initial mode set, a client with no damage information). */
int virtio_gpu_transfer_to_host_2d(u32 resource_id, u32 width, u32 height)
{
    return virtio_gpu_transfer_to_host_2d_rect(resource_id, 0, 0, width, height, 0);
}

int virtio_gpu_resource_flush(u32 resource_id, u32 width, u32 height)
{
    return virtio_gpu_resource_flush_rect(resource_id, 0, 0, width, height);
}

int virtio_gpu_setup_framebuffer(void)
{
    /* 1. Get Display Info */
    u32 width, height;
    if (virtio_gpu_get_display_info(&width, &height) < 0) {
        pr_debug("[VIRTIO-GPU] Failed to get display info\n");
        return -1;
    }
    
    /* Fallback if display info is weird */
    if (width == 0 || height == 0) {
        width = 1024;
        height = 768;
    }
    
    pr_debug("[VIRTIO-GPU] Display resolution: %ux%u\n", width, height);

    g_gpu.screen_width = width;
    g_gpu.screen_height = height;
    g_gpu.resource_id = 1;
    
    /* Allocate physical memory for double-buffered framebuffer (contiguous) */
    u32 bpp = 4; /* 32-bit ARGB */
    g_gpu.framebuffer_size = width * height * bpp * 2; /* 2 full screens */
    u32 pages = (g_gpu.framebuffer_size + 4095) / 4096;
    
    g_gpu.framebuffer_phys = pmm_alloc_pages(pages);
    if (!g_gpu.framebuffer_phys) {
        /* Fallback to single buffer if memory is tight */
        g_gpu.framebuffer_size = width * height * bpp;
        pages = (g_gpu.framebuffer_size + 4095) / 4096;
        g_gpu.framebuffer_phys = pmm_alloc_pages(pages);
        if (!g_gpu.framebuffer_phys) {
            pr_debug("[VIRTIO-GPU] Out of memory for framebuffer\n");
            return -1;
        }
    }
    
    /* Map it to virtual memory */
    g_gpu.framebuffer_virt = vmm_map_io(g_gpu.framebuffer_phys, pages * 4096);
    memset(g_gpu.framebuffer_virt, 0, g_gpu.framebuffer_size); /* Clear screen to black */
    
    /* 2. Create 2D Resource (height sized to hold all allocated buffer space) */
    u32 total_h = (g_gpu.framebuffer_size >= width * height * bpp * 2) ? (height * 2) : height;
    if (virtio_gpu_resource_create_2d(g_gpu.resource_id, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM, width, total_h) < 0) {
        pr_debug("[VIRTIO-GPU] Failed to create 2D resource\n");
        return -1;
    }
    
    /* 3. Attach Backing */
    if (virtio_gpu_resource_attach_backing(g_gpu.resource_id, g_gpu.framebuffer_phys, g_gpu.framebuffer_size) < 0) {
        pr_debug("[VIRTIO-GPU] Failed to attach backing\n");
        return -1;
    }
    
    /* 4. Set Scanout */
    if (virtio_gpu_set_scanout(0, g_gpu.resource_id, width, height) < 0) {
        pr_debug("[VIRTIO-GPU] Failed to set scanout\n");
        return -1;
    }
    
    /* 5. Transfer to Host */
    if (virtio_gpu_transfer_to_host_2d(g_gpu.resource_id, width, height) < 0) {
        pr_debug("[VIRTIO-GPU] Failed to transfer to host\n");
        return -1;
    }
    
    /* 6. Flush */
    if (virtio_gpu_resource_flush(g_gpu.resource_id, width, height) < 0) {
        pr_debug("[VIRTIO-GPU] Failed to flush resource\n");
        return -1;
    }
    
    pr_debug("[VIRTIO-GPU] Framebuffer setup complete.\n");
    return 0;
}

/* ── Hardware cursor ─────────────────────────────────────────────────────────
 *
 * The cursor is a separate host resource that the device composites over the
 * scanout on its own. Once its 64x64 BGRA image is uploaded, moving it is a
 * single MOVE_CURSOR on the cursor queue: no scanout transfer, no flush, and
 * nothing for the compositor to redraw. UPDATE_CURSOR with resource_id 0
 * removes it.
 * ------------------------------------------------------------------------- */

#define VIRTIO_GPU_CURSOR_RESID  0xC0
#define VIRTIO_GPU_CURSOR_BYTES  (VIRTIO_GPU_CURSOR_W * VIRTIO_GPU_CURSOR_H * 4)

static int virtio_gpu_cursor_alloc(void)
{
    if (g_gpu.cursor_virt) return 0;

    u32 pages = (VIRTIO_GPU_CURSOR_BYTES + 4095) / 4096;
    g_gpu.cursor_phys = pmm_alloc_pages(pages);
    if (!g_gpu.cursor_phys) return -1;

    g_gpu.cursor_virt = vmm_map_io(g_gpu.cursor_phys, pages * 4096);
    if (!g_gpu.cursor_virt) return -1;
    memset(g_gpu.cursor_virt, 0, VIRTIO_GPU_CURSOR_BYTES);
    g_gpu.cursor_res_id = VIRTIO_GPU_CURSOR_RESID;

    if (virtio_gpu_resource_create_2d(g_gpu.cursor_res_id,
                                      VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                                      VIRTIO_GPU_CURSOR_W, VIRTIO_GPU_CURSOR_H) < 0)
        return -1;
    if (virtio_gpu_resource_attach_backing(g_gpu.cursor_res_id, g_gpu.cursor_phys,
                                           VIRTIO_GPU_CURSOR_BYTES) < 0)
        return -1;

    pr_debug("[VIRTIO-GPU] hardware cursor resource %u ready (64x64 BGRA)\n",
             g_gpu.cursor_res_id);
    return 0;
}

int virtio_gpu_cursor_define(const u32 *bgra, u32 hot_x, u32 hot_y)
{
    if (!bgra) return -1;
    if (virtio_gpu_cursor_alloc() < 0) return -1;

    memcpy(g_gpu.cursor_virt, bgra, VIRTIO_GPU_CURSOR_BYTES);
    if (virtio_gpu_transfer_to_host_2d_rect(g_gpu.cursor_res_id, 0, 0,
                                            VIRTIO_GPU_CURSOR_W,
                                            VIRTIO_GPU_CURSOR_H, 0) < 0)
        return -1;

    struct virtio_gpu_update_cursor cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type       = VIRTIO_GPU_CMD_UPDATE_CURSOR;
    cmd.pos.scanout_id = 0;
    cmd.resource_id    = g_gpu.cursor_res_id;
    cmd.hot_x          = hot_x;
    cmd.hot_y          = hot_y;

    return virtio_gpu_send_cursor(&g_gpu, &cmd, sizeof(cmd), NULL, 0);
}

int virtio_gpu_cursor_move(u32 x, u32 y)
{
    if (!g_gpu.cursor_virt) return -1;   /* no image uploaded yet */

    struct virtio_gpu_update_cursor cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type       = VIRTIO_GPU_CMD_MOVE_CURSOR;
    cmd.pos.scanout_id = 0;
    cmd.pos.x          = x;
    cmd.pos.y          = y;
    cmd.resource_id    = g_gpu.cursor_res_id;   /* keep the current image */

    return virtio_gpu_send_cursor(&g_gpu, &cmd, sizeof(cmd), NULL, 0);
}

int virtio_gpu_cursor_hide(void)
{
    if (!g_gpu.cursor_virt) return 0;

    struct virtio_gpu_update_cursor cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type       = VIRTIO_GPU_CMD_UPDATE_CURSOR;
    cmd.pos.scanout_id = 0;
    cmd.resource_id    = 0;   /* 0 removes the overlay */

    return virtio_gpu_send_cursor(&g_gpu, &cmd, sizeof(cmd), NULL, 0);
}
