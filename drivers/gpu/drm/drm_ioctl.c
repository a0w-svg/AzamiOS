/* ============================================================================
 * AzamiOS — DRM: user API dispatch
 * File: drivers/gpu/drm/drm_ioctl.c
 *
 * The whole DRM_IOCTL_* surface, written once against the core object model
 * so no GPU driver has to touch userspace memory.  Requests that enumerate
 * resources follow Linux's two-pass convention: a client first calls with
 * zero counts to learn how many objects exist, allocates, then calls again —
 * so the kernel fills an array only when the client's buffer is large enough,
 * and always reports the true count.
 *
 * Access control mirrors Linux: render nodes get GEM and PRIME but no
 * modesetting, and the KMS requests that change what is on screen require
 * DRM master.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "drm_core.h"
#include "../../../kernel/uaccess.h"
#include "../../../kernel/lib/string.h"
#include "../../../kernel/mm/kmalloc.h"

/* ── User-copy helpers ───────────────────────────────────────────────────── */

#define DRM_COPY_IN(dst, arg)                                        \
    do {                                                             \
        if (copy_from_user((dst), (void *)(uintptr_t)(arg),          \
                           sizeof(*(dst))) != 0) return -(s64)EFAULT;\
    } while (0)

#define DRM_COPY_OUT(arg, src)                                       \
    do {                                                             \
        if (copy_to_user((void *)(uintptr_t)(arg), (src),            \
                         sizeof(*(src))) != 0) return -(s64)EFAULT;  \
    } while (0)

/* Copy a NUL-terminated string into a user buffer of known capacity. */
static void drm_copy_string(char *user_ptr, size_t cap, const char *str)
{
    if (!user_ptr || cap == 0) return;
    size_t len = strlen(str);
    if (len >= cap) len = cap - 1;
    copy_to_user(user_ptr, str, len);
}

/* KMS requests are refused on render nodes and on non-master file handles. */
static bool drm_can_modeset(drm_file_t *file)
{
    return !file->is_render_node && file->is_master;
}

/* ── Mode translation ────────────────────────────────────────────────────── */

static void drm_mode_to_user(struct drm_mode_modeinfo *out, const drm_display_mode_t *in)
{
    out->clock       = in->clock;
    out->hdisplay    = in->hdisplay;
    out->hsync_start = in->hsync_start;
    out->hsync_end   = in->hsync_end;
    out->htotal      = in->htotal;
    out->hskew       = in->hskew;
    out->vdisplay    = in->vdisplay;
    out->vsync_start = in->vsync_start;
    out->vsync_end   = in->vsync_end;
    out->vtotal      = in->vtotal;
    out->vscan       = in->vscan;
    out->vrefresh    = in->vrefresh;
    out->flags       = in->flags;
    out->type        = in->type;
    memcpy(out->name, in->name, sizeof(out->name));
}

static void drm_mode_from_user(drm_display_mode_t *out, const struct drm_mode_modeinfo *in)
{
    memset(out, 0, sizeof(*out));
    out->clock       = in->clock;
    out->hdisplay    = in->hdisplay;
    out->hsync_start = in->hsync_start;
    out->hsync_end   = in->hsync_end;
    out->htotal      = in->htotal;
    out->hskew       = in->hskew;
    out->vdisplay    = in->vdisplay;
    out->vsync_start = in->vsync_start;
    out->vsync_end   = in->vsync_end;
    out->vtotal      = in->vtotal;
    out->vscan       = in->vscan;
    out->vrefresh    = in->vrefresh;
    out->flags       = in->flags;
    out->type        = in->type;
    memcpy(out->name, in->name, sizeof(out->name) - 1);
}

/* ── Core requests ───────────────────────────────────────────────────────── */

static s64 drm_ioctl_version(drm_device_t *dev, u64 arg)
{
    struct drm_version ver;
    DRM_COPY_IN(&ver, arg);

    const drm_driver_t *drv = dev->driver;
    ver.version_major      = drv->major;
    ver.version_minor      = drv->minor;
    ver.version_patchlevel = drv->patchlevel;

    if (ver.name && ver.name_len) drm_copy_string(ver.name, ver.name_len, drv->name);
    if (ver.date && ver.date_len) drm_copy_string(ver.date, ver.date_len, drv->date);
    if (ver.desc && ver.desc_len) drm_copy_string(ver.desc, ver.desc_len, drv->desc);

    ver.name_len = strlen(drv->name);
    ver.date_len = strlen(drv->date);
    ver.desc_len = strlen(drv->desc);

    DRM_COPY_OUT(arg, &ver);
    return 0;
}

