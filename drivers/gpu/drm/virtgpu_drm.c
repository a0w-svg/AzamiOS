/* ============================================================================
 * AzamiOS — virtio-gpu: KMS driver for the VirtIO GPU
 * File: drivers/gpu/drm/virtgpu_drm.c
 *
 * virtio-gpu has no scanout aperture the CPU can write to.  The guest owns a
 * host-backed 2D resource, renders into its backing pages, and then tells the
 * host which region changed — TRANSFER_TO_HOST_2D followed by RESOURCE_FLUSH.
 * So every buffer object here is a shadow, and a page flip is a blit into the
 * resource backing plus a flush, which is exactly how Linux's virtio-gpu
 * driver handles a dumb-buffer client.
 *
 * The transport and command helpers already exist in drivers/video; this file
 * is the KMS half that was missing, and it is what finally brings the
 * virtio-gpu framebuffer up.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "drm_core.h"
#include "../../base/pci_bus.h"
#include "../../video/virtio_gpu.h"
#include "../../../kernel/mm/kmalloc.h"
#include "../../../kernel/lib/string.h"

extern virtio_gpu_state_t g_gpu;
/* Command helpers and their rectangle-scoped variants are declared in
 * virtio_gpu.h, included above. */

typedef struct virtgpu_device {
    u32   width, height, pitch;
    void *backing;          /* resource backing pages, HHDM mapped */
    u32   resource_id;
    drm_crtc_t *crtc;
} virtgpu_device_t;

/* ── Scanout ─────────────────────────────────────────────────────────────── */

static int virtgpu_flush(drm_crtc_t *crtc, drm_framebuffer_t *fb,
                         const drm_rect_t *clip)
{
    virtgpu_device_t *vg = (virtgpu_device_t *)crtc->dev->dev_private;
    if (!vg || !fb || !fb->obj) return -EINVAL;

    drm_rect_t r = { 0, 0,
                     fb->width  < vg->width  ? fb->width  : vg->width,
                     fb->height < vg->height ? fb->height : vg->height };
    if (clip) {
        if (clip->x1 > r.x1) r.x1 = clip->x1;
        if (clip->y1 > r.y1) r.y1 = clip->y1;
        if (clip->x2 < r.x2) r.x2 = clip->x2;
        if (clip->y2 < r.y2) r.y2 = clip->y2;
        if (r.x1 >= r.x2 || r.y1 >= r.y2) return 0;
    }

    /* Copy only the damaged rows into the resource backing, then name that
     * same rectangle to the host: TRANSFER_TO_HOST_2D moves just those bytes
     * across the virtqueue and RESOURCE_FLUSH repaints just that region.  A
     * one-line caret blink no longer drags the whole framebuffer to the host
     * every frame. */
    drm_gem_blit_rect(fb->obj, vg->backing, vg->pitch, &r, 32);

    u32 dw = r.x2 - r.x1, dh = r.y2 - r.y1;
    u64 doff = (u64)r.y1 * vg->pitch + (u64)r.x1 * 4;

    if (virtio_gpu_transfer_to_host_2d_rect(vg->resource_id, r.x1, r.y1, dw, dh, doff) < 0)
        return -EIO;
    if (virtio_gpu_resource_flush_rect(vg->resource_id, r.x1, r.y1, dw, dh) < 0)
        return -EIO;
    return 0;
}

/* ── Hardware cursor ─────────────────────────────────────────────────────── */

static int virtgpu_cursor_set(drm_crtc_t *crtc, drm_gem_object_t *bo, u32 w, u32 h)
{
    if (!bo)
        return virtio_gpu_cursor_hide() ? -EIO : 0;

    /* virtio-gpu's hardware cursor is a fixed 64x64 image; a client asking
     * for another size falls back to a software cursor by getting -EINVAL. */
    if ((w && w != VIRTIO_GPU_CURSOR_W) || (h && h != VIRTIO_GPU_CURSOR_H))
        return -EINVAL;

    /* Gather the ARGB8888 image into a linear 64x64 buffer. drm_gem_blit_rect
     * copes with a bo whose backing pages are not contiguous. One master, and
     * the device lock inside the command path, keep this static buffer from
     * being entered twice at once. */
    static u32 img[VIRTIO_GPU_CURSOR_W * VIRTIO_GPU_CURSOR_H];
    drm_rect_t all = { 0, 0, VIRTIO_GPU_CURSOR_W, VIRTIO_GPU_CURSOR_H };
    __builtin_memset(img, 0, sizeof(img));
    drm_gem_blit_rect(bo, img, VIRTIO_GPU_CURSOR_W * 4, &all, 32);

    return virtio_gpu_cursor_define(img, (u32)crtc->cursor_hot_x,
                                    (u32)crtc->cursor_hot_y) ? -EIO : 0;
}

static int virtgpu_cursor_move(drm_crtc_t *crtc, s32 x, s32 y)
{
    (void)crtc;
    /* MOVE_CURSOR places the hotspot on the scanout; a cursor dragged past
     * the top or left edge is clamped to the origin. */
    u32 ux = x < 0 ? 0u : (u32)x;
    u32 uy = y < 0 ? 0u : (u32)y;
    return virtio_gpu_cursor_move(ux, uy) ? -EIO : 0;
}

static int virtgpu_mode_set(drm_crtc_t *crtc, drm_framebuffer_t *fb,
                            const drm_display_mode_t *mode, u32 x, u32 y)
{
    (void)x; (void)y;
    virtgpu_device_t *vg = (virtgpu_device_t *)crtc->dev->dev_private;
    if (!mode) return 0;

    /* The resource is created once at its host-reported size; a different
     * mode would need the resource torn down and rebuilt, which this driver
     * does not do yet. */
    if (mode->hdisplay != vg->width || mode->vdisplay != vg->height) {
        return -EINVAL;
    }
    if (virtio_gpu_set_scanout(0, vg->resource_id, vg->width, vg->height) < 0) return -EIO;
    if (fb) return virtgpu_flush(crtc, fb, NULL);
    return 0;
}

