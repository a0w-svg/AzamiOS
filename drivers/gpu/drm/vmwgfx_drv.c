/* ============================================================================
 * AzamiOS — vmwgfx: KMS driver for the VMware SVGA II adapter
 * File: drivers/gpu/drm/vmwgfx_drv.c
 *
 * The VMware SVGA II device (PCI 15AD:0405, QEMU's `-vga vmware`) is driven
 * through an index/value register pair in I/O space, a linear framebuffer in
 * BAR1, and a command FIFO in BAR2.  Unlike the Bochs adapter it does real
 * modesetting — width, height and depth are just registers — so this driver
 * exposes the whole standard mode ladder rather than one fixed mode.
 *
 * The device does not track CPU writes to the framebuffer, so a client's
 * pixels only reach the screen when the driver posts a SVGA_CMD_UPDATE
 * rectangle through the FIFO.  Buffer objects are therefore shadows in system
 * memory that page_flip()/dirty_fb() blit into VRAM and then flush, which is
 * also what makes the update rectangle meaningful.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "drm_core.h"
#include "../../base/pci_bus.h"
#include "../../../kernel/mm/kmalloc.h"
#include "../../../kernel/lib/string.h"
#include "../../../arch/x86_64/mm/vmm.h"

/* ── I/O port offsets from the BAR0 base ─────────────────────────────────── */
#define SVGA_INDEX_PORT          0
#define SVGA_VALUE_PORT          1

/* ── Registers ───────────────────────────────────────────────────────────── */
#define SVGA_REG_ID              0
#define SVGA_REG_ENABLE          1
#define SVGA_REG_WIDTH           2
#define SVGA_REG_HEIGHT          3
#define SVGA_REG_MAX_WIDTH       4
#define SVGA_REG_MAX_HEIGHT      5
#define SVGA_REG_DEPTH           6
#define SVGA_REG_BITS_PER_PIXEL  7
#define SVGA_REG_BYTES_PER_LINE  12
#define SVGA_REG_FB_START        13
#define SVGA_REG_FB_OFFSET       14
#define SVGA_REG_VRAM_SIZE       15
#define SVGA_REG_FB_SIZE         16
#define SVGA_REG_CAPABILITIES    17
#define SVGA_REG_MEM_START       18   /* FIFO base   */
#define SVGA_REG_MEM_SIZE        19   /* FIFO length */
#define SVGA_REG_CONFIG_DONE     20
#define SVGA_REG_SYNC            21
#define SVGA_REG_BUSY            22

/* Version handshake: write the newest ID the driver speaks and read it back. */
#define SVGA_MAGIC               0x900000UL
#define SVGA_MAKE_ID(ver)        ((u32)((SVGA_MAGIC << 8) | (ver)))
#define SVGA_ID_0                SVGA_MAKE_ID(0)
#define SVGA_ID_1                SVGA_MAKE_ID(1)
#define SVGA_ID_2                SVGA_MAKE_ID(2)

/* ── FIFO ────────────────────────────────────────────────────────────────── */
#define SVGA_FIFO_MIN            0    /* u32 indices into the FIFO mapping */
#define SVGA_FIFO_MAX            1
#define SVGA_FIFO_NEXT_CMD       2
#define SVGA_FIFO_STOP           3
#define SVGA_FIFO_NUM_REGS       4

#define SVGA_REG_CURSOR_ON       27
#define SVGA_REG_CURSOR_X        28
#define SVGA_REG_CURSOR_Y        29
#define SVGA_REG_CURSOR_ID       30

#define SVGA_CMD_UPDATE          1
#define SVGA_CMD_RECT_FILL       2
#define SVGA_CMD_RECT_COPY       3
#define SVGA_CMD_DEFINE_ALPHA_CURSOR 22

typedef struct vmwgfx_device {
    u16    io_base;
    void  *fb;                  /* mapped linear framebuffer   */
    size_t fb_size;
    volatile u32 *fifo;         /* mapped command FIFO         */
    size_t fifo_size;
    u32    width, height, bpp, pitch;
    u32    max_width, max_height;
    drm_crtc_t *crtc;
} vmwgfx_device_t;