static s64 drm_ioctl_get_cap(drm_device_t *dev, u64 arg)
{
    struct drm_get_cap cap;
    DRM_COPY_IN(&cap, arg);

    switch (cap.capability) {
    case DRM_CAP_DUMB_BUFFER:
        cap.value = (dev->driver->features & DRIVER_GEM) ? 1 : 0;
        break;
    case DRM_CAP_DUMB_PREFERRED_DEPTH:  cap.value = 24; break;
    case DRM_CAP_DUMB_PREFER_SHADOW:    cap.value = dev->prefer_shadow ? 1 : 0; break;
    case DRM_CAP_PRIME:                 cap.value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT; break;
    case DRM_CAP_TIMESTAMP_MONOTONIC:   cap.value = 1; break;
    case DRM_CAP_ASYNC_PAGE_FLIP:       cap.value = dev->driver->page_flip ? 1 : 0; break;
    case DRM_CAP_CURSOR_WIDTH:          cap.value = dev->cursor_width; break;
    case DRM_CAP_CURSOR_HEIGHT:         cap.value = dev->cursor_height; break;
    case DRM_CAP_VBLANK_HIGH_CRTC:      cap.value = 1; break;
    case DRM_CAP_CRTC_IN_VBLANK_EVENT:  cap.value = 1; break;
    default:                            cap.value = 0; break;
    }

    DRM_COPY_OUT(arg, &cap);
    return 0;
}

static s64 drm_ioctl_set_client_cap(drm_file_t *file, u64 arg)
{
    struct drm_set_client_cap ccap;
    DRM_COPY_IN(&ccap, arg);

    switch (ccap.capability) {
    case DRM_CLIENT_CAP_UNIVERSAL_PLANES:
        file->universal_planes = ccap.value != 0;
        return 0;
    case DRM_CLIENT_CAP_ATOMIC:
        /* Atomic modesetting is not implemented; a client that insists on it
         * must be told so rather than silently getting legacy behaviour. */
        if (!(file->dev->driver->features & DRIVER_ATOMIC)) return -(s64)EOPNOTSUPP;
        file->atomic = ccap.value != 0;
        return 0;
    case DRM_CLIENT_CAP_STEREO_3D:
        return ccap.value ? -(s64)EOPNOTSUPP : 0;
    default:
        return -(s64)EINVAL;
    }
}

/* ── Resource enumeration ────────────────────────────────────────────────── */

/*
 * Copy an ID array out under Linux's two-pass rule: fill the client's buffer
 * only when it is big enough for every object, then report the real count.
 */
static bool drm_copy_ids(u64 user_ptr, u32 user_count, const u32 *ids, u32 count)
{
    if (!user_ptr || user_count < count || count == 0) return true;
    return copy_to_user((void *)(uintptr_t)user_ptr, ids, count * sizeof(u32)) == 0;
}

static s64 drm_ioctl_getresources(drm_device_t *dev, u64 arg)
{
    struct drm_mode_card_res res;
    DRM_COPY_IN(&res, arg);

    u32 ids[32];
    u32 n;

    n = 0;
    for (drm_crtc_t *c = dev->crtc_list; c && n < ARRAY_SIZE(ids); c = c->next) ids[n++] = c->base.id;
    if (!drm_copy_ids(res.crtc_id_ptr, res.count_crtcs, ids, n)) return -(s64)EFAULT;
    res.count_crtcs = n;

    n = 0;
    for (drm_connector_t *c = dev->connector_list; c && n < ARRAY_SIZE(ids); c = c->next) ids[n++] = c->base.id;
    if (!drm_copy_ids(res.connector_id_ptr, res.count_connectors, ids, n)) return -(s64)EFAULT;
    res.count_connectors = n;

    n = 0;
    for (drm_encoder_t *e = dev->encoder_list; e && n < ARRAY_SIZE(ids); e = e->next) ids[n++] = e->base.id;
    if (!drm_copy_ids(res.encoder_id_ptr, res.count_encoders, ids, n)) return -(s64)EFAULT;
    res.count_encoders = n;

    n = 0;
    for (drm_framebuffer_t *f = dev->fb_list; f && n < ARRAY_SIZE(ids); f = f->next) ids[n++] = f->base.id;
    if (!drm_copy_ids(res.fb_id_ptr, res.count_fbs, ids, n)) return -(s64)EFAULT;
    res.count_fbs = n;

    res.min_width  = dev->min_width;
    res.max_width  = dev->max_width;
    res.min_height = dev->min_height;
    res.max_height = dev->max_height;

    DRM_COPY_OUT(arg, &res);
    return 0;
}

static s64 drm_ioctl_getcrtc(drm_device_t *dev, u64 arg)
{
    struct drm_mode_crtc out;
    DRM_COPY_IN(&out, arg);

    drm_crtc_t *crtc = drm_crtc_find(dev, out.crtc_id);
    if (!crtc) return -(s64)ENOENT;

    out.fb_id      = crtc->fb ? crtc->fb->base.id : 0;
    out.x          = crtc->x;
    out.y          = crtc->y;
    out.gamma_size = 0;
    out.mode_valid = crtc->mode_valid ? 1 : 0;
    memset(&out.mode, 0, sizeof(out.mode));
    if (crtc->mode_valid) drm_mode_to_user(&out.mode, &crtc->mode);

    DRM_COPY_OUT(arg, &out);
    return 0;
}

