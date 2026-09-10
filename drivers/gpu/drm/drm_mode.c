/* ============================================================================
 * AzamiOS — DRM: KMS object model
 * File: drivers/gpu/drm/drm_mode.c
 *
 * CRTCs, encoders, connectors, planes and framebuffers, plus the per-card ID
 * space they share.  Every object carries a drm_mode_object_t header with a
 * card-unique ID, so DRM_IOCTL_MODE_* requests can name any of them the way
 * Linux clients expect.
 *
 * A driver builds its display pipeline in load():
 *
 *     crtc = drm_crtc_create(dev);
 *     enc  = drm_encoder_create(dev, DRM_MODE_ENCODER_TMDS, 1 << crtc->index);
 *     conn = drm_connector_create(dev, DRM_MODE_CONNECTOR_VIRTUAL, enc);
 *     drm_connector_add_default_modes(conn, 1920, 1080, 1280, 800);
 *     drm_plane_create(dev, DRM_PLANE_TYPE_PRIMARY, 1 << crtc->index, fmts, 2);
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "drm_core.h"
#include "../../../kernel/mm/kmalloc.h"
#include "../../../kernel/lib/string.h"

/* ── Mode object IDs ─────────────────────────────────────────────────────── */

u32 drm_mode_object_add(drm_device_t *dev, drm_mode_object_t *obj, u32 type)
{
    obj->id   = dev->next_object_id++;
    obj->type = type;
    return obj->id;
}

drm_mode_object_t *drm_mode_object_find(drm_device_t *dev, u32 id, u32 type)
{
    if (!dev || id == 0) return NULL;

    if (type == DRM_MODE_OBJECT_CRTC || type == DRM_MODE_OBJECT_ANY) {
        for (drm_crtc_t *c = dev->crtc_list; c; c = c->next)
            if (c->base.id == id) return &c->base;
    }
    if (type == DRM_MODE_OBJECT_ENCODER || type == DRM_MODE_OBJECT_ANY) {
        for (drm_encoder_t *e = dev->encoder_list; e; e = e->next)
            if (e->base.id == id) return &e->base;
    }
    if (type == DRM_MODE_OBJECT_CONNECTOR || type == DRM_MODE_OBJECT_ANY) {
        for (drm_connector_t *c = dev->connector_list; c; c = c->next)
            if (c->base.id == id) return &c->base;
    }
    if (type == DRM_MODE_OBJECT_PLANE || type == DRM_MODE_OBJECT_ANY) {
        for (drm_plane_t *p = dev->plane_list; p; p = p->next)
            if (p->base.id == id) return &p->base;
    }
    if (type == DRM_MODE_OBJECT_FB || type == DRM_MODE_OBJECT_ANY) {
        for (drm_framebuffer_t *f = dev->fb_list; f; f = f->next)
            if (f->base.id == id) return &f->base;
    }
    return NULL;
}

/* Append to a singly-linked list so enumeration order matches creation
 * order — clients index CRTCs and planes positionally. */
#define DRM_LIST_APPEND(head, node)              \
    do {                                         \
        __typeof__(head) *_pp = &(head);         \
        while (*_pp) _pp = &(*_pp)->next;        \
        (node)->next = NULL;                     \
        *_pp = (node);                           \
    } while (0)

/* ── CRTCs ───────────────────────────────────────────────────────────────── */

drm_crtc_t *drm_crtc_create(drm_device_t *dev)
{
    drm_crtc_t *crtc = (drm_crtc_t *)kzalloc(sizeof(drm_crtc_t));
    if (!crtc) return NULL;

    crtc->dev   = dev;
    crtc->index = dev->num_crtc;
    drm_mode_object_add(dev, &crtc->base, DRM_MODE_OBJECT_CRTC);

    DRM_LIST_APPEND(dev->crtc_list, crtc);
    dev->num_crtc++;
    return crtc;
}

drm_crtc_t *drm_crtc_find(drm_device_t *dev, u32 id)
{
    drm_mode_object_t *o = drm_mode_object_find(dev, id, DRM_MODE_OBJECT_CRTC);
    return o ? container_of(o, drm_crtc_t, base) : NULL;
}

/* ── Encoders ────────────────────────────────────────────────────────────── */

drm_encoder_t *drm_encoder_create(drm_device_t *dev, u32 type, u32 possible_crtcs)
{
    drm_encoder_t *enc = (drm_encoder_t *)kzalloc(sizeof(drm_encoder_t));
    if (!enc) return NULL;

    enc->dev            = dev;
    enc->encoder_type   = type;
    enc->possible_crtcs = possible_crtcs;
    drm_mode_object_add(dev, &enc->base, DRM_MODE_OBJECT_ENCODER);

    /* Bind to the first CRTC the mask allows, the usual single-head case. */
    for (drm_crtc_t *c = dev->crtc_list; c; c = c->next) {
        if (possible_crtcs & (1U << c->index)) { enc->crtc = c; break; }
    }

    DRM_LIST_APPEND(dev->encoder_list, enc);
    dev->num_encoder++;
    return enc;
}

