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
 *
 * Multi-monitor: GET_DISPLAY_INFO reports up to VIRTIO_GPU_MAX_SCANOUTS
 * independent outputs (QEMU's virtio-gpu-pci exposes more than one when
 * started with max_outputs > 1). Scanout 0 keeps reusing the resource
 * virtio_gpu_setup_framebuffer() already stands up — fbdev.c and the
 * hardware cursor both depend on its exact resource id and backing — while
 * every other enabled scanout gets its own independent resource, backing
 * store and CRTC/encoder/connector, each fed by its own GEM shadow buffer.
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

/* One CRTC's worth of scanout state: its own resource, backing and pitch, so
 * flushing one monitor never touches another's resource. */
typedef struct virtgpu_output {
    u32   scanout_id;
    u32   width, height, pitch;
    void *backing;          /* resource backing pages, HHDM mapped */
    u32   resource_id;
    drm_crtc_t *crtc;
} virtgpu_output_t;

typedef struct virtgpu_device {
    virtgpu_output_t outputs[VIRTIO_GPU_MAX_SCANOUTS];
    u32              num_outputs;
} virtgpu_device_t;

static virtgpu_output_t *virtgpu_find_output(virtgpu_device_t *vg, drm_crtc_t *crtc)
{
    if (!vg) return NULL;
    for (u32 i = 0; i < vg->num_outputs; i++) {
        if (vg->outputs[i].crtc == crtc) return &vg->outputs[i];
    }
    return NULL;
}

/* ── Scanout ─────────────────────────────────────────────────────────────── */

static int virtgpu_flush(drm_crtc_t *crtc, drm_framebuffer_t *fb,
                         const drm_rect_t *clip)
{
    virtgpu_device_t *vg = (virtgpu_device_t *)crtc->dev->dev_private;
    virtgpu_output_t *out = virtgpu_find_output(vg, crtc);
    if (!out || !fb || !fb->obj) return -EINVAL;

    drm_rect_t r = { 0, 0,
                     fb->width  < out->width  ? fb->width  : out->width,
                     fb->height < out->height ? fb->height : out->height };
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
    drm_gem_blit_rect(fb->obj, out->backing, out->pitch, &r, 32);

    u32 dw = r.x2 - r.x1, dh = r.y2 - r.y1;
    u64 doff = (u64)r.y1 * out->pitch + (u64)r.x1 * 4;

    if (virtio_gpu_transfer_to_host_2d_rect(out->resource_id, r.x1, r.y1, dw, dh, doff) < 0)
        return -EIO;
    if (virtio_gpu_resource_flush_rect(out->resource_id, r.x1, r.y1, dw, dh) < 0)
        return -EIO;
    return 0;
}

/* ── Hardware cursor ─────────────────────────────────────────────────────── */

static int virtgpu_cursor_set(drm_crtc_t *crtc, drm_gem_object_t *bo, u32 w, u32 h)
{
    virtgpu_device_t *vg = (virtgpu_device_t *)crtc->dev->dev_private;
    virtgpu_output_t *out = virtgpu_find_output(vg, crtc);
    /* UPDATE_CURSOR always targets scanout 0 in this driver's command
     * wrappers (fbdev.c's cursor ioctls make the same assumption), so a
     * second monitor gets no hardware cursor rather than a silently wrong
     * one on the primary head. */
    if (!out || out->scanout_id != 0) return -EINVAL;

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
    virtgpu_device_t *vg = (virtgpu_device_t *)crtc->dev->dev_private;
    virtgpu_output_t *out = virtgpu_find_output(vg, crtc);
    if (!out || out->scanout_id != 0) return -EINVAL;

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
    virtgpu_output_t *out = virtgpu_find_output(vg, crtc);
    if (!out || !mode) return 0;

    /* The resource is created once at its host-reported size; a different
     * mode would need the resource torn down and rebuilt, which this driver
     * does not do yet. */
    if (mode->hdisplay != out->width || mode->vdisplay != out->height) {
        return -EINVAL;
    }
    if (virtio_gpu_set_scanout(out->scanout_id, out->resource_id, out->width, out->height) < 0)
        return -EIO;
    if (fb) return virtgpu_flush(crtc, fb, NULL);
    return 0;
}