static s64 drm_ioctl_setcrtc(drm_device_t *dev, drm_file_t *file, u64 arg)
{
    if (!drm_can_modeset(file)) return -(s64)EACCES;

    struct drm_mode_crtc req;
    DRM_COPY_IN(&req, arg);

    drm_crtc_t *crtc = drm_crtc_find(dev, req.crtc_id);
    if (!crtc) return -(s64)ENOENT;

    /* fb_id 0 with no mode is the documented way to turn a CRTC off. */
    if (!req.mode_valid) {
        if (dev->driver->mode_set) dev->driver->mode_set(crtc, NULL, NULL, 0, 0);
        crtc->fb         = NULL;
        crtc->enabled    = false;
        crtc->mode_valid = false;
        return 0;
    }

    drm_framebuffer_t *fb = NULL;
    if (req.fb_id) {
        fb = drm_framebuffer_find(dev, req.fb_id);
        if (!fb) return -(s64)ENOENT;
    }

    drm_display_mode_t mode;
    drm_mode_from_user(&mode, &req.mode);
    if (mode.hdisplay == 0 || mode.vdisplay == 0) return -(s64)EINVAL;
    if (mode.hdisplay > dev->max_width || mode.vdisplay > dev->max_height) return -(s64)EINVAL;

    int ret = 0;
    if (dev->driver->mode_set) {
        ret = dev->driver->mode_set(crtc, fb, &mode, req.x, req.y);
    }
    if (ret != 0) return (s64)ret;

    crtc->mode       = mode;
    crtc->mode_valid = true;
    crtc->enabled    = true;
    crtc->fb         = fb;
    crtc->x          = req.x;
    crtc->y          = req.y;
    if (crtc->primary) crtc->primary->fb = fb;

    /* The frame period follows the mode, and the freshly programmed scanout
     * is already correct, so nothing is owed to the next frame. */
    drm_vblank_crtc_reset(crtc, &mode);
    crtc->damage_valid = false;

    pr_debug("[DRM] card%d: CRTC %u set to %ux%u fb=%u\n",
             dev->index, crtc->base.id, mode.hdisplay, mode.vdisplay, req.fb_id);
    return 0;
}

static s64 drm_ioctl_getconnector(drm_device_t *dev, u64 arg)
{
    struct drm_mode_get_connector out;
    DRM_COPY_IN(&out, arg);

    drm_connector_t *conn = drm_connector_find(dev, out.connector_id);
    if (!conn) return -(s64)ENOENT;

    if (out.modes_ptr && out.count_modes >= conn->nmodes && conn->nmodes) {
        for (u32 i = 0; i < conn->nmodes; i++) {
            struct drm_mode_modeinfo umode;
            drm_mode_to_user(&umode, &conn->modes[i]);
            if (copy_to_user((void *)(uintptr_t)(out.modes_ptr + i * sizeof(umode)),
                             &umode, sizeof(umode)) != 0) {
                return -(s64)EFAULT;
            }
        }
    }
    out.count_modes = conn->nmodes;

    u32 enc_id = conn->encoder ? conn->encoder->base.id : 0;
    u32 nenc   = conn->encoder ? 1 : 0;
    if (!drm_copy_ids(out.encoders_ptr, out.count_encoders, &enc_id, nenc)) return -(s64)EFAULT;
    out.count_encoders = nenc;

    /* No connector properties are exposed yet; report an empty set rather
     * than leaving the client's count untouched. */
    out.count_props = 0;

    out.encoder_id        = enc_id;
    out.connector_type    = conn->connector_type;
    out.connector_type_id = conn->connector_type_id;
    out.connection        = conn->status;
    out.mm_width          = conn->mm_width;
    out.mm_height         = conn->mm_height;
    out.subpixel          = conn->subpixel;

    DRM_COPY_OUT(arg, &out);
    return 0;
}

static s64 drm_ioctl_getencoder(drm_device_t *dev, u64 arg)
{
    struct drm_mode_get_encoder out;
    DRM_COPY_IN(&out, arg);

    drm_encoder_t *enc = drm_encoder_find(dev, out.encoder_id);
    if (!enc) return -(s64)ENOENT;

    out.encoder_type    = enc->encoder_type;
    out.crtc_id         = enc->crtc ? enc->crtc->base.id : 0;
    out.possible_crtcs  = enc->possible_crtcs;
    out.possible_clones = enc->possible_clones;

    DRM_COPY_OUT(arg, &out);
    return 0;
}

/* ── Planes ──────────────────────────────────────────────────────────────── */

static s64 drm_ioctl_getplaneresources(drm_device_t *dev, drm_file_t *file, u64 arg)
{
    struct drm_mode_get_plane_res out;
    DRM_COPY_IN(&out, arg);

    u32 ids[16];
    u32 n = 0;
    for (drm_plane_t *p = dev->plane_list; p && n < ARRAY_SIZE(ids); p = p->next) {
        /* Without the universal-planes client cap, only overlays are visible,
         * exactly as in Linux. */
        if (!file->universal_planes && p->plane_type != DRM_PLANE_TYPE_OVERLAY) continue;
        ids[n++] = p->base.id;
    }

    if (!drm_copy_ids(out.plane_id_ptr, out.count_planes, ids, n)) return -(s64)EFAULT;
    out.count_planes = n;

    DRM_COPY_OUT(arg, &out);
    return 0;
}