static int virtgpu_load(drm_device_t *dev)
{
    virtgpu_device_t *vg = (virtgpu_device_t *)dev->dev_private;

    dev->min_width     = vg->width;
    dev->max_width     = vg->width;
    dev->min_height    = vg->height;
    dev->max_height    = vg->height;
    dev->cursor_width  = VIRTIO_GPU_CURSOR_W;
    dev->cursor_height = VIRTIO_GPU_CURSOR_H;
    dev->prefer_shadow = true;

    drm_crtc_t *crtc = drm_crtc_create(dev);
    if (!crtc) return -ENOMEM;
    drm_encoder_t *enc = drm_encoder_create(dev, DRM_MODE_ENCODER_VIRTUAL, 1U << crtc->index);
    drm_connector_t *conn = drm_connector_create(dev, DRM_MODE_CONNECTOR_VIRTUAL, enc);
    if (!enc || !conn) return -ENOMEM;

    drm_display_mode_t mode;
    drm_mode_simple(&mode, vg->width, vg->height, 60);
    mode.type |= DRM_MODE_TYPE_PREFERRED;
    drm_connector_add_mode(conn, &mode);

    static const u32 formats[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };
    drm_plane_create(dev, DRM_PLANE_TYPE_PRIMARY, 1U << crtc->index,
                     formats, ARRAY_SIZE(formats));

    /* A cursor plane, so universal-plane and atomic clients see the hardware
     * cursor the legacy CURSOR ioctl drives through .cursor_set/.cursor_move. */
    static const u32 cursor_formats[] = { DRM_FORMAT_ARGB8888 };
    drm_plane_create(dev, DRM_PLANE_TYPE_CURSOR, 1U << crtc->index,
                     cursor_formats, ARRAY_SIZE(cursor_formats));

    crtc->mode       = mode;
    crtc->mode_valid = true;
    crtc->enabled    = true;
    vg->crtc         = crtc;

    pr_debug("[VIRTIO-GPU-DRM] %ux%u, resource %u backed at %p\n",
             vg->width, vg->height, vg->resource_id, vg->backing);
    return 0;
}

static void virtgpu_unload(drm_device_t *dev)
{
    if (dev->dev_private) {
        kfree(dev->dev_private);
        dev->dev_private = NULL;
    }
}

static const drm_driver_t virtgpu_drm_driver = {
    .name      = "virtio_gpu",
    .desc      = "VirtIO GPU 2D display",
    .date      = "20260831",
    .major     = 0, .minor = 1, .patchlevel = 0,
    .features  = DRIVER_MODESET | DRIVER_GEM | DRIVER_RENDER,
    .load      = virtgpu_load,
    .unload    = virtgpu_unload,
    .mode_set    = virtgpu_mode_set,
    .page_flip   = virtgpu_flush,
    .dirty_fb    = virtgpu_flush,
    .cursor_set  = virtgpu_cursor_set,
    .cursor_move = virtgpu_cursor_move,
};

/* ── PCI binding ─────────────────────────────────────────────────────────── */

static int virtgpu_pci_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;

    /* The VirtIO transport is brought up during PCI enumeration; without a
     * control queue there is nothing to talk to. */
    if (!g_gpu.controlq) {
        pr_debug("[VIRTIO-GPU-DRM] transport not initialised — declining\n");
        return -ENODEV;
    }

    /* Create the host resource and its backing on first use. */
    if (g_gpu.framebuffer_phys == 0) {
        if (virtio_gpu_setup_framebuffer() < 0) return -ENODEV;
    }
    if (!g_gpu.framebuffer_virt) return -ENODEV;

    virtgpu_device_t *vg = (virtgpu_device_t *)kzalloc(sizeof(virtgpu_device_t));
    if (!vg) return -ENOMEM;

    vg->width       = g_gpu.screen_width;
    vg->height      = g_gpu.screen_height;
    vg->pitch       = g_gpu.screen_width * 4;
    vg->backing     = g_gpu.framebuffer_virt;
    vg->resource_id = g_gpu.resource_id;

    drm_device_t *dev = drm_dev_alloc(&virtgpu_drm_driver, dm);
    if (!dev) { kfree(vg); return -ENOMEM; }
    dev->dev_private = vg;

    int ret = drm_dev_register(dev);
    if (ret != 0) {
        kfree(vg);
        kfree(dev);
        return ret;
    }

    dm_set_drvdata(dm, dev);
    return 0;
}

static void virtgpu_pci_remove(dm_device_t *dm)
{
    drm_device_t *dev = (drm_device_t *)dm_get_drvdata(dm);
    if (dev) drm_dev_unregister(dev);
}

static const pci_device_id_t virtgpu_pci_ids[] = {
    { PCI_DEVICE(0x1AF4, 0x1050) },   /* virtio-gpu (modern)  */
    { PCI_DEVICE(0x1AF4, 0x1010) },   /* virtio-gpu (legacy)  */
    { 0 }
};

static pci_driver_t virtgpu_pci_driver = {
    .drv      = { .name = "virtio_gpu" },
    .id_table = virtgpu_pci_ids,
    .probe    = virtgpu_pci_probe,
    .remove   = virtgpu_pci_remove,
};

void virtgpu_drm_init(void)
{
    pci_driver_register(&virtgpu_pci_driver);
}
