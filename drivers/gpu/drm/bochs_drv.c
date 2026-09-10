/* ============================================================================
 * AzamiOS — bochs-drm: KMS driver for the Bochs/QEMU VBE adapter
 * File: drivers/gpu/drm/bochs_drv.c
 *
 * The Bochs Graphics Adapter (QEMU's `-vga std`, PCI 1234:1111) exposes a
 * linear framebuffer twice the size of one screen, and a hardware Y-offset
 * register.  That is enough for genuine zero-copy page flipping: the two
 * halves of VRAM become two scanout slots, a dumb buffer that fits is
 * allocated directly in one of them, and a flip is a register write rather
 * than a copy.
 *
 * Buffers that do not fit a slot — anything past the second, or larger than
 * a screen — fall back to system memory and are blitted into the visible
 * slot on flip, which is what drm_gem_blit() and dirty_fb() are for.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "drm_core.h"
#include "../../base/pci_bus.h"
#include "../../misc/bga.h"
#include "../../../kernel/mm/kmalloc.h"
#include "../../../kernel/lib/string.h"

#define BOCHS_SCANOUT_SLOTS 2

typedef struct bochs_device {
    drm_device_t     *drm;
    phys_addr_t       vram_phys;
    u8               *vram;             /* mapped aperture, see bga.c   */
    size_t            vram_size;        /* video memory actually present */
    size_t            slot_size;        /* one screen at the live mode  */
    drm_gem_object_t *slot_obj[BOCHS_SCANOUT_SLOTS];
    u32               active_slot;
    drm_crtc_t       *crtc;
    drm_connector_t  *connector;
} bochs_device_t;

/* ── Scanout slot allocation ─────────────────────────────────────────────── */

static int bochs_gem_place(drm_device_t *dev, drm_gem_object_t *obj)
{
    bochs_device_t *bochs = (bochs_device_t *)dev->dev_private;
    if (!bochs || bochs->slot_size == 0) return -ENOSPC;
    if (obj->size > bochs->slot_size) return -ENOSPC;

    for (u32 slot = 0; slot < BOCHS_SCANOUT_SLOTS; slot++) {
        if (bochs->slot_obj[slot]) continue;
        u64 offset = (u64)slot * bochs->slot_size;
        if (offset + obj->size > bochs->vram_size) continue;

        for (size_t i = 0; i < obj->npages; i++) {
            obj->pages[i] = bochs->vram_phys + offset + i * PAGE_SIZE;
        }
        obj->in_vram         = true;
        obj->vram_offset     = offset;
        bochs->slot_obj[slot] = obj;
        return 0;
    }
    return -ENOSPC;
}

static void bochs_gem_release(drm_device_t *dev, drm_gem_object_t *obj)
{
    bochs_device_t *bochs = (bochs_device_t *)dev->dev_private;
    if (!bochs || !obj->in_vram) return;

    for (u32 slot = 0; slot < BOCHS_SCANOUT_SLOTS; slot++) {
        if (bochs->slot_obj[slot] == obj) bochs->slot_obj[slot] = NULL;
    }
}

/* ── Scanout ─────────────────────────────────────────────────────────────── */

/* Copy a system-memory framebuffer into the slot currently being scanned out. */
static int bochs_blit_shadow(bochs_device_t *bochs, drm_framebuffer_t *fb,
                             const drm_rect_t *clip)
{
    if (!fb || !fb->obj || !bochs->vram) return -EINVAL;

    u32 dst_pitch = bga_get_pitch();
    if (dst_pitch == 0) return -EINVAL;

    u64 base = (u64)bochs->active_slot * bochs->slot_size;
    if (base >= bochs->vram_size) return -EINVAL;

    /*
     * The aperture is finite and the client's framebuffer need not match the
     * live mode, so the copy is clipped to whichever is smaller — the visible
     * scanout, the source, or the memory left in the slot.
     */
    u32 w = fb->width  < bga_get_width()  ? fb->width  : bga_get_width();
    u32 h = fb->height < bga_get_height() ? fb->height : bga_get_height();

    u32 rows_left = (u32)((bochs->vram_size - base) / dst_pitch);
    if (h > rows_left) h = rows_left;
    if (w == 0 || h == 0) return 0;

    /* Then narrow again to the region the client says it touched. */
    drm_rect_t r = { 0, 0, w, h };
    if (clip) {
        if (clip->x1 > r.x1) r.x1 = clip->x1;
        if (clip->y1 > r.y1) r.y1 = clip->y1;
        if (clip->x2 < r.x2) r.x2 = clip->x2;
        if (clip->y2 < r.y2) r.y2 = clip->y2;
        if (r.x1 >= r.x2 || r.y1 >= r.y2) return 0;
    }

    drm_gem_blit_rect(fb->obj, bochs->vram + base, dst_pitch, &r, fb->bpp);
    return 0;
}