/* ── Register access ─────────────────────────────────────────────────────── */

static void svga_write(vmwgfx_device_t *sv, u32 index, u32 value)
{
    outl(sv->io_base + SVGA_INDEX_PORT, index);
    outl(sv->io_base + SVGA_VALUE_PORT, value);
}

static u32 svga_read(vmwgfx_device_t *sv, u32 index)
{
    outl(sv->io_base + SVGA_INDEX_PORT, index);
    return inl(sv->io_base + SVGA_VALUE_PORT);
}

/* ── FIFO ────────────────────────────────────────────────────────────────── */

static void svga_fifo_init(vmwgfx_device_t *sv)
{
    /* The first four words are the FIFO's own registers, so the command area
     * starts immediately after them. */
    sv->fifo[SVGA_FIFO_MIN]      = SVGA_FIFO_NUM_REGS * sizeof(u32);
    sv->fifo[SVGA_FIFO_MAX]      = (u32)sv->fifo_size;
    sv->fifo[SVGA_FIFO_NEXT_CMD] = SVGA_FIFO_NUM_REGS * sizeof(u32);
    sv->fifo[SVGA_FIFO_STOP]     = SVGA_FIFO_NUM_REGS * sizeof(u32);
    svga_write(sv, SVGA_REG_CONFIG_DONE, 1);
}

/* Drain the FIFO and wait for the device to catch up. */
static void svga_sync(vmwgfx_device_t *sv)
{
    svga_write(sv, SVGA_REG_SYNC, 1);
    /* Bounded wait: an emulated adapter completes immediately, and a wedged
     * one must not hang the kernel. */
    for (u32 spins = 0; spins < 100000; spins++) {
        if (svga_read(sv, SVGA_REG_BUSY) == 0) return;
        cpu_pause();
    }
}

static void svga_fifo_write(vmwgfx_device_t *sv, u32 value)
{
    u32 min  = sv->fifo[SVGA_FIFO_MIN];
    u32 max  = sv->fifo[SVGA_FIFO_MAX];
    u32 next = sv->fifo[SVGA_FIFO_NEXT_CMD];

    u32 after = next + sizeof(u32);
    if (after == max) after = min;

    /* Full ring: let the device drain before overwriting unread commands. */
    if (after == sv->fifo[SVGA_FIFO_STOP]) {
        svga_sync(sv);
        if (after == sv->fifo[SVGA_FIFO_STOP]) return;
    }

    sv->fifo[next / sizeof(u32)]  = value;
    sv->fifo[SVGA_FIFO_NEXT_CMD]  = after;
}

static void svga_update_rect(vmwgfx_device_t *sv, u32 x, u32 y, u32 w, u32 h)
{
    svga_fifo_write(sv, SVGA_CMD_UPDATE);
    svga_fifo_write(sv, x);
    svga_fifo_write(sv, y);
    svga_fifo_write(sv, w);
    svga_fifo_write(sv, h);
    svga_sync(sv);
}

/* ── Scanout ─────────────────────────────────────────────────────────────── */

static int vmwgfx_flush(drm_crtc_t *crtc, drm_framebuffer_t *fb,
                        const drm_rect_t *clip)
{
    vmwgfx_device_t *sv = (vmwgfx_device_t *)crtc->dev->dev_private;
    if (!sv || !fb || !fb->obj || !sv->fb) return -EINVAL;

    drm_rect_t r = { 0, 0,
                     fb->width  < sv->width  ? fb->width  : sv->width,
                     fb->height < sv->height ? fb->height : sv->height };
    if (clip) {
        if (clip->x1 > r.x1) r.x1 = clip->x1;
        if (clip->y1 > r.y1) r.y1 = clip->y1;
        if (clip->x2 < r.x2) r.x2 = clip->x2;
        if (clip->y2 < r.y2) r.y2 = clip->y2;
        if (r.x1 >= r.x2 || r.y1 >= r.y2) return 0;
    }

    /* SVGA takes an update rectangle, so a damaged region costs the host one
     * small update instead of a full-screen one. */
    drm_gem_blit_rect(fb->obj, sv->fb, sv->pitch, &r, sv->bpp);
    svga_update_rect(sv, r.x1, r.y1, r.x2 - r.x1, r.y2 - r.y1);
    return 0;
}