drm_encoder_t *drm_encoder_find(drm_device_t *dev, u32 id)
{
    drm_mode_object_t *o = drm_mode_object_find(dev, id, DRM_MODE_OBJECT_ENCODER);
    return o ? container_of(o, drm_encoder_t, base) : NULL;
}

/* ── Connectors ──────────────────────────────────────────────────────────── */

drm_connector_t *drm_connector_create(drm_device_t *dev, u32 type, drm_encoder_t *enc)
{
    drm_connector_t *conn = (drm_connector_t *)kzalloc(sizeof(drm_connector_t));
    if (!conn) return NULL;

    conn->dev            = dev;
    conn->connector_type = type;
    conn->status         = DRM_MODE_CONNECTED;
    conn->subpixel       = DRM_MODE_SUBPIXEL_HORIZONTAL_RGB;
    conn->encoder        = enc;
    /* A 16:9 24" panel unless the driver knows better; clients use this only
     * to compute DPI. */
    conn->mm_width       = 531;
    conn->mm_height      = 298;

    /* connector_type_id counts instances of the same connector type. */
    u32 same = 0;
    for (drm_connector_t *c = dev->connector_list; c; c = c->next) {
        if (c->connector_type == type) same++;
    }
    conn->connector_type_id = same + 1;

    drm_mode_object_add(dev, &conn->base, DRM_MODE_OBJECT_CONNECTOR);
    DRM_LIST_APPEND(dev->connector_list, conn);
    dev->num_connector++;
    return conn;
}

drm_connector_t *drm_connector_find(drm_device_t *dev, u32 id)
{
    drm_mode_object_t *o = drm_mode_object_find(dev, id, DRM_MODE_OBJECT_CONNECTOR);
    return o ? container_of(o, drm_connector_t, base) : NULL;
}

int drm_connector_add_mode(drm_connector_t *conn, const drm_display_mode_t *mode)
{
    if (!conn || !mode) return -EINVAL;
    if (conn->nmodes >= DRM_MAX_MODES) return -ENOSPC;
    conn->modes[conn->nmodes++] = *mode;
    return 0;
}

/* ── Standard mode table ─────────────────────────────────────────────────── */
/*
 * A conventional VESA/CEA ladder.  Timings are the standard ones for each
 * resolution at 60 Hz; emulated adapters ignore them, but clients that log
 * or filter modes expect them to be self-consistent.
 */
static const drm_display_mode_t g_standard_modes[] = {
    { 148500, 1920, 2008, 2052, 2200, 0, 1080, 1084, 1089, 1125, 0, 60,
      DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC, DRM_MODE_TYPE_DRIVER, "1920x1080" },
    { 108000, 1600, 1664, 1728, 1800, 0,  900,  901,  904, 1000, 0, 60,
      DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC, DRM_MODE_TYPE_DRIVER, "1600x900" },
    {  79500, 1440, 1520, 1672, 1904, 0,  900,  903,  909,  934, 0, 60,
      DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC, DRM_MODE_TYPE_DRIVER, "1440x900" },
    {  74250, 1280, 1344, 1480, 1680, 0,  800,  801,  804,  831, 0, 60,
      DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC, DRM_MODE_TYPE_DRIVER, "1280x800" },
    {  65000, 1024, 1048, 1184, 1344, 0,  768,  771,  777,  806, 0, 60,
      DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC, DRM_MODE_TYPE_DRIVER, "1024x768" },
    {  40000,  800,  840,  968, 1056, 0,  600,  601,  605,  628, 0, 60,
      DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC, DRM_MODE_TYPE_DRIVER, "800x600" },
    {  25175,  640,  656,  752,  800, 0,  480,  490,  492,  525, 0, 60,
      DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC, DRM_MODE_TYPE_DRIVER, "640x480" },
};

int drm_connector_add_default_modes(drm_connector_t *conn,
                                    u32 max_w, u32 max_h,
                                    u32 pref_w, u32 pref_h)
{
    if (!conn) return -EINVAL;
    int added = 0;

    for (u32 i = 0; i < ARRAY_SIZE(g_standard_modes); i++) {
        const drm_display_mode_t *m = &g_standard_modes[i];
        if (max_w && m->hdisplay > max_w) continue;
        if (max_h && m->vdisplay > max_h) continue;

        drm_display_mode_t mode = *m;
        if (m->hdisplay == pref_w && m->vdisplay == pref_h) {
            mode.type |= DRM_MODE_TYPE_PREFERRED;
        }
        if (drm_connector_add_mode(conn, &mode) == 0) added++;
    }

    /* A panel whose native size is not on the ladder still needs its own mode
     * listed first, or clients will pick something the hardware cannot show. */
    bool have_pref = false;
    for (u32 i = 0; i < conn->nmodes; i++) {
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { have_pref = true; break; }
    }
    if (!have_pref && pref_w && pref_h) {
        drm_display_mode_t mode;
        drm_mode_simple(&mode, pref_w, pref_h, 60);
        mode.type |= DRM_MODE_TYPE_PREFERRED;

        if (conn->nmodes < DRM_MAX_MODES) {
            for (u32 i = conn->nmodes; i > 0; i--) conn->modes[i] = conn->modes[i - 1];
            conn->modes[0] = mode;
            conn->nmodes++;
            added++;
        } else {
            conn->modes[0] = mode;
        }
    }
    return added;
}

