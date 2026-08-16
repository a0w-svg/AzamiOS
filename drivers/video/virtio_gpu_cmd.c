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

int virtio_gpu_set_scanout(u32 scanout_id, u32 resource_id, u32 width, u32 height)
{
    struct virtio_gpu_set_scanout cmd;
    struct virtio_gpu_ctrl_hdr resp;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd.resource_id = resource_id;
    cmd.scanout_id = scanout_id;
    cmd.r.x = 0;
    cmd.r.y = 0;
    cmd.r.width = width;
    cmd.r.height = height;

    memset(&resp, 0, sizeof(resp));

    if (virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp)) < 0) {
        return -1;
    }

    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_transfer_to_host_2d(u32 resource_id, u32 width, u32 height)
{
    struct virtio_gpu_transfer_to_host_2d cmd;
    struct virtio_gpu_ctrl_hdr resp;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd.resource_id = resource_id;
    cmd.offset = 0;
    cmd.r.x = 0;
    cmd.r.y = 0;
    cmd.r.width = width;
    cmd.r.height = height;

    memset(&resp, 0, sizeof(resp));

    if (virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp)) < 0) {
        return -1;
    }

    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_resource_flush(u32 resource_id, u32 width, u32 height)
{
    struct virtio_gpu_resource_flush cmd;
    struct virtio_gpu_ctrl_hdr resp;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd.resource_id = resource_id;
    cmd.r.x = 0;
    cmd.r.y = 0;
    cmd.r.width = width;
    cmd.r.height = height;

    memset(&resp, 0, sizeof(resp));

    if (virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp)) < 0) {
        return -1;
    }

    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
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
    
    /* Allocate physical memory for the framebuffer (contiguous) */
    u32 bpp = 4; /* 32-bit ARGB */
    g_gpu.framebuffer_size = width * height * bpp;
    u32 pages = (g_gpu.framebuffer_size + 4095) / 4096;
    
    g_gpu.framebuffer_phys = pmm_alloc_pages(pages);
    if (!g_gpu.framebuffer_phys) {
        pr_debug("[VIRTIO-GPU] Out of memory for framebuffer\n");
        return -1;
    }
    
    /* Map it to virtual memory */
    g_gpu.framebuffer_virt = vmm_map_io(g_gpu.framebuffer_phys, pages * 4096);
    memset(g_gpu.framebuffer_virt, 0, g_gpu.framebuffer_size); /* Clear screen to black */
    
    /* 2. Create 2D Resource */
    if (virtio_gpu_resource_create_2d(g_gpu.resource_id, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM, width, height) < 0) {
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