static s64 drm_ioctl_getplane(drm_device_t *dev, u64 arg)
{
    struct drm_mode_get_plane out;
    DRM_COPY_IN(&out, arg);

    drm_plane_t *plane = drm_plane_find(dev, out.plane_id);
    if (!plane) return -(s64)ENOENT;

    if (out.format_type_ptr && out.count_format_types >= plane->nformats && plane->nformats) {
        if (copy_to_user((void *)(uintptr_t)out.format_type_ptr,
                         plane->formats, plane->nformats * sizeof(u32)) != 0) {
            return -(s64)EFAULT;
        }
    }
    out.count_format_types = plane->nformats;

    out.crtc_id        = plane->crtc ? plane->crtc->base.id : 0;
    out.fb_id          = plane->fb ? plane->fb->base.id : 0;
    out.possible_crtcs = plane->possible_crtcs;
    out.gamma_size     = 0;

    DRM_COPY_OUT(arg, &out);
    return 0;
}

static s64 drm_ioctl_setplane(drm_device_t *dev, drm_file_t *file, u64 arg)
{
    if (!drm_can_modeset(file)) return -(s64)EACCES;

    struct drm_mode_set_plane req;
    DRM_COPY_IN(&req, arg);

    drm_plane_t *plane = drm_plane_find(dev, req.plane_id);
    if (!plane) return -(s64)ENOENT;

    /* fb_id 0 disables the plane. */
    if (req.fb_id == 0) {
        plane->fb   = NULL;
        plane->crtc = NULL;
        return 0;
    }

    drm_framebuffer_t *fb = drm_framebuffer_find(dev, req.fb_id);
    if (!fb) return -(s64)ENOENT;

    drm_crtc_t *crtc = drm_crtc_find(dev, req.crtc_id);
    if (!crtc) return -(s64)ENOENT;
    if (!(plane->possible_crtcs & (1U << crtc->index))) return -(s64)EINVAL;

    plane->fb     = fb;
    plane->crtc   = crtc;
    plane->crtc_x = req.crtc_x;
    plane->crtc_y = req.crtc_y;
    plane->crtc_w = req.crtc_w;
    plane->crtc_h = req.crtc_h;

    /* The primary plane is the scanout source, so updating it is a flip and
     * lands at a frame boundary like any other. */
    if (plane->plane_type == DRM_PLANE_TYPE_PRIMARY) {
        int ret = drm_vblank_queue_flip(crtc, file, fb, 0, false, false);
        if (ret == -EBUSY) ret = 0;   /* the queued flip supersedes this one */
        if (ret != 0) return (s64)ret;
    }
    return 0;
}

/* ── Framebuffers ────────────────────────────────────────────────────────── */

static s64 drm_ioctl_addfb(drm_device_t *dev, drm_file_t *file, u64 arg)
{
    if (file->is_render_node) return -(s64)EACCES;

    struct drm_mode_fb_cmd req;
    DRM_COPY_IN(&req, arg);

    drm_gem_object_t *obj = drm_gem_handle_lookup(file, req.handle);
    if (!obj) return -(s64)ENOENT;
    if (req.pitch == 0 || req.height == 0) return -(s64)EINVAL;
    if ((size_t)req.pitch * req.height > obj->size) return -(s64)EINVAL;

    u32 format = (req.depth == 32) ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
    drm_framebuffer_t *fb = drm_framebuffer_create(dev, obj, req.width, req.height,
                                                   req.pitch, req.bpp, req.depth, format);
    if (!fb) return -(s64)ENOMEM;

    req.fb_id = fb->base.id;
    DRM_COPY_OUT(arg, &req);
    return 0;
}

static s64 drm_ioctl_addfb2(drm_device_t *dev, drm_file_t *file, u64 arg)
{
    if (file->is_render_node) return -(s64)EACCES;

    struct drm_mode_fb_cmd2 req;
    DRM_COPY_IN(&req, arg);

    drm_gem_object_t *obj = drm_gem_handle_lookup(file, req.handles[0]);
    if (!obj) return -(s64)ENOENT;

    u32 bpp, depth;
    switch (req.pixel_format) {
    case DRM_FORMAT_XRGB8888: bpp = 32; depth = 24; break;
    case DRM_FORMAT_ARGB8888: bpp = 32; depth = 32; break;
    case DRM_FORMAT_RGB565:   bpp = 16; depth = 16; break;
    default:                  return -(s64)EINVAL;
    }

    u32 pitch = req.pitches[0];
    if (pitch == 0 || req.height == 0) return -(s64)EINVAL;
    if ((size_t)pitch * req.height + req.offsets[0] > obj->size) return -(s64)EINVAL;

    drm_framebuffer_t *fb = drm_framebuffer_create(dev, obj, req.width, req.height,
                                                   pitch, bpp, depth, req.pixel_format);
    if (!fb) return -(s64)ENOMEM;

    req.fb_id = fb->base.id;
    DRM_COPY_OUT(arg, &req);
    return 0;
}

