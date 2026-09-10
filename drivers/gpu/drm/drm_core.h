/* ============================================================================
 * AzamiOS — Direct Rendering Manager: core object model
 * File: drivers/gpu/drm/drm_core.h
 *
 * A Linux-shaped DRM core.  The core owns everything that is the same for
 * every GPU — the mode-object ID space, KMS object lists, GEM buffer objects
 * and handle namespaces, the /dev/dri nodes and the whole ioctl surface —
 * and calls down into a drm_driver_t for the handful of operations that are
 * genuinely hardware-specific:
 *
 *        userspace ── ioctl ──▶ drm_ioctl.c
 *                                   │  looks up mode objects, GEM handles
 *                                   ▼
 *                          drm_mode.c / drm_gem.c
 *                                   │  mode_set / page_flip / cursor / dirty
 *                                   ▼
 *                    bochs · simpledrm · virtio-gpu  (drm_driver_t)
 *
 * Adding a GPU driver means filling in a drm_driver_t and calling
 * drm_dev_register() — no ioctl code, no ID bookkeeping, no /dev plumbing.
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"
#include "../../../include/azami/defs.h"
#include "../../../include/azami/drm.h"
#include "../../../arch/x86_64/cpu/spinlock.h"
#include "../../base/base.h"

struct drm_device;
struct drm_file;
struct drm_crtc;

/* ── Limits ──────────────────────────────────────────────────────────────── */
#define DRM_MAX_CARDS            4
#define DRM_MAX_HANDLES         64     /* GEM handles per open file          */
#define DRM_MAX_MODES           16     /* probed modes per connector         */
#define DRM_MAX_FORMATS          8     /* pixel formats per plane            */
#define DRM_MAX_EVENTS          16     /* queued flip/vblank events per file */

/* ── Driver feature bits ─────────────────────────────────────────────────── */
#define DRIVER_MODESET          (1U << 0)
#define DRIVER_GEM              (1U << 1)
#define DRIVER_RENDER           (1U << 2)
#define DRIVER_ATOMIC           (1U << 3)

/* ── Mode object types (Linux ABI values) ────────────────────────────────── */
#define DRM_MODE_OBJECT_CRTC       0xCCCCCCCCU
#define DRM_MODE_OBJECT_CONNECTOR  0xC0C0C0C0U
#define DRM_MODE_OBJECT_ENCODER    0xE0E0E0E0U
#define DRM_MODE_OBJECT_MODE       0xDEDEDEDEU
#define DRM_MODE_OBJECT_PLANE      0xEEEEEEEEU
#define DRM_MODE_OBJECT_FB         0xFBFBFBFBU
#define DRM_MODE_OBJECT_ANY        0U

/* Every KMS object starts with one of these; IDs are unique per card. */
typedef struct drm_mode_object {
    u32 id;
    u32 type;
} drm_mode_object_t;

/* Kernel-side display mode — same fields as the ABI struct, kept separate so
 * the core never hands a userspace layout to a driver. */
typedef struct drm_display_mode {
    u32  clock;
    u16  hdisplay, hsync_start, hsync_end, htotal, hskew;
    u16  vdisplay, vsync_start, vsync_end, vtotal, vscan;
    u32  vrefresh;
    u32  flags;
    u32  type;
    char name[32];
} drm_display_mode_t;

/* A damage rectangle in framebuffer pixels; x2/y2 are exclusive.  NULL where
 * a clip is expected always means "the whole framebuffer". */
typedef struct drm_rect {
    u32 x1, y1, x2, y2;
} drm_rect_t;

/* ── GEM: a buffer object ────────────────────────────────────────────────── */
typedef struct drm_gem_object {
    struct drm_device *dev;
    size_t             size;            /* byte size, page-aligned           */
    u32                width, height, pitch, bpp;

    phys_addr_t       *pages;           /* backing pages, one per PAGE_SIZE  */
    size_t             npages;
    bool               in_vram;         /* pages point into the scanout      */
    u64                vram_offset;     /* byte offset within VRAM if so     */

    u64                mmap_offset;     /* fake offset handed to mmap()      */
    u32                name;            /* GEM flink name, 0 if unnamed      */
    int                refcount;
    struct drm_gem_object *next;
} drm_gem_object_t;

