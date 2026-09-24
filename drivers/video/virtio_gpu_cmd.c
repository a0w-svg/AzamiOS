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

int virtio_gpu_get_display_info_all(struct virtio_gpu_display_one *out_modes,
                                    u32 max, u32 *out_count)
{
    struct virtio_gpu_ctrl_hdr cmd;
    struct virtio_gpu_resp_display_info resp;

    if (!out_modes || !out_count) return -1;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    memset(&resp, 0, sizeof(resp));

    if (virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp)) < 0) {
        return -1;
    }

    if (resp.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        return -1;
    }

    u32 n = max < VIRTIO_GPU_MAX_SCANOUTS ? max : VIRTIO_GPU_MAX_SCANOUTS;
    memset(out_modes, 0, sizeof(*out_modes) * max);
    for (u32 i = 0; i < n; i++) out_modes[i] = resp.pmodes[i];
    *out_count = n;
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

/* ── Resource teardown ────────────────────────────────────────────────────── */

int virtio_gpu_resource_detach_backing(u32 resource_id)
{
    if (resource_id == 0) return 0;

    struct virtio_gpu_resource_detach_backing cmd;
    struct virtio_gpu_ctrl_hdr resp;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type   = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    cmd.resource_id = resource_id;

    memset(&resp, 0, sizeof(resp));

    if (virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp)) < 0)
        return -1;
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_resource_unref(u32 resource_id)
{
    if (resource_id == 0) return 0;

    struct virtio_gpu_resource_unref cmd;
    struct virtio_gpu_ctrl_hdr resp;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    cmd.resource_id = resource_id;

    memset(&resp, 0, sizeof(resp));

    if (virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp)) < 0)
        return -1;

    int ret = (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
    if (ret == 0)
        pr_debug("[VIRTIO-GPU] resource %u freed on host\n", resource_id);
    else
        pr_debug("[VIRTIO-GPU] resource %u UNREF failed (host resp 0x%x)\n",
                 resource_id, resp.type);
    return ret;
}

/* ── 3D (Virgl) command helpers ──────────────────────────────────────────── */

int virtio_gpu_cmd_context_create(u32 ctx_id, const char *name)
{
    struct virtio_gpu_ctx_create cmd;
    struct virtio_gpu_ctrl_hdr resp;
    
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    cmd.hdr.ctx_id = ctx_id;
    if (name) {
        cmd.nlen = strlen(name);
        if (cmd.nlen > 63) cmd.nlen = 63;
        strncpy(cmd.debug_name, name, 64);
        cmd.debug_name[63] = '\0';
    } else {
        cmd.nlen = 0;
    }
    
    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp)) < 0) {
        return -1;
    }
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_cmd_context_destroy(u32 ctx_id)
{
    struct virtio_gpu_ctx_destroy cmd;
    struct virtio_gpu_ctrl_hdr resp;
    
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    cmd.hdr.ctx_id = ctx_id;
    
    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp)) < 0) {
        return -1;
    }
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_cmd_context_attach_resource(u32 ctx_id, u32 res_id)
{
    struct virtio_gpu_ctx_resource cmd;
    struct virtio_gpu_ctrl_hdr resp;
    
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    cmd.hdr.ctx_id = ctx_id;
    cmd.resource_id = res_id;
    
    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp)) < 0) {
        return -1;
    }
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_cmd_resource_create_3d(u32 res_id, u32 target, u32 format, u32 bind, u32 width, u32 height, u32 depth, u32 array_size)
{
    struct virtio_gpu_resource_create_3d cmd;
    struct virtio_gpu_ctrl_hdr resp;
    
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    cmd.resource_id = res_id;
    cmd.target = target;
    cmd.format = format;
    cmd.bind = bind;
    cmd.width = width;
    cmd.height = height;
    cmd.depth = depth;
    cmd.array_size = array_size;
    cmd.last_level = 0;
    cmd.nr_samples = 0;
    cmd.flags = 0;
    
    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp)) < 0) {
        return -1;
    }
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_cmd_submit_3d(u32 ctx_id, void *buf, u32 size)
{
    struct virtio_gpu_cmd_submit *cmd;
    struct virtio_gpu_ctrl_hdr resp;

    u32 cmd_size = sizeof(*cmd) + size;
    cmd = kzalloc(cmd_size);
    if (!cmd) return -1;

    cmd->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    cmd->hdr.ctx_id = ctx_id;
    cmd->size = size;
    memcpy(cmd + 1, buf, size);

    memset(&resp, 0, sizeof(resp));
    int ret = virtio_gpu_send_command(&g_gpu, cmd, cmd_size, &resp, sizeof(resp));
    kfree(cmd);

    if (ret < 0) return -1;
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_resource_attach_backing_pages(u32 resource_id, const phys_addr_t *pages,
                                             u32 npages, u32 page_size)
{
    if (!pages || npages == 0) return -1;

    struct virtio_gpu_resource_attach_backing *cmd;
    struct virtio_gpu_mem_entry *ents;
    struct virtio_gpu_ctrl_hdr resp;

    u32 cmd_size = sizeof(*cmd) + sizeof(*ents) * npages;
    cmd = kzalloc(cmd_size);
    if (!cmd) return -1;

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd->resource_id = resource_id;
    cmd->nr_entries = npages;

    ents = (struct virtio_gpu_mem_entry *)(cmd + 1);
    for (u32 i = 0; i < npages; i++) {
        ents[i].addr = pages[i];
        ents[i].length = page_size;
        ents[i].padding = 0;
    }

    memset(&resp, 0, sizeof(resp));
    int ret = virtio_gpu_send_command(&g_gpu, cmd, cmd_size, &resp, sizeof(resp));
    kfree(cmd);

    if (ret < 0) return -1;
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_cmd_get_capset_info(u32 capset_index, u32 *out_id,
                                   u32 *out_max_version, u32 *out_max_size)
{
    struct virtio_gpu_get_capset_info cmd;
    struct virtio_gpu_resp_capset_info resp;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
    cmd.capset_index = capset_index;

    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp)) < 0)
        return -1;
    if (resp.hdr.type != VIRTIO_GPU_RESP_OK_CAPSET_INFO)
        return -1;

    if (out_id) *out_id = resp.capset_id;
    if (out_max_version) *out_max_version = resp.capset_max_version;
    if (out_max_size) *out_max_size = resp.capset_max_size;
    return 0;
}