static int vmwgfx_mode_set(drm_crtc_t *crtc, drm_framebuffer_t *fb,
                           const drm_display_mode_t *mode, u32 x, u32 y)
{
    (void)x; (void)y;
    vmwgfx_device_t *sv = (vmwgfx_device_t *)crtc->dev->dev_private;
    if (!mode) {
        svga_write(sv, SVGA_REG_ENABLE, 0);
        return 0;
    }
    if (mode->hdisplay > sv->max_width || mode->vdisplay > sv->max_height) {
        return -EINVAL;
    }

    svga_write(sv, SVGA_REG_ENABLE, 0);
    svga_write(sv, SVGA_REG_WIDTH,  mode->hdisplay);
    svga_write(sv, SVGA_REG_HEIGHT, mode->vdisplay);
    svga_write(sv, SVGA_REG_BITS_PER_PIXEL, 32);
    svga_write(sv, SVGA_REG_ENABLE, 1);

    /* Geometry is only authoritative after the device has accepted it. */
    sv->width  = svga_read(sv, SVGA_REG_WIDTH);
    sv->height = svga_read(sv, SVGA_REG_HEIGHT);
    sv->bpp    = svga_read(sv, SVGA_REG_BITS_PER_PIXEL);
    sv->pitch  = svga_read(sv, SVGA_REG_BYTES_PER_LINE);
    if (sv->pitch == 0) sv->pitch = sv->width * ((sv->bpp + 7) / 8);

    pr_debug("[VMWGFX] mode set to %ux%u %ubpp (pitch %u)\n",
             sv->width, sv->height, sv->bpp, sv->pitch);

    if (fb) return vmwgfx_flush(crtc, fb, NULL);
    return 0;
}

static int vmwgfx_load(drm_device_t *dev)
{
    vmwgfx_device_t *sv = (vmwgfx_device_t *)dev->dev_private;

    dev->min_width     = 640;
    dev->min_height    = 480;
    dev->max_width     = sv->max_width;
    dev->max_height    = sv->max_height;
    dev->cursor_width  = 64;
    dev->cursor_height = 64;
    dev->prefer_shadow = true;

    drm_crtc_t *crtc = drm_crtc_create(dev);
    if (!crtc) return -ENOMEM;
    drm_encoder_t *enc = drm_encoder_create(dev, DRM_MODE_ENCODER_VIRTUAL, 1U << crtc->index);
    drm_connector_t *conn = drm_connector_create(dev, DRM_MODE_CONNECTOR_VIRTUAL, enc);
    if (!enc || !conn) return -ENOMEM;

    drm_connector_add_default_modes(conn, sv->max_width, sv->max_height,
                                    sv->width, sv->height);

    static const u32 formats[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };
    drm_plane_create(dev, DRM_PLANE_TYPE_PRIMARY, 1U << crtc->index,
                     formats, ARRAY_SIZE(formats));

    drm_mode_simple(&crtc->mode, sv->width, sv->height, 60);
    crtc->mode_valid = true;
    crtc->enabled    = true;
    sv->crtc         = crtc;

    pr_debug("[VMWGFX] %ux%u %ubpp, VRAM %llu KiB, FIFO %llu KiB, max %ux%u\n",
             sv->width, sv->height, sv->bpp,
             (unsigned long long)(sv->fb_size / 1024),
             (unsigned long long)(sv->fifo_size / 1024),
             sv->max_width, sv->max_height);
    return 0;
}