/* ── KMS objects ─────────────────────────────────────────────────────────── */
typedef struct drm_framebuffer {
    drm_mode_object_t  base;
    struct drm_device *dev;
    u32                width, height, pitch, bpp, depth, pixel_format;
    drm_gem_object_t  *obj;
    int                refcount;
    struct drm_framebuffer *next;
} drm_framebuffer_t;

typedef struct drm_plane {
    drm_mode_object_t  base;
    struct drm_device *dev;
    u32                plane_type;      /* DRM_PLANE_TYPE_*                  */
    u32                possible_crtcs;
    u32                formats[DRM_MAX_FORMATS];
    u32                nformats;
    struct drm_crtc   *crtc;
    drm_framebuffer_t *fb;
    s32                crtc_x, crtc_y;
    u32                crtc_w, crtc_h;
    struct drm_plane  *next;
} drm_plane_t;

typedef struct drm_crtc {
    drm_mode_object_t  base;
    struct drm_device *dev;
    u32                index;           /* 0-based, for possible_crtcs masks */
    drm_framebuffer_t *fb;
    drm_display_mode_t mode;
    bool               mode_valid;
    bool               enabled;
    u32                x, y;

    drm_plane_t       *primary;
    drm_plane_t       *cursor;
    drm_gem_object_t  *cursor_bo;
    s32                cursor_x, cursor_y;
    s32                cursor_hot_x, cursor_hot_y;
    bool               cursor_visible;

    /* ── Vblank clock (drm_vblank.c) ────────────────────────────────────
     * The emulated adapters here have no scanout interrupt, so vblank is a
     * software clock: a period derived from the mode, a monotonic timestamp
     * of the last edge, and a 64-bit sequence.  Everything that has to look
     * atomic on screen is timed against it. */
    u64                vblank_period_ns;
    u64                last_vblank_ns;
    u64                vblank_count;

    /* ── Flip queued for the next vblank ────────────────────────────────
     * A page flip takes a reference on the framebuffer and returns; the
     * vblank worker is what actually swaps the scanout, so a client never
     * sees half of one frame and half of the next. */
    drm_framebuffer_t *flip_fb;
    struct drm_file   *flip_file;
    u64                flip_user_data;
    bool               flip_pending;
    bool               flip_event;

    /* ── Damage accumulated since the last flush ────────────────────────
     * Shadow-buffered drivers copy only these rows, so a compositor that
     * reports its damage pays for what it changed and nothing more. */
    drm_rect_t         damage;
    bool               damage_valid;

    struct drm_crtc   *next;
} drm_crtc_t;

typedef struct drm_encoder {
    drm_mode_object_t  base;
    struct drm_device *dev;
    u32                encoder_type;
    u32                possible_crtcs;
    u32                possible_clones;
    drm_crtc_t        *crtc;
    struct drm_encoder *next;
} drm_encoder_t;

typedef struct drm_connector {
    drm_mode_object_t  base;
    struct drm_device *dev;
    u32                connector_type;
    u32                connector_type_id;
    u32                status;          /* DRM_MODE_CONNECTED, …             */
    u32                mm_width, mm_height;
    u32                subpixel;
    drm_encoder_t     *encoder;
    drm_display_mode_t modes[DRM_MAX_MODES];
    u32                nmodes;
    struct drm_connector *next;
} drm_connector_t;

/* ── Per-open state ──────────────────────────────────────────────────────── */
typedef struct drm_pending_event {
    struct drm_event_vblank ev;
    bool                    valid;
} drm_pending_event_t;

typedef struct drm_file {
    struct drm_device *dev;
    bool               is_master;
    bool               is_render_node;
    bool               authenticated;
    u32                magic;

    /* Per-file GEM handle namespace: handle 0 is never valid, as in Linux. */
    drm_gem_object_t  *handles[DRM_MAX_HANDLES];
    u32                next_handle;

    bool               universal_planes;
    bool               atomic;

    drm_pending_event_t events[DRM_MAX_EVENTS];
    u32                event_head, event_tail;
} drm_file_t;