int virtio_gpu_cmd_get_capset(u32 capset_id, u32 capset_version,
                              void *out_buf, u32 buf_size, u32 *out_size)
{
    if (!out_buf || buf_size == 0) return -1;

    struct virtio_gpu_get_capset cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
    cmd.capset_id = capset_id;
    cmd.capset_version = capset_version;

    /* The response is a bare ctrl_hdr immediately followed by up to
     * buf_size bytes of capset data — allocate both together so the
     * virtqueue's single writable descriptor covers the whole thing. */
    u32 resp_size = sizeof(struct virtio_gpu_ctrl_hdr) + buf_size;
    struct virtio_gpu_ctrl_hdr *resp = kzalloc(resp_size);
    if (!resp) return -1;

    int ret = virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), resp, resp_size);
    if (ret < 0 || resp->type != VIRTIO_GPU_RESP_OK_CAPSET) {
        kfree(resp);
        return -1;
    }

    memcpy(out_buf, (u8 *)resp + sizeof(struct virtio_gpu_ctrl_hdr), buf_size);
    if (out_size) *out_size = buf_size;
    kfree(resp);
    return 0;
}

int virtio_gpu_cmd_transfer_to_host_3d(u32 ctx_id, u32 resource_id,
                                       u32 x, u32 y, u32 z, u32 w, u32 h, u32 d,
                                       u64 offset, u32 level, u32 stride,
                                       u32 layer_stride)
{
    struct virtio_gpu_transfer_host_3d cmd;
    struct virtio_gpu_ctrl_hdr resp;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    cmd.hdr.ctx_id = ctx_id;
    cmd.box.x = x; cmd.box.y = y; cmd.box.z = z;
    cmd.box.w = w; cmd.box.h = h; cmd.box.d = d;
    cmd.offset = offset;
    cmd.resource_id = resource_id;
    cmd.level = level;
    cmd.stride = stride;
    cmd.layer_stride = layer_stride;

    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp)) < 0)
        return -1;
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_cmd_transfer_from_host_3d(u32 ctx_id, u32 resource_id,
                                         u32 x, u32 y, u32 z, u32 w, u32 h, u32 d,
                                         u64 offset, u32 level, u32 stride,
                                         u32 layer_stride)
{
    struct virtio_gpu_transfer_host_3d cmd;
    struct virtio_gpu_ctrl_hdr resp;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    cmd.hdr.ctx_id = ctx_id;
    cmd.box.x = x; cmd.box.y = y; cmd.box.z = z;
    cmd.box.w = w; cmd.box.h = h; cmd.box.d = d;
    cmd.offset = offset;
    cmd.resource_id = resource_id;
    cmd.level = level;
    cmd.stride = stride;
    cmd.layer_stride = layer_stride;

    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp)) < 0)
        return -1;
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

/* ── Multi-monitor scanouts ───────────────────────────────────────────────── */