static s64 drm_ioctl_getfb(drm_device_t *dev, u64 arg)
{
    struct drm_mode_fb_cmd out;
    DRM_COPY_IN(&out, arg);

    drm_framebuffer_t *fb = drm_framebuffer_find(dev, out.fb_id);
    if (!fb) return -(s64)ENOENT;

    out.width  = fb->width;
    out.height = fb->height;
    out.pitch  = fb->pitch;
    out.bpp    = fb->bpp;
    out.depth  = fb->depth;
    /* Linux only hands back a GEM handle to the master; unprivileged callers
     * get the geometry with handle 0. */
    out.handle = 0;

    DRM_COPY_OUT(arg, &out);
    return 0;
}

static s64 drm_ioctl_rmfb(drm_device_t *dev, drm_file_t *file, u64 arg)
{
    if (file->is_render_node) return -(s64)EACCES;

    u32 fb_id;
    if (copy_from_user(&fb_id, (void *)(uintptr_t)arg, sizeof(fb_id)) != 0) return -(s64)EFAULT;

    drm_framebuffer_t *fb = drm_framebuffer_find(dev, fb_id);
    if (!fb) return -(s64)ENOENT;

    drm_framebuffer_put(dev, fb);
    return 0;
}

static s64 drm_ioctl_dirtyfb(drm_device_t *dev, drm_file_t *file, u64 arg)
{
    if (file->is_render_node) return -(s64)EACCES;

    struct drm_mode_fb_dirty_cmd req;
    DRM_COPY_IN(&req, arg);

    drm_framebuffer_t *fb = drm_framebuffer_find(dev, req.fb_id);
    if (!fb) return -(s64)ENOENT;
    if (!dev->driver->dirty_fb) return 0;

    drm_crtc_t *crtc = NULL;
    for (drm_crtc_t *c = dev->crtc_list; c; c = c->next) {
        if (c->fb == fb) { crtc = c; break; }
    }
    if (!crtc) return 0;   /* not on screen; nothing to flush */

    /*
     * Record what the client says changed rather than flushing the whole
     * framebuffer.  A compositor that redraws one menu then pays for one
     * menu, and the copy itself happens at the next frame boundary so it is
     * never visible half-done.
     */
    if (req.num_clips == 0 || req.clips_ptr == 0) {
        drm_crtc_add_damage(crtc, NULL);            /* whole framebuffer */
    } else {
        u32 n = req.num_clips;
        if (n > 64) { drm_crtc_add_damage(crtc, NULL); n = 0; }

        for (u32 i = 0; i < n; i++) {
            struct drm_clip_rect c;
            if (copy_from_user(&c, (void *)(uintptr_t)(req.clips_ptr + i * sizeof(c)),
                               sizeof(c)) != 0) {
                return -(s64)EFAULT;
            }
            drm_rect_t r = { c.x1, c.y1, c.x2, c.y2 };
            if (r.x2 > fb->width)  r.x2 = fb->width;
            if (r.y2 > fb->height) r.y2 = fb->height;
            drm_crtc_add_damage(crtc, &r);
        }
    }

    /* Without a running vblank clock there is nothing to defer to. */
    if (!drm_vblank_running()) return (s64)drm_crtc_flush_damage(crtc);
    return 0;
}

/* ── Dumb buffers ────────────────────────────────────────────────────────── */

static s64 drm_ioctl_create_dumb(drm_device_t *dev, drm_file_t *file, u64 arg)
{
    struct drm_mode_create_dumb req;
    DRM_COPY_IN(&req, arg);

    if (req.width == 0 || req.height == 0) return -(s64)EINVAL;
    if (req.width > 8192 || req.height > 8192) return -(s64)EINVAL;

    u32 bpp = req.bpp ? req.bpp : 32;
    if (bpp != 16 && bpp != 24 && bpp != 32) return -(s64)EINVAL;

    /* 64-byte pitch alignment keeps rows on cache lines and matches what the
     * scanout engines here expect. */
    u32 pitch = ALIGN_UP(req.width * ((bpp + 7) / 8), 64u);

    drm_gem_object_t *obj = drm_gem_object_create(dev, req.width, req.height, bpp, pitch);
    if (!obj) return -(s64)ENOMEM;

    u32 handle = drm_gem_handle_create(file, obj);
    /* create() returned the object with one reference for the caller; the
     * handle took its own, so drop ours either way. */
    drm_gem_object_put(dev, obj);
    if (!handle) return -(s64)EMFILE;

    req.handle = handle;
    req.pitch  = pitch;
    req.size   = obj->size;

    DRM_COPY_OUT(arg, &req);
    return 0;
}