/* ── Driver operations ───────────────────────────────────────────────────── */
typedef struct drm_driver {
    const char *name;
    const char *desc;
    const char *date;
    int         major, minor, patchlevel;
    u32         features;

    /** Bring the hardware up; fill in mode config and KMS objects. */
    int  (*load)(struct drm_device *dev);
    void (*unload)(struct drm_device *dev);

    /**
     * Place a new buffer object.  A driver that can scan out directly from
     * VRAM sets obj->in_vram and fills obj->pages itself; returning -ENOSPC
     * (or leaving the hook NULL) makes the core allocate system memory and
     * treat the buffer as a shadow that page_flip() blits from.
     */
    int  (*gem_place)(struct drm_device *dev, drm_gem_object_t *obj);
    void (*gem_release)(struct drm_device *dev, drm_gem_object_t *obj);

    /** Program a mode and scan out @fb.  @fb may be NULL to blank. */
    int  (*mode_set)(drm_crtc_t *crtc, drm_framebuffer_t *fb,
                     const drm_display_mode_t *mode, u32 x, u32 y);

    /**
     * Switch scanout to @fb.  Called from the vblank worker for page flips
     * and directly for plane updates.  @clip is the region known to have
     * changed, or NULL for the whole framebuffer; a driver that flips by
     * reprogramming a scanout address ignores it, a driver that copies uses
     * it to copy less.
     */
    int  (*page_flip)(drm_crtc_t *crtc, drm_framebuffer_t *fb,
                      const drm_rect_t *clip);

    /** Push a shadow-buffer framebuffer to the display, clipped to @clip. */
    int  (*dirty_fb)(drm_crtc_t *crtc, drm_framebuffer_t *fb,
                     const drm_rect_t *clip);

    int  (*cursor_set)(drm_crtc_t *crtc, drm_gem_object_t *bo, u32 w, u32 h);
    int  (*cursor_move)(drm_crtc_t *crtc, s32 x, s32 y);
} drm_driver_t;

/* ── The card ────────────────────────────────────────────────────────────── */
typedef struct drm_device {
    const drm_driver_t *driver;
    void               *dev_private;
    dm_device_t        *dm;              /* driver-model device, may be NULL */
    int                 index;           /* N in cardN                       */
    char                unique[32];      /* bus id, e.g. "pci:0000:00:02.0"  */

    /* Mode configuration limits, reported by GETRESOURCES. */
    u32 min_width, max_width, min_height, max_height;
    u32 cursor_width, cursor_height;
    bool prefer_shadow;

    drm_crtc_t        *crtc_list;
    drm_encoder_t     *encoder_list;
    drm_connector_t   *connector_list;
    drm_plane_t       *plane_list;
    drm_framebuffer_t *fb_list;
    drm_gem_object_t  *gem_list;
    u32 num_crtc, num_encoder, num_connector, num_plane, num_fb;

    u32                next_object_id;
    u32                next_gem_name;
    u64                next_mmap_offset;
    drm_file_t        *master;
    u64                vblank_count;

    spinlock_t         lock;
    struct drm_device *next;
} drm_device_t;

/* ── Core API (drm_drv.c) ────────────────────────────────────────────────── */

/** drm_dev_alloc(driver, dm) → a card, not yet visible to userspace. */
drm_device_t *drm_dev_alloc(const drm_driver_t *driver, dm_device_t *dm);

/**
 * drm_dev_register(dev) — run driver->load(), then publish the card.
 *
 * Creates /dev/dri/cardN (plus /dev/dri/renderD(128+N) when the driver sets
 * DRIVER_RENDER) and joins the "drm" class so it appears under /sys/class/drm.
 */
int  drm_dev_register(drm_device_t *dev);
void drm_dev_unregister(drm_device_t *dev);

/** drm_dev_nth(n) / drm_dev_count() — enumerate registered cards. */
drm_device_t *drm_dev_nth(u32 n);
u32           drm_dev_count(void);