static void vmwgfx_unload(drm_device_t *dev)
{
    vmwgfx_device_t *sv = (vmwgfx_device_t *)dev->dev_private;
    if (sv) {
        svga_write(sv, SVGA_REG_CONFIG_DONE, 0);
        svga_write(sv, SVGA_REG_ENABLE, 0);
        kfree(sv);
        dev->dev_private = NULL;
    }
}

static int vmwgfx_cursor_set(drm_crtc_t *crtc, drm_gem_object_t *bo, u32 w, u32 h)
{
    vmwgfx_device_t *sv = (vmwgfx_device_t *)crtc->dev->dev_private;
    if (!sv) return -EINVAL;

    if (!bo) {
        svga_write(sv, SVGA_REG_CURSOR_ON, 0);
        return 0;
    }

    if (w > 64 || h > 64) return -EINVAL;

    static u32 img[64 * 64];
    u32 cur_w = w ? w : 64;
    u32 cur_h = h ? h : 64;
    drm_rect_t all = { 0, 0, cur_w, cur_h };
    memset(img, 0, sizeof(img));
    drm_gem_blit_rect(bo, img, cur_w * 4, &all, 32);

    svga_fifo_write(sv, SVGA_CMD_DEFINE_ALPHA_CURSOR);
    svga_fifo_write(sv, 1); /* Cursor ID */
    svga_fifo_write(sv, (u32)crtc->cursor_hot_x);
    svga_fifo_write(sv, (u32)crtc->cursor_hot_y);
    svga_fifo_write(sv, cur_w);
    svga_fifo_write(sv, cur_h);
    for (u32 i = 0; i < cur_w * cur_h; i++) {
        svga_fifo_write(sv, img[i]);
    }
    svga_sync(sv);

    svga_write(sv, SVGA_REG_CURSOR_ID, 1);
    svga_write(sv, SVGA_REG_CURSOR_ON, 1);
    return 0;
}

static int vmwgfx_cursor_move(drm_crtc_t *crtc, s32 x, s32 y)
{
    vmwgfx_device_t *sv = (vmwgfx_device_t *)crtc->dev->dev_private;
    if (!sv) return -EINVAL;

    svga_write(sv, SVGA_REG_CURSOR_X, (u32)x);
    svga_write(sv, SVGA_REG_CURSOR_Y, (u32)y);
    return 0;
}

static const drm_driver_t vmwgfx_drm_driver = {
    .name        = "vmwgfx",
    .desc        = "VMware SVGA II display adapter",
    .date        = "20260831",
    .major       = 2, .minor = 0, .patchlevel = 0,
    .features    = DRIVER_MODESET | DRIVER_GEM | DRIVER_RENDER,
    .load        = vmwgfx_load,
    .unload      = vmwgfx_unload,
    .mode_set    = vmwgfx_mode_set,
    .page_flip   = vmwgfx_flush,
    .dirty_fb    = vmwgfx_flush,
    .cursor_set  = vmwgfx_cursor_set,
    .cursor_move = vmwgfx_cursor_move,
};

/* ── PCI binding ─────────────────────────────────────────────────────────── */