int virtio_gpu_setup_scanout_resource(u32 scanout_id, u32 resource_id,
                                      u32 width, u32 height,
                                      phys_addr_t *out_phys, void **out_virt,
                                      u32 *out_pitch)
{
    if (!out_phys || !out_virt || width == 0 || height == 0) return -1;

    u32 bpp = 4; /* 32-bit ARGB, same format as the primary scanout */
    u32 size = width * height * bpp;
    u32 pages = (size + 4095) / 4096;

    phys_addr_t phys = pmm_alloc_pages(pages);
    if (!phys) {
        pr_debug("[VIRTIO-GPU] scanout %u: out of memory for backing (%ux%u)\n",
                 scanout_id, width, height);
        return -1;
    }

    void *virt = vmm_map_io(phys, pages * 4096);
    if (!virt) {
        pr_debug("[VIRTIO-GPU] scanout %u: failed to map backing\n", scanout_id);
        return -1;
    }
    memset(virt, 0, size);

    if (virtio_gpu_resource_create_2d(resource_id, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                                      width, height) < 0) {
        pr_debug("[VIRTIO-GPU] scanout %u: failed to create resource %u\n",
                 scanout_id, resource_id);
        return -1;
    }
    if (virtio_gpu_resource_attach_backing(resource_id, phys, size) < 0) {
        pr_debug("[VIRTIO-GPU] scanout %u: failed to attach backing\n", scanout_id);
        return -1;
    }
    if (virtio_gpu_set_scanout(scanout_id, resource_id, width, height) < 0) {
        pr_debug("[VIRTIO-GPU] scanout %u: failed to bind resource %u\n",
                 scanout_id, resource_id);
        return -1;
    }
    if (virtio_gpu_transfer_to_host_2d(resource_id, width, height) < 0 ||
        virtio_gpu_resource_flush(resource_id, width, height) < 0) {
        pr_debug("[VIRTIO-GPU] scanout %u: initial flush failed\n", scanout_id);
        return -1;
    }

    *out_phys = phys;
    *out_virt = virt;
    if (out_pitch) *out_pitch = width * bpp;

    pr_debug("[VIRTIO-GPU] scanout %u ready: %ux%u, resource %u\n",
             scanout_id, width, height, resource_id);
    return 0;
}

/* ── EDID (VIRTIO_GPU_F_EDID) ─────────────────────────────────────────────── */

int virtio_gpu_get_edid(u32 scanout_id, u8 *out, u32 out_len)
{
    struct virtio_gpu_get_edid cmd;
    struct virtio_gpu_resp_edid resp;

    if (!out || out_len == 0) return -1;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_GET_EDID;
    cmd.scanout_id = scanout_id;

    memset(&resp, 0, sizeof(resp));

    if (virtio_gpu_send_command(&g_gpu, &cmd, sizeof(cmd), &resp, sizeof(resp)) < 0) {
        return -1;
    }
    if (resp.hdr.type != VIRTIO_GPU_RESP_OK_EDID) {
        return -1;
    }

    u32 n = resp.size < VIRTIO_GPU_EDID_MAX_SIZE ? resp.size : VIRTIO_GPU_EDID_MAX_SIZE;
    if (n > out_len) n = out_len;
    memcpy(out, resp.edid, n);
    return (int)n;
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

int virtio_gpu_cursor_define(const u32 *bgra, u32 hot_x, u32 hot_y, u32 scanout_id)
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
    cmd.pos.scanout_id = scanout_id;
    cmd.resource_id    = g_gpu.cursor_res_id;
    cmd.hot_x          = hot_x;
    cmd.hot_y          = hot_y;

    return virtio_gpu_send_cursor(&g_gpu, &cmd, sizeof(cmd), NULL, 0);
}

int virtio_gpu_cursor_move(u32 x, u32 y, u32 scanout_id)
{
    if (!g_gpu.cursor_virt) return -1;   /* no image uploaded yet */

    struct virtio_gpu_update_cursor cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type       = VIRTIO_GPU_CMD_MOVE_CURSOR;
    cmd.pos.scanout_id = scanout_id;
    cmd.pos.x          = x;
    cmd.pos.y          = y;
    cmd.resource_id    = g_gpu.cursor_res_id;   /* keep the current image */

    return virtio_gpu_send_cursor(&g_gpu, &cmd, sizeof(cmd), NULL, 0);
}

int virtio_gpu_cursor_hide(u32 scanout_id)
{
    if (!g_gpu.cursor_virt) return 0;

    struct virtio_gpu_update_cursor cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type       = VIRTIO_GPU_CMD_UPDATE_CURSOR;
    cmd.pos.scanout_id = scanout_id;
    cmd.resource_id    = 0;   /* 0 removes the overlay */

    return virtio_gpu_send_cursor(&g_gpu, &cmd, sizeof(cmd), NULL, 0);
}