/** drm_class() — the "drm" device class, for drivers that add sub-devices. */
dm_class_t *drm_class(void);

/**
 * drm_send_event() — queue one event record on @file.
 *
 * @type is DRM_EVENT_FLIP_COMPLETE or DRM_EVENT_VBLANK.  Delivery is to the
 * file that asked for the event, as in Linux, not to whoever happens to be
 * master, and any thread blocked in read() on that file is woken.
 */
void drm_send_event(drm_file_t *file, drm_crtc_t *crtc, u32 type,
                    u64 user_data, u64 timestamp_ns, u64 sequence);

/* ── Mode object API (drm_mode.c) ────────────────────────────────────────── */
u32 drm_mode_object_add(drm_device_t *dev, drm_mode_object_t *obj, u32 type);
drm_mode_object_t *drm_mode_object_find(drm_device_t *dev, u32 id, u32 type);

drm_crtc_t      *drm_crtc_create(drm_device_t *dev);
drm_encoder_t   *drm_encoder_create(drm_device_t *dev, u32 type, u32 possible_crtcs);
drm_connector_t *drm_connector_create(drm_device_t *dev, u32 type, drm_encoder_t *enc);
drm_plane_t     *drm_plane_create(drm_device_t *dev, u32 plane_type,
                                  u32 possible_crtcs,
                                  const u32 *formats, u32 nformats);

drm_crtc_t      *drm_crtc_find(drm_device_t *dev, u32 id);
drm_encoder_t   *drm_encoder_find(drm_device_t *dev, u32 id);
drm_connector_t *drm_connector_find(drm_device_t *dev, u32 id);
drm_plane_t     *drm_plane_find(drm_device_t *dev, u32 id);

/** drm_connector_add_mode() — append a probed mode to a connector. */
int drm_connector_add_mode(drm_connector_t *conn, const drm_display_mode_t *mode);

/**
 * drm_connector_add_default_modes() — add the CVT-ish standard mode list,
 * capped to @max_w x @max_h, marking @pref_w x @pref_h as preferred.
 */
int drm_connector_add_default_modes(drm_connector_t *conn,
                                    u32 max_w, u32 max_h,
                                    u32 pref_w, u32 pref_h);

/** drm_mode_simple() — build a single fixed mode (used by simpledrm). */
void drm_mode_simple(drm_display_mode_t *mode, u32 w, u32 h, u32 refresh);

/* ── Framebuffer API (drm_mode.c) ────────────────────────────────────────── */
drm_framebuffer_t *drm_framebuffer_create(drm_device_t *dev, drm_gem_object_t *obj,
                                          u32 width, u32 height, u32 pitch,
                                          u32 bpp, u32 depth, u32 pixel_format);
drm_framebuffer_t *drm_framebuffer_find(drm_device_t *dev, u32 id);
void drm_framebuffer_put(drm_device_t *dev, drm_framebuffer_t *fb);

/* ── GEM API (drm_gem.c) ─────────────────────────────────────────────────── */
drm_gem_object_t *drm_gem_object_create(drm_device_t *dev, u32 width, u32 height,
                                        u32 bpp, u32 pitch);
void  drm_gem_object_get(drm_gem_object_t *obj);
void  drm_gem_object_put(drm_device_t *dev, drm_gem_object_t *obj);

u32   drm_gem_handle_create(drm_file_t *file, drm_gem_object_t *obj);
drm_gem_object_t *drm_gem_handle_lookup(drm_file_t *file, u32 handle);
int   drm_gem_handle_delete(drm_file_t *file, u32 handle);
void  drm_gem_release_all(drm_file_t *file);

drm_gem_object_t *drm_gem_object_by_mmap_offset(drm_device_t *dev, u64 offset);
drm_gem_object_t *drm_gem_object_by_name(drm_device_t *dev, u32 name);
u32   drm_gem_object_flink(drm_device_t *dev, drm_gem_object_t *obj);

/** drm_gem_kmap(obj) → HHDM pointer to the first page, for kernel-side blits. */
void *drm_gem_page_ptr(drm_gem_object_t *obj, size_t page_index);