static int vmwgfx_pci_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;

    u16 io_base = (u16)pci_get_bar(dm->hal, 0);
    if (io_base == 0) return -ENODEV;

    vmwgfx_device_t *sv = (vmwgfx_device_t *)kzalloc(sizeof(vmwgfx_device_t));
    if (!sv) return -ENOMEM;
    sv->io_base = io_base;

    /* Negotiate the newest interface the device understands. */
    u32 version = 0;
    for (u32 want = SVGA_ID_2; ; want--) {
        svga_write(sv, SVGA_REG_ID, want);
        if (svga_read(sv, SVGA_REG_ID) == want) { version = want; break; }
        if (want == SVGA_ID_0) break;
    }
    if (version == 0) {
        pr_debug("[VMWGFX] no supported SVGA interface version — declining\n");
        kfree(sv);
        return -ENODEV;
    }

    sv->max_width  = svga_read(sv, SVGA_REG_MAX_WIDTH);
    sv->max_height = svga_read(sv, SVGA_REG_MAX_HEIGHT);
    if (sv->max_width == 0 || sv->max_height == 0) {
        sv->max_width  = 1920;
        sv->max_height = 1200;
    }

    phys_addr_t fb_phys = (phys_addr_t)svga_read(sv, SVGA_REG_FB_START);
    if (fb_phys == 0) fb_phys = pci_get_bar(dm->hal, 1);
    sv->fb_size = svga_read(sv, SVGA_REG_VRAM_SIZE);
    if (sv->fb_size == 0) sv->fb_size = svga_read(sv, SVGA_REG_FB_SIZE);

    phys_addr_t fifo_phys = (phys_addr_t)svga_read(sv, SVGA_REG_MEM_START);
    if (fifo_phys == 0) fifo_phys = pci_get_bar(dm->hal, 2);
    sv->fifo_size = svga_read(sv, SVGA_REG_MEM_SIZE);

    if (!fb_phys || !fifo_phys || sv->fb_size == 0 || sv->fifo_size == 0) {
        pr_debug("[VMWGFX] incomplete aperture description — declining\n");
        kfree(sv);
        return -ENODEV;
    }

    sv->fb   = vmm_map_io(fb_phys, sv->fb_size);
    sv->fifo = (volatile u32 *)vmm_map_io(fifo_phys, sv->fifo_size);
    if (!sv->fb || !sv->fifo) {
        kfree(sv);
        return -ENOMEM;
    }

    /* Come up in the mode the firmware left, or a sane default. */
    sv->width  = svga_read(sv, SVGA_REG_WIDTH);
    sv->height = svga_read(sv, SVGA_REG_HEIGHT);
    if (sv->width == 0 || sv->height == 0) { sv->width = 1024; sv->height = 768; }
    if (sv->width > sv->max_width)   sv->width  = sv->max_width;
    if (sv->height > sv->max_height) sv->height = sv->max_height;

    svga_write(sv, SVGA_REG_WIDTH,  sv->width);
    svga_write(sv, SVGA_REG_HEIGHT, sv->height);
    svga_write(sv, SVGA_REG_BITS_PER_PIXEL, 32);
    svga_fifo_init(sv);
    svga_write(sv, SVGA_REG_ENABLE, 1);

    sv->bpp   = svga_read(sv, SVGA_REG_BITS_PER_PIXEL);
    sv->pitch = svga_read(sv, SVGA_REG_BYTES_PER_LINE);
    if (sv->pitch == 0) sv->pitch = sv->width * ((sv->bpp + 7) / 8);

    drm_device_t *dev = drm_dev_alloc(&vmwgfx_drm_driver, dm);
    if (!dev) { kfree(sv); return -ENOMEM; }
    dev->dev_private = sv;

    int ret = drm_dev_register(dev);
    if (ret != 0) {
        kfree(sv);
        kfree(dev);
        return ret;
    }

    dm_set_drvdata(dm, dev);
    pr_debug("[VMWGFX] SVGA interface version %u accepted\n", version & 0xFF);
    return 0;
}

static void vmwgfx_pci_remove(dm_device_t *dm)
{
    drm_device_t *dev = (drm_device_t *)dm_get_drvdata(dm);
    if (dev) drm_dev_unregister(dev);
}

static const pci_device_id_t vmwgfx_pci_ids[] = {
    { PCI_DEVICE(0x15AD, 0x0405) },   /* VMware SVGA II */
    { 0 }
};

static pci_driver_t vmwgfx_pci_driver = {
    .drv      = { .name = "vmwgfx" },
    .id_table = vmwgfx_pci_ids,
    .probe    = vmwgfx_pci_probe,
    .remove   = vmwgfx_pci_remove,
};

void vmwgfx_drm_init(void)
{
    pci_driver_register(&vmwgfx_pci_driver);
}
