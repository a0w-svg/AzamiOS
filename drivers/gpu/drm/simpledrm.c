/* ============================================================================
 * AzamiOS — simpledrm: KMS driver for the bootloader's framebuffer
 * File: drivers/gpu/drm/simpledrm.c
 *
 * The fallback that always works.  Limine hands the kernel a linear
 * framebuffer already in a working mode; there is no way to reprogram it and
 * no second buffer to flip to, so this driver exposes exactly one CRTC with
 * exactly one mode and treats every buffer object as a shadow that is
 * blitted to the display on flip or dirty — the same design as Linux's
 * simpledrm.
 *
 * It binds to the "simple-framebuffer" platform device, so it only claims
 * the display when no real GPU driver has.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "drm_core.h"
#include "../../base/platform.h"
#include "../../../arch/x86_64/boot/limine_req.h"
#include "../../../kernel/mm/kmalloc.h"
#include "../../../kernel/lib/string.h"

typedef struct simpledrm_device {
    void  *vram;            /* HHDM pointer to the framebuffer */
    size_t vram_size;
    u32    width, height, pitch, bpp;
    drm_crtc_t *crtc;
} simpledrm_device_t;

static int simpledrm_blit(drm_crtc_t *crtc, drm_framebuffer_t *fb,
                          const drm_rect_t *clip)
{
    simpledrm_device_t *sdev = (simpledrm_device_t *)crtc->dev->dev_private;
    if (!sdev || !fb || !fb->obj) return -EINVAL;

    drm_rect_t r = { 0, 0,
                     fb->width  < sdev->width  ? fb->width  : sdev->width,
                     fb->height < sdev->height ? fb->height : sdev->height };
    if (clip) {
        if (clip->x1 > r.x1) r.x1 = clip->x1;
        if (clip->y1 > r.y1) r.y1 = clip->y1;
        if (clip->x2 < r.x2) r.x2 = clip->x2;
        if (clip->y2 < r.y2) r.y2 = clip->y2;
        if (r.x1 >= r.x2 || r.y1 >= r.y2) return 0;
    }

    drm_gem_blit_rect(fb->obj, sdev->vram, sdev->pitch, &r, sdev->bpp);
    return 0;
}

static int simpledrm_mode_set(drm_crtc_t *crtc, drm_framebuffer_t *fb,
                              const drm_display_mode_t *mode, u32 x, u32 y)
{
    (void)x; (void)y;
    simpledrm_device_t *sdev = (simpledrm_device_t *)crtc->dev->dev_private;
    if (!mode) return 0;

    /* The bootloader's mode is the only one the hardware is in. */
    if (mode->hdisplay != sdev->width || mode->vdisplay != sdev->height) {
        return -EINVAL;
    }
    if (fb) return simpledrm_blit(crtc, fb, NULL);
    return 0;
}

static int simpledrm_load(drm_device_t *dev)
{
    simpledrm_device_t *sdev = (simpledrm_device_t *)dev->dev_private;

    dev->min_width     = sdev->width;
    dev->max_width     = sdev->width;
    dev->min_height    = sdev->height;
    dev->max_height    = sdev->height;
    dev->cursor_width  = 0;
    dev->cursor_height = 0;
    /* Every buffer lives in system memory and is copied to the display, which
     * is precisely what DUMB_PREFER_SHADOW tells clients. */
    dev->prefer_shadow = true;

    drm_crtc_t *crtc = drm_crtc_create(dev);
    if (!crtc) return -ENOMEM;
    drm_encoder_t *enc = drm_encoder_create(dev, DRM_MODE_ENCODER_NONE, 1U << crtc->index);
    drm_connector_t *conn = drm_connector_create(dev, DRM_MODE_CONNECTOR_Unknown, enc);
    if (!enc || !conn) return -ENOMEM;

    drm_display_mode_t mode;
    drm_mode_simple(&mode, sdev->width, sdev->height, 60);
    mode.type |= DRM_MODE_TYPE_PREFERRED;
    drm_connector_add_mode(conn, &mode);

    static const u32 formats[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };
    drm_plane_create(dev, DRM_PLANE_TYPE_PRIMARY, 1U << crtc->index,
                     formats, ARRAY_SIZE(formats));

    crtc->mode       = mode;
    crtc->mode_valid = true;
    crtc->enabled    = true;
    sdev->crtc       = crtc;

    pr_debug("[SIMPLEDRM] %ux%u %ubpp shadow-buffered at %p\n",
             sdev->width, sdev->height, sdev->bpp, sdev->vram);
    return 0;
}

static void simpledrm_unload(drm_device_t *dev)
{
    if (dev->dev_private) {
        kfree(dev->dev_private);
        dev->dev_private = NULL;
    }
}

static const drm_driver_t simpledrm_driver = {
    .name      = "simpledrm",
    .desc      = "Bootloader-provided framebuffer",
    .date      = "20260831",
    .major     = 1, .minor = 0, .patchlevel = 0,
    .features  = DRIVER_MODESET | DRIVER_GEM | DRIVER_RENDER,
    .load      = simpledrm_load,
    .unload    = simpledrm_unload,
    .mode_set  = simpledrm_mode_set,
    .page_flip = simpledrm_blit,
    .dirty_fb  = simpledrm_blit,
};

/* ── Platform binding ────────────────────────────────────────────────────── */

static int simpledrm_probe(platform_device_t *pdev)
{
    /* Another driver already owns the display; nothing to fall back to. */
    if (drm_dev_count() > 0) return -EBUSY;

    const platform_resource_t *res = platform_get_resource(pdev, PLATFORM_RES_MEM, 0);
    struct limine_framebuffer *lfb = (struct limine_framebuffer *)pdev->pdata;
    if (!res || !lfb) return -ENODEV;

    simpledrm_device_t *sdev = (simpledrm_device_t *)kzalloc(sizeof(simpledrm_device_t));
    if (!sdev) return -ENOMEM;

    sdev->vram      = PHYS_TO_VIRT((phys_addr_t)res->start);
    sdev->vram_size = (size_t)res->size;
    sdev->width     = (u32)lfb->width;
    sdev->height    = (u32)lfb->height;
    sdev->pitch     = (u32)lfb->pitch;
    sdev->bpp       = (u32)lfb->bpp;

    drm_device_t *dev = drm_dev_alloc(&simpledrm_driver, pdev->dev);
    if (!dev) { kfree(sdev); return -ENOMEM; }
    dev->dev_private = sdev;

    int ret = drm_dev_register(dev);
    if (ret != 0) {
        kfree(sdev);
        kfree(dev);
        return ret;
    }

    dm_set_drvdata(pdev->dev, dev);
    return 0;
}

static void simpledrm_remove(platform_device_t *pdev)
{
    drm_device_t *dev = (drm_device_t *)dm_get_drvdata(pdev->dev);
    if (dev) drm_dev_unregister(dev);
}

static platform_driver_t simpledrm_platform_driver = {
    .drv    = { .name = "simple-framebuffer" },
    .probe  = simpledrm_probe,
    .remove = simpledrm_remove,
};

void simpledrm_init(void)
{
    platform_driver_register(&simpledrm_platform_driver);
}