void drm_mode_simple(drm_display_mode_t *mode, u32 w, u32 h, u32 refresh)
{
    memset(mode, 0, sizeof(*mode));

    /* Blanking intervals roughly following GTF: ~20% horizontal, ~5% vertical.
     * Emulated scanout ignores them; they only need to be plausible. */
    mode->hdisplay    = (u16)w;
    mode->hsync_start = (u16)(w + w / 20);
    mode->hsync_end   = (u16)(w + w / 10);
    mode->htotal      = (u16)(w + w / 5);
    mode->vdisplay    = (u16)h;
    mode->vsync_start = (u16)(h + 3);
    mode->vsync_end   = (u16)(h + 9);
    mode->vtotal      = (u16)(h + h / 20 + 10);
    mode->vrefresh    = refresh;
    mode->clock       = (u32)(((u64)mode->htotal * mode->vtotal * refresh) / 1000);
    mode->flags       = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
    mode->type        = DRM_MODE_TYPE_DRIVER;
    snprintf(mode->name, sizeof(mode->name), "%ux%u", w, h);
}

/* ── Planes ──────────────────────────────────────────────────────────────── */

drm_plane_t *drm_plane_create(drm_device_t *dev, u32 plane_type,
                              u32 possible_crtcs,
                              const u32 *formats, u32 nformats)
{
    drm_plane_t *plane = (drm_plane_t *)kzalloc(sizeof(drm_plane_t));
    if (!plane) return NULL;

    plane->dev            = dev;
    plane->plane_type     = plane_type;
    plane->possible_crtcs = possible_crtcs;

    if (nformats > DRM_MAX_FORMATS) nformats = DRM_MAX_FORMATS;
    for (u32 i = 0; i < nformats; i++) plane->formats[i] = formats[i];
    plane->nformats = nformats;

    for (drm_crtc_t *c = dev->crtc_list; c; c = c->next) {
        if (!(possible_crtcs & (1U << c->index))) continue;
        plane->crtc = c;
        if (plane_type == DRM_PLANE_TYPE_PRIMARY) c->primary = plane;
        if (plane_type == DRM_PLANE_TYPE_CURSOR)  c->cursor  = plane;
        break;
    }

    drm_mode_object_add(dev, &plane->base, DRM_MODE_OBJECT_PLANE);
    DRM_LIST_APPEND(dev->plane_list, plane);
    dev->num_plane++;
    return plane;
}

drm_plane_t *drm_plane_find(drm_device_t *dev, u32 id)
{
    drm_mode_object_t *o = drm_mode_object_find(dev, id, DRM_MODE_OBJECT_PLANE);
    return o ? container_of(o, drm_plane_t, base) : NULL;
}

/* ── Framebuffers ────────────────────────────────────────────────────────── */

drm_framebuffer_t *drm_framebuffer_create(drm_device_t *dev, drm_gem_object_t *obj,
                                          u32 width, u32 height, u32 pitch,
                                          u32 bpp, u32 depth, u32 pixel_format)
{
    if (!dev || !obj) return NULL;

    drm_framebuffer_t *fb = (drm_framebuffer_t *)kzalloc(sizeof(drm_framebuffer_t));
    if (!fb) return NULL;

    fb->dev          = dev;
    fb->obj          = obj;
    fb->width        = width;
    fb->height       = height;
    fb->pitch        = pitch;
    fb->bpp          = bpp;
    fb->depth        = depth;
    fb->pixel_format = pixel_format;
    fb->refcount     = 1;
    drm_gem_object_get(obj);

    drm_mode_object_add(dev, &fb->base, DRM_MODE_OBJECT_FB);
    DRM_LIST_APPEND(dev->fb_list, fb);
    dev->num_fb++;
    return fb;
}

drm_framebuffer_t *drm_framebuffer_find(drm_device_t *dev, u32 id)
{
    drm_mode_object_t *o = drm_mode_object_find(dev, id, DRM_MODE_OBJECT_FB);
    return o ? container_of(o, drm_framebuffer_t, base) : NULL;
}

void drm_framebuffer_put(drm_device_t *dev, drm_framebuffer_t *fb)
{
    if (!dev || !fb) return;
    if (--fb->refcount > 0) return;

    /* Detach from anything still pointing at it, so scanout never follows a
     * dangling framebuffer after RMFB. */
    for (drm_crtc_t *c = dev->crtc_list; c; c = c->next) {
        if (c->fb == fb) { c->fb = NULL; c->enabled = false; }
    }
    for (drm_plane_t *p = dev->plane_list; p; p = p->next) {
        if (p->fb == fb) p->fb = NULL;
    }

    drm_framebuffer_t **pp = &dev->fb_list;
    while (*pp) {
        if (*pp == fb) { *pp = fb->next; break; }
        pp = &(*pp)->next;
    }
    dev->num_fb--;

    drm_gem_object_put(dev, fb->obj);
    kfree(fb);
}