static int bochs_page_flip(drm_crtc_t *crtc, drm_framebuffer_t *fb,
                           const drm_rect_t *clip)
{
    bochs_device_t *bochs = (bochs_device_t *)crtc->dev->dev_private;
    if (!fb || !fb->obj) return -EINVAL;

    /* Look the slot up rather than deriving it from the offset: a mode change
     * resizes the slots, and the ownership table stays correct across one.
     * This is a true flip — one register write, no copy, nothing torn — so
     * the damage rectangle is irrelevant. */
    if (fb->obj->in_vram) {
        for (u32 slot = 0; slot < BOCHS_SCANOUT_SLOTS; slot++) {
            if (bochs->slot_obj[slot] != fb->obj) continue;
            if (bga_flip_buffer(slot) != 0) return -EIO;
            bochs->active_slot = slot;
            return 0;
        }
    }
    return bochs_blit_shadow(bochs, fb, clip);
}

static int bochs_dirty_fb(drm_crtc_t *crtc, drm_framebuffer_t *fb,
                          const drm_rect_t *clip)
{
    bochs_device_t *bochs = (bochs_device_t *)crtc->dev->dev_private;
    /* A framebuffer that is already the scanout needs no flush. */
    if (fb && fb->obj && fb->obj->in_vram) return 0;
    return bochs_blit_shadow(bochs, fb, clip);
}

static int bochs_mode_set(drm_crtc_t *crtc, drm_framebuffer_t *fb,
                          const drm_display_mode_t *mode, u32 x, u32 y)
{
    (void)x; (void)y;
    bochs_device_t *bochs = (bochs_device_t *)crtc->dev->dev_private;
    if (!mode) return 0;   /* blanking: leave the adapter programmed */

    bga_set_video_mode(mode->hdisplay, mode->vdisplay, 32, 1);

    /* The mode determines how much VRAM one screen costs, and therefore how
     * the scanout slots are laid out.  Buffers placed under the previous mode
     * keep their pages and their slot, but a client that changes mode is
     * expected to re-create them, as it would on any KMS driver. */
    bochs->slot_size   = bga_get_fb_size();
    bochs->active_slot = 0;

    if (fb) return bochs_page_flip(crtc, fb, NULL);
    return 0;
}

/* ── Driver ──────────────────────────────────────────────────────────────── */