/* ── EDID (VIRTIO_GPU_F_EDID) ────────────────────────────────────────────────
 *
 * GET_DISPLAY_INFO already gives us each scanout's resolution, which is all
 * mode-setting strictly needs; EDID adds the monitor's actual preferred
 * refresh rate, used here only to pace this scanout's software vblank clock
 * (drm_vblank.c) closer to what the host is really presenting at instead of
 * a flat assumed 60 Hz.
 * ------------------------------------------------------------------------- */

static u32 virtgpu_probe_refresh(u32 scanout_id, u32 width, u32 height)
{
    if (!virtio_gpu_edid_supported()) return 60;

    u8 edid[128];
    if (virtio_gpu_get_edid(scanout_id, edid, sizeof(edid)) < (int)sizeof(edid))
        return 60;

    /* EDID 1.x base block: byte 54 starts the first of four fixed 18-byte
     * descriptor slots. The spec reserves the first slot for the display's
     * preferred timing; a non-timing descriptor (name, serial, ...) marks
     * itself with a zero pixel clock in its first two bytes. */
    const u8 *d = &edid[54];
    u32 pixclk_10khz = (u32)d[0] | ((u32)d[1] << 8);
    if (pixclk_10khz == 0) return 60;

    u32 hactive = (u32)d[2] | ((((u32)d[4] >> 4) & 0xF) << 8);
    u32 hblank  = (u32)d[3] | (((u32)d[4] & 0xF) << 8);
    u32 vactive = (u32)d[5] | ((((u32)d[7] >> 4) & 0xF) << 8);
    u32 vblank  = (u32)d[6] | (((u32)d[7] & 0xF) << 8);
    u32 htotal = hactive + hblank;
    u32 vtotal = vactive + vblank;
    if (htotal == 0 || vtotal == 0) return 60;

    /* This descriptor should describe the same panel GET_DISPLAY_INFO
     * already reported for this scanout; if it names a different resolution
     * (a stale or otherwise unrelated EDID), don't let it feed a refresh
     * rate that does not belong to this mode. */
    if (hactive != width || vactive != height) return 60;

    u64 refresh = ((u64)pixclk_10khz * 10000ULL) / ((u64)htotal * (u64)vtotal);
    if (refresh < 24 || refresh > 240) return 60;   /* implausible: keep 60 */
    return (u32)refresh;
}

/* ── Device bring-up ─────────────────────────────────────────────────────── */