static s64 drm_ioctl_map_dumb(drm_file_t *file, u64 arg)
{
    struct drm_mode_map_dumb req;
    DRM_COPY_IN(&req, arg);

    drm_gem_object_t *obj = drm_gem_handle_lookup(file, req.handle);
    if (!obj) return -(s64)ENOENT;

    req.offset = obj->mmap_offset;
    DRM_COPY_OUT(arg, &req);
    return 0;
}

static s64 drm_ioctl_destroy_dumb(drm_file_t *file, u64 arg)
{
    struct drm_mode_destroy_dumb req;
    DRM_COPY_IN(&req, arg);
    return drm_gem_handle_delete(file, req.handle) == 0 ? 0 : -(s64)ENOENT;
}

/* ── GEM naming ──────────────────────────────────────────────────────────── */

static s64 drm_ioctl_gem_close(drm_file_t *file, u64 arg)
{
    struct drm_gem_close req;
    DRM_COPY_IN(&req, arg);
    return drm_gem_handle_delete(file, req.handle) == 0 ? 0 : -(s64)EINVAL;
}

static s64 drm_ioctl_gem_flink(drm_device_t *dev, drm_file_t *file, u64 arg)
{
    struct drm_gem_flink req;
    DRM_COPY_IN(&req, arg);

    drm_gem_object_t *obj = drm_gem_handle_lookup(file, req.handle);
    if (!obj) return -(s64)ENOENT;

    req.name = drm_gem_object_flink(dev, obj);
    DRM_COPY_OUT(arg, &req);
    return 0;
}

static s64 drm_ioctl_gem_open(drm_device_t *dev, drm_file_t *file, u64 arg)
{
    struct drm_gem_open req;
    DRM_COPY_IN(&req, arg);

    drm_gem_object_t *obj = drm_gem_object_by_name(dev, req.name);
    if (!obj) return -(s64)ENOENT;

    u32 handle = drm_gem_handle_create(file, obj);
    if (!handle) return -(s64)EMFILE;

    req.handle = handle;
    req.size   = obj->size;
    DRM_COPY_OUT(arg, &req);
    return 0;
}

/* ── PRIME ───────────────────────────────────────────────────────────────── */

static s64 drm_ioctl_prime_handle_to_fd(drm_device_t *dev, drm_file_t *file, u64 arg)
{
    struct drm_prime_handle req;
    DRM_COPY_IN(&req, arg);

    drm_gem_object_t *obj = drm_gem_handle_lookup(file, req.handle);
    if (!obj) return -(s64)ENOENT;

    int fd = drm_prime_export_fd(dev, obj);
    if (fd < 0) return (s64)fd;

    req.fd = fd;
    DRM_COPY_OUT(arg, &req);
    return 0;
}

static s64 drm_ioctl_prime_fd_to_handle(drm_device_t *dev, drm_file_t *file, u64 arg)
{
    struct drm_prime_handle req;
    DRM_COPY_IN(&req, arg);

    drm_gem_object_t *obj = drm_prime_import_fd(dev, req.fd);
    if (!obj) return -(s64)EBADF;

    u32 handle = drm_gem_handle_create(file, obj);
    if (!handle) return -(s64)EMFILE;

    req.handle = handle;
    DRM_COPY_OUT(arg, &req);
    return 0;
}

/* ── Flips, cursors and vblank ───────────────────────────────────────────── */

/*
 * PAGE_FLIP hands the framebuffer to the vblank engine and returns; the swap
 * happens at the next frame boundary and the completion event says when.
 * That is the whole anti-tearing contract: the client draws into the buffer
 * it is not showing, and the display only ever switches between frames.
 */
static s64 drm_ioctl_page_flip(drm_device_t *dev, drm_file_t *file, u64 arg)
{
    if (!drm_can_modeset(file)) return -(s64)EACCES;

    struct drm_mode_crtc_page_flip req;
    DRM_COPY_IN(&req, arg);

    drm_crtc_t *crtc = drm_crtc_find(dev, req.crtc_id);
    if (!crtc) return -(s64)ENOENT;

    drm_framebuffer_t *fb = drm_framebuffer_find(dev, req.fb_id);
    if (!fb) return -(s64)ENOENT;
    if (!crtc->enabled) return -(s64)EINVAL;

    int ret = drm_vblank_queue_flip(crtc, file, fb, req.user_data,
                                    (req.flags & DRM_MODE_PAGE_FLIP_EVENT) != 0,
                                    (req.flags & DRM_MODE_PAGE_FLIP_ASYNC) != 0);
    return (s64)ret;
}