/** drm_gem_blit() — blit a whole shadow buffer into a VRAM scanout window. */
void drm_gem_blit(drm_gem_object_t *src, void *dst_virt, u32 dst_pitch,
                  u32 width, u32 height, u32 bpp);

/** drm_gem_blit_rect() — the same, restricted to one damage rectangle. */
void drm_gem_blit_rect(drm_gem_object_t *src, void *dst_virt, u32 dst_pitch,
                       const drm_rect_t *clip, u32 bpp);

/* ── PRIME buffer sharing (drm_drv.c) ────────────────────────────────────── */

/** drm_prime_export_fd() → a dma-buf file descriptor for @obj, or negative. */
int drm_prime_export_fd(drm_device_t *dev, drm_gem_object_t *obj);

/** drm_prime_import_fd() → the object behind a dma-buf fd, or NULL. */
drm_gem_object_t *drm_prime_import_fd(drm_device_t *dev, int fd);

/* ── ioctl dispatch (drm_ioctl.c) ────────────────────────────────────────── */
s64 drm_ioctl_dispatch(drm_device_t *dev, drm_file_t *file, u32 cmd, u64 arg);

/* ── Vblank engine (drm_vblank.c) ────────────────────────────────────────── */

/** drm_vblank_init() — start the vblank worker.  Call once, after the cards. */
void drm_vblank_init(void);

/** drm_vblank_running() — false until the worker thread is up. */
bool drm_vblank_running(void);

/** drm_now_ns() — the monotonic clock the vblank engine times against. */
u64  drm_now_ns(void);

/**
 * drm_vblank_crtc_reset(crtc, mode) — restart the vblank clock for a mode.
 *
 * Called from mode set: the period comes from mode->vrefresh, and the
 * sequence keeps counting so a client's frame numbers stay monotonic.
 */
void drm_vblank_crtc_reset(drm_crtc_t *crtc, const drm_display_mode_t *mode);

/**
 * drm_vblank_queue_flip() — schedule @fb to become the scanout at the next
 * vblank.  Returns -EBUSY if a flip is already pending on this CRTC, which is
 * exactly what libdrm clients expect and use to pace themselves.  With
 * @async the swap happens immediately instead (DRM_MODE_PAGE_FLIP_ASYNC).
 */
int  drm_vblank_queue_flip(drm_crtc_t *crtc, drm_file_t *file,
                           drm_framebuffer_t *fb, u64 user_data,
                           bool want_event, bool async);

/**
 * drm_vblank_wait(crtc, target, out_seq, out_ns) — block until the CRTC's
 * vblank count reaches @target (0 means "the next one").  Returns 0, or
 * -EINVAL/-EAGAIN if the wait could not be honoured.
 */
int  drm_vblank_wait(drm_crtc_t *crtc, u64 target, u64 *out_seq, u64 *out_ns);

/** drm_crtc_add_damage(crtc, r) — union @r into the CRTC's pending damage. */
void drm_crtc_add_damage(drm_crtc_t *crtc, const drm_rect_t *r);

/** drm_crtc_flush_damage(crtc) — push pending damage to the display now. */
int  drm_crtc_flush_damage(drm_crtc_t *crtc);

/** drm_vblank_file_closed() — drop any flip a closing file still owns. */
void drm_vblank_file_closed(drm_device_t *dev, drm_file_t *file);

/**
 * drm_wait_on(chan) / drm_wake(chan) — sleep and wake on a kernel address.
 *
 * A sleeper is woken by drm_wake() on the same address, and in any case after
 * one scheduler tick, so a lost wake-up costs latency rather than a hang.
 * Callers re-test their condition in a loop.
 */
void drm_wait_on(const void *chan);
void drm_wake(const void *chan);

/* ── Built-in drivers ────────────────────────────────────────────────────── */
void drm_subsystem_init(void);   /* registers the class and every KMS driver */
void bochs_drm_init(void);
void vmwgfx_drm_init(void);
void simpledrm_init(void);
void virtgpu_drm_init(void);