static int bochs_load(drm_device_t *dev)
{
    bochs_device_t *bochs = (bochs_device_t *)kzalloc(sizeof(bochs_device_t));
    if (!bochs) return -ENOMEM;

    bochs->drm       = dev;
    bochs->vram_phys = bga_get_fb_phys();
    bochs->vram      = (u8 *)bga_get_fb_virt();
    bochs->vram_size = bga_get_vram_size();
    bochs->slot_size = bga_get_fb_size();
    dev->dev_private = bochs;

    if (!bochs->vram || bochs->vram_size == 0) {
        kfree(bochs);
        dev->dev_private = NULL;
        return -ENODEV;
    }

    u32 w = bga_get_width()  ? bga_get_width()  : 1280;
    u32 h = bga_get_height() ? bga_get_height() : 800;

    /* The adapter has no hardware cursor, so no cursor plane is advertised
     * and clients composite their own pointer. */
    dev->cursor_width  = 0;
    dev->cursor_height = 0;
    dev->prefer_shadow = false;
    dev->max_width     = 1920;
    dev->max_height    = 1200;

    drm_crtc_t    *crtc = drm_crtc_create(dev);
    if (!crtc) return -ENOMEM;
    drm_encoder_t *enc  = drm_encoder_create(dev, DRM_MODE_ENCODER_DAC, 1U << crtc->index);
    drm_connector_t *conn = drm_connector_create(dev, DRM_MODE_CONNECTOR_VGA, enc);
    if (!enc || !conn) return -ENOMEM;

    drm_connector_add_default_modes(conn, dev->max_width, dev->max_height, w, h);

    static const u32 formats[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };
    drm_plane_create(dev, DRM_PLANE_TYPE_PRIMARY, 1U << crtc->index,
                     formats, ARRAY_SIZE(formats));

    /* Report the mode the firmware left us in, so a client that never calls
     * SETCRTC still sees a sane CRTC state. */
    drm_mode_simple(&crtc->mode, w, h, 60);
    crtc->mode_valid = true;
    crtc->enabled    = true;

    bochs->crtc      = crtc;
    bochs->connector = conn;

    u32 slots = 0;
    while (slots < BOCHS_SCANOUT_SLOTS && bochs->slot_size &&
           (u64)(slots + 1) * bochs->slot_size <= bochs->vram_size) {
        slots++;
    }

    pr_debug("[BOCHS-DRM] %ux%u, VRAM 0x%016llx (%llu KiB, %u scanout slot%s)\n",
             w, h, (unsigned long long)bochs->vram_phys,
             (unsigned long long)(bochs->vram_size / 1024),
             slots, slots == 1 ? "" : "s");
    return 0;
}

static void bochs_unload(drm_device_t *dev)
{
    if (dev->dev_private) {
        kfree(dev->dev_private);
        dev->dev_private = NULL;
    }
}

static const drm_driver_t bochs_drm_driver = {
    .name        = "bochs-drm",
    .desc        = "Bochs/QEMU VBE display adapter",
    .date        = "20260831",
    .major       = 1, .minor = 0, .patchlevel = 0,
    .features    = DRIVER_MODESET | DRIVER_GEM | DRIVER_RENDER,
    .load        = bochs_load,
    .unload      = bochs_unload,
    .gem_place   = bochs_gem_place,
    .gem_release = bochs_gem_release,
    .mode_set    = bochs_mode_set,
    .page_flip   = bochs_page_flip,
    .dirty_fb    = bochs_dirty_fb,
};

/* ── PCI binding ─────────────────────────────────────────────────────────── */

static int bochs_pci_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;

    /* The BGA driver programs the adapter; without it there is no linear
     * framebuffer to scan out from. */
    if (bga_get_fb_phys() == 0) {
        pr_debug("[BOCHS-DRM] BGA framebuffer not initialised — declining\n");
        return -ENODEV;
    }

    drm_device_t *dev = drm_dev_alloc(&bochs_drm_driver, dm);
    if (!dev) return -ENOMEM;

    int ret = drm_dev_register(dev);
    if (ret != 0) {
        kfree(dev);
        return ret;
    }

    dm_set_drvdata(dm, dev);
    return 0;
}

static void bochs_pci_remove(dm_device_t *dm)
{
    drm_device_t *dev = (drm_device_t *)dm_get_drvdata(dm);
    if (dev) drm_dev_unregister(dev);
}

static const pci_device_id_t bochs_pci_ids[] = {
    { PCI_DEVICE(0x1234, 0x1111) },   /* QEMU standard VGA        */
    { PCI_DEVICE(0x80EE, 0xBEEF) },   /* VirtualBox VBE adapter   */
    { 0 }
};

static pci_driver_t bochs_pci_driver = {
    .drv      = { .name = "bochs-drm" },
    .id_table = bochs_pci_ids,
    .probe    = bochs_pci_probe,
    .remove   = bochs_pci_remove,
};

void bochs_drm_init(void)
{
    pci_driver_register(&bochs_pci_driver);
}