static s64 drm_ioctl_cursor(drm_device_t *dev, drm_file_t *file, u32 cmd, u64 arg)
{
    if (!drm_can_modeset(file)) return -(s64)EACCES;

    /* CURSOR2 is CURSOR plus a hotspot; read the larger layout only for it. */
    struct drm_mode_cursor2 req;
    memset(&req, 0, sizeof(req));

    if (cmd == DRM_IOCTL_MODE_CURSOR2) {
        DRM_COPY_IN(&req, arg);
    } else {
        struct drm_mode_cursor small;
        DRM_COPY_IN(&small, arg);
        req.flags  = small.flags;
        req.crtc_id = small.crtc_id;
        req.x = small.x; req.y = small.y;
        req.width = small.width; req.height = small.height;
        req.handle = small.handle;
    }

    drm_crtc_t *crtc = drm_crtc_find(dev, req.crtc_id);
    if (!crtc) return -(s64)ENOENT;

    if (req.flags & DRM_MODE_CURSOR_BO) {
        drm_gem_object_t *bo = req.handle ? drm_gem_handle_lookup(file, req.handle) : NULL;
        if (req.handle && !bo) return -(s64)ENOENT;

        if (crtc->cursor_bo) drm_gem_object_put(dev, crtc->cursor_bo);
        crtc->cursor_bo = bo;
        if (bo) drm_gem_object_get(bo);
        crtc->cursor_visible = bo != NULL;

        if (dev->driver->cursor_set) {
            int ret = dev->driver->cursor_set(crtc, bo, req.width, req.height);
            if (ret != 0) return (s64)ret;
        }
    }

    if (req.flags & DRM_MODE_CURSOR_MOVE) {
        crtc->cursor_x = req.x;
        crtc->cursor_y = req.y;
        if (cmd == DRM_IOCTL_MODE_CURSOR2) {
            crtc->cursor_hot_x = req.hot_x;
            crtc->cursor_hot_y = req.hot_y;
        }
        if (dev->driver->cursor_move) {
            int ret = dev->driver->cursor_move(crtc, req.x, req.y);
            if (ret != 0) return (s64)ret;
        }
    }
    return 0;
}

/*
 * WAIT_VBLANK blocks until the frame the client asked for, either an absolute
 * sequence number or one relative to now.  It is what a client without a
 * flip-completion loop uses to pace itself, so it has to actually wait — a
 * counter that increments on being read teaches userspace to spin.
 */
static s64 drm_ioctl_wait_vblank(drm_device_t *dev, drm_file_t *file, u64 arg)
{
    union drm_wait_vblank req;
    DRM_COPY_IN(&req, arg);

    /* The CRTC index rides in the high bits of the type field. */
    u32 idx = (req.request.type & _DRM_VBLANK_HIGH_CRTC_MASK)
              >> _DRM_VBLANK_HIGH_CRTC_SHIFT;
    if (req.request.type & _DRM_VBLANK_SECONDARY) idx = 1;

    drm_crtc_t *crtc = dev->crtc_list;
    for (u32 i = 0; crtc && i < idx; i++) crtc = crtc->next;
    if (!crtc) return -(s64)EINVAL;

    bool relative = (req.request.type & _DRM_VBLANK_RELATIVE) != 0;
    u64  current  = crtc->vblank_count;
    u64  target   = relative ? current + req.request.sequence
                             : req.request.sequence;

    /* An absolute request for a frame already gone by is answered now. */
    if (target <= current) target = current;

    /* _DRM_VBLANK_EVENT means "tell me on the fd", not "block here". */
    if (req.request.type & _DRM_VBLANK_EVENT) {
        drm_send_event(file, crtc, DRM_EVENT_VBLANK, req.request.signal,
                       crtc->last_vblank_ns, target);
        memset(&req.reply, 0, sizeof(req.reply));
        req.reply.type     = DRM_EVENT_VBLANK;
        req.reply.sequence = (u32)target;
        DRM_COPY_OUT(arg, &req);
        return 0;
    }

    u64 seq = current, ns = crtc->last_vblank_ns;
    drm_vblank_wait(crtc, target, &seq, &ns);

    memset(&req.reply, 0, sizeof(req.reply));
    req.reply.type      = DRM_EVENT_VBLANK;
    req.reply.sequence  = (u32)seq;
    req.reply.tval_sec  = (s64)(ns / 1000000000ULL);
    req.reply.tval_usec = (s64)((ns % 1000000000ULL) / 1000ULL);

    DRM_COPY_OUT(arg, &req);
    return 0;
}

/* ── Authentication and master ───────────────────────────────────────────── */

static s64 drm_ioctl_master(drm_device_t *dev, drm_file_t *file, bool set)
{
    spinlock_lock(&dev->lock);
    s64 ret = 0;

    if (set) {
        if (dev->master && dev->master != file) {
            ret = -(s64)EBUSY;
        } else {
            dev->master        = file;
            file->is_master    = true;
            file->authenticated = true;
        }
    } else if (dev->master == file) {
        dev->master     = NULL;
        file->is_master = false;
    }

    spinlock_unlock(&dev->lock);
    return ret;
}

/* ── Dispatch ────────────────────────────────────────────────────────────── */