static int virtgpu_load(drm_device_t *dev)
{
    virtgpu_device_t *vg = (virtgpu_device_t *)dev->dev_private;

    dev->min_width     = vg->outputs[0].width;
    dev->max_width     = vg->outputs[0].width;
    dev->min_height    = vg->outputs[0].height;
    dev->max_height    = vg->outputs[0].height;
    dev->cursor_width  = VIRTIO_GPU_CURSOR_W;
    dev->cursor_height = VIRTIO_GPU_CURSOR_H;
    dev->prefer_shadow = true;

    static const u32 formats[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };
    static const u32 cursor_formats[] = { DRM_FORMAT_ARGB8888 };

    for (u32 i = 0; i < vg->num_outputs; i++) {
        virtgpu_output_t *out = &vg->outputs[i];

        drm_crtc_t *crtc = drm_crtc_create(dev);
        if (!crtc) return -ENOMEM;
        drm_encoder_t *enc = drm_encoder_create(dev, DRM_MODE_ENCODER_VIRTUAL, 1U << crtc->index);
        drm_connector_t *conn = drm_connector_create(dev, DRM_MODE_CONNECTOR_VIRTUAL, enc);
        if (!enc || !conn) return -ENOMEM;

        u32 refresh = virtgpu_probe_refresh(out->scanout_id, out->width, out->height);

        drm_display_mode_t mode;
        drm_mode_simple(&mode, out->width, out->height, refresh);
        mode.type |= DRM_MODE_TYPE_PREFERRED;
        drm_connector_add_mode(conn, &mode);

        drm_plane_create(dev, DRM_PLANE_TYPE_PRIMARY, 1U << crtc->index,
                         formats, ARRAY_SIZE(formats));

        /* A cursor plane, so universal-plane and atomic clients see the
         * hardware cursor the legacy CURSOR ioctl drives through
         * .cursor_set/.cursor_move — scanout 0 only, see virtgpu_cursor_set. */
        if (out->scanout_id == 0) {
            drm_plane_create(dev, DRM_PLANE_TYPE_CURSOR, 1U << crtc->index,
                             cursor_formats, ARRAY_SIZE(cursor_formats));
        }

        crtc->mode       = mode;
        crtc->mode_valid = true;
        crtc->enabled    = true;
        out->crtc        = crtc;

        pr_debug("[VIRTIO-GPU-DRM] scanout %u: %ux%u@%uHz, resource %u backed at %p\n",
                 out->scanout_id, out->width, out->height, refresh,
                 out->resource_id, out->backing);
    }
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
    .date      = "20260912",
    .major     = 0, .minor = 2, .patchlevel = 0,
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

    /* This probe only ever runs when the PCI bus actually matched a
     * virtio-gpu device against virtgpu_pci_ids below, so this is where the
     * transport gets brought up too — one dynamic bring-up for the whole
     * device, transport and KMS together, instead of the old unconditional
     * call from kernel/main.c. virtio_gpu_init() is idempotent (refuses a
     * second call with -EBUSY), so a rescan or hotplug re-probe is safe. */
    if (!g_gpu.controlq) {
        if (virtio_gpu_init(dm->hal) < 0) {
            pr_debug("[VIRTIO-GPU-DRM] transport bring-up failed — declining\n");
            return -ENODEV;
        }
    }

    /* Create the host resource and its backing on first use. */
    if (g_gpu.framebuffer_phys == 0) {
        if (virtio_gpu_setup_framebuffer() < 0) return -ENODEV;
    }
    if (!g_gpu.framebuffer_virt) return -ENODEV;

    virtgpu_device_t *vg = (virtgpu_device_t *)kzalloc(sizeof(virtgpu_device_t));
    if (!vg) return -ENOMEM;

    /* Scanout 0 reuses the resource virtio_gpu_setup_framebuffer() already
     * stood up: fbdev.c and the hardware cursor both hold onto g_gpu's fields
     * directly, so this has to stay the same resource, not a new one. */
    vg->outputs[0].scanout_id  = 0;
    vg->outputs[0].width       = g_gpu.screen_width;
    vg->outputs[0].height      = g_gpu.screen_height;
    vg->outputs[0].pitch       = g_gpu.screen_width * 4;
    vg->outputs[0].backing     = g_gpu.framebuffer_virt;
    vg->outputs[0].resource_id = g_gpu.resource_id;
    vg->num_outputs            = 1;

    /* Bring up any further scanout the host reports as enabled — real
     * multi-monitor, each with its own resource and backing rather than a
     * mirror of scanout 0. QEMU's virtio-gpu-pci needs max_outputs > 1 (its
     * default is 1) for this to ever find one. */
    struct virtio_gpu_display_one modes[VIRTIO_GPU_MAX_SCANOUTS];
    u32 nmodes = 0;
    if (virtio_gpu_get_display_info_all(modes, VIRTIO_GPU_MAX_SCANOUTS, &nmodes) == 0) {
        for (u32 i = 1; i < nmodes && vg->num_outputs < VIRTIO_GPU_MAX_SCANOUTS; i++) {
            if (!modes[i].enabled) continue;
            if (modes[i].r.width == 0 || modes[i].r.height == 0) continue;

            u32 resource_id = 100 + i;   /* clear of resource 1 (scanout 0) and the 0xC0 cursor */
            phys_addr_t phys = 0;
            void *virt = NULL;
            u32 pitch = 0;
            if (virtio_gpu_setup_scanout_resource(i, resource_id, modes[i].r.width,
                                                  modes[i].r.height, &phys, &virt, &pitch) < 0) {
                pr_debug("[VIRTIO-GPU-DRM] scanout %u: setup failed, skipping\n", i);
                continue;
            }

            virtgpu_output_t *out = &vg->outputs[vg->num_outputs++];
            out->scanout_id  = i;
            out->width       = modes[i].r.width;
            out->height      = modes[i].r.height;
            out->pitch       = pitch;
            out->backing     = virt;
            out->resource_id = resource_id;
        }
    }

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