s64 drm_ioctl_dispatch(drm_device_t *dev, drm_file_t *file, u32 cmd, u64 arg)
{
    if (!dev || !file) return -(s64)EBADF;

    /* SET_MASTER/DROP_MASTER carry no argument; everything else must point at
     * a user address. */
    bool needs_arg = (cmd != DRM_IOCTL_SET_MASTER && cmd != DRM_IOCTL_DROP_MASTER);
    if (needs_arg && (!arg || (uintptr_t)arg >= 0x8000000000000000ULL)) {
        return -(s64)EFAULT;
    }

    switch (cmd) {
    case DRM_IOCTL_VERSION:            return drm_ioctl_version(dev, arg);
    case DRM_IOCTL_GET_CAP:            return drm_ioctl_get_cap(dev, arg);
    case DRM_IOCTL_SET_CLIENT_CAP:     return drm_ioctl_set_client_cap(file, arg);
    case DRM_IOCTL_SET_MASTER:         return drm_ioctl_master(dev, file, true);
    case DRM_IOCTL_DROP_MASTER:        return drm_ioctl_master(dev, file, false);

    case DRM_IOCTL_GET_UNIQUE: {
        struct drm_unique unq;
        DRM_COPY_IN(&unq, arg);
        if (unq.unique && unq.unique_len) {
            drm_copy_string(unq.unique, unq.unique_len, dev->unique);
        }
        unq.unique_len = strlen(dev->unique);
        DRM_COPY_OUT(arg, &unq);
        return 0;
    }

    case DRM_IOCTL_GET_MAGIC: {
        struct drm_auth auth = { .magic = file->magic };
        DRM_COPY_OUT(arg, &auth);
        return 0;
    }

    case DRM_IOCTL_AUTH_MAGIC: {
        if (!file->is_master) return -(s64)EACCES;
        struct drm_auth auth;
        DRM_COPY_IN(&auth, arg);
        /* Single-master, single-machine: any magic the master vouches for is
         * accepted, which is all the authentication dance is used for here. */
        return 0;
    }

    case DRM_IOCTL_GEM_CLOSE:          return drm_ioctl_gem_close(file, arg);
    case DRM_IOCTL_GEM_FLINK:          return drm_ioctl_gem_flink(dev, file, arg);
    case DRM_IOCTL_GEM_OPEN:           return drm_ioctl_gem_open(dev, file, arg);
    case DRM_IOCTL_PRIME_HANDLE_TO_FD: return drm_ioctl_prime_handle_to_fd(dev, file, arg);
    case DRM_IOCTL_PRIME_FD_TO_HANDLE: return drm_ioctl_prime_fd_to_handle(dev, file, arg);
    case DRM_IOCTL_WAIT_VBLANK:        return drm_ioctl_wait_vblank(dev, file, arg);

    case DRM_IOCTL_MODE_CREATE_DUMB:   return drm_ioctl_create_dumb(dev, file, arg);
    case DRM_IOCTL_MODE_MAP_DUMB:      return drm_ioctl_map_dumb(file, arg);
    case DRM_IOCTL_MODE_DESTROY_DUMB:  return drm_ioctl_destroy_dumb(file, arg);
    }

    /* Everything below is modesetting, which render nodes never get. */
    if (file->is_render_node) return -(s64)EACCES;

    switch (cmd) {
    case DRM_IOCTL_MODE_GETRESOURCES:      return drm_ioctl_getresources(dev, arg);
    case DRM_IOCTL_MODE_GETCRTC:           return drm_ioctl_getcrtc(dev, arg);
    case DRM_IOCTL_MODE_SETCRTC:           return drm_ioctl_setcrtc(dev, file, arg);
    case DRM_IOCTL_MODE_GETCONNECTOR:      return drm_ioctl_getconnector(dev, arg);
    case DRM_IOCTL_MODE_GETENCODER:        return drm_ioctl_getencoder(dev, arg);
    case DRM_IOCTL_MODE_GETPLANERESOURCES: return drm_ioctl_getplaneresources(dev, file, arg);
    case DRM_IOCTL_MODE_GETPLANE:          return drm_ioctl_getplane(dev, arg);
    case DRM_IOCTL_MODE_SETPLANE:          return drm_ioctl_setplane(dev, file, arg);
    case DRM_IOCTL_MODE_ADDFB:             return drm_ioctl_addfb(dev, file, arg);
    case DRM_IOCTL_MODE_ADDFB2:            return drm_ioctl_addfb2(dev, file, arg);
    case DRM_IOCTL_MODE_GETFB:             return drm_ioctl_getfb(dev, arg);
    case DRM_IOCTL_MODE_RMFB:              return drm_ioctl_rmfb(dev, file, arg);
    case DRM_IOCTL_MODE_DIRTYFB:           return drm_ioctl_dirtyfb(dev, file, arg);
    case DRM_IOCTL_MODE_PAGE_FLIP:         return drm_ioctl_page_flip(dev, file, arg);
    case DRM_IOCTL_MODE_CURSOR:
    case DRM_IOCTL_MODE_CURSOR2:           return drm_ioctl_cursor(dev, file, cmd, arg);
    default:                               return -(s64)EINVAL;
    }
}
