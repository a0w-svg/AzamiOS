/* ============================================================================
 * AzamiOS — DRM: atomic modesetting (property objects, blobs, ATOMIC commit)
 * File: drivers/gpu/drm/drm_atomic.c
 *
 * A minimal but real KMS property model, built once in the core so every
 * driver gets it for free: a small, fixed table of well-known properties
 * (FB_ID, CRTC_ID, CRTC_X/Y/W/H, ACTIVE, MODE_ID, ...) whose *values* read
 * and write straight through to the native crtc_t/plane_t/connector_t fields
 * legacy SETCRTC/SETPLANE/CURSOR already use — there is no generic per-object
 * property-value store to keep in sync with those fields, because there
 * isn't a second copy of the state to begin with.
 *
 * DRM_IOCTL_MODE_ATOMIC validates an entire batch of (object, property,
 * value) triples — resolving every object and framebuffer reference and
 * taking whatever transient GEM/fb references the resolved values need —
 * before writing anything, so a request that fails half-way through leaves
 * every object exactly as it found it (real atomicity, not "mostly applied
 * then bailed"). Only once every triple in the batch has validated clean
 * does it get applied, in one pass, followed by one driver call per CRTC
 * touched — a mode_set() if ACTIVE/MODE_ID changed, otherwise a
 * drm_vblank_queue_flip() if a plane's FB_ID did, and cursor_set()/
 * cursor_move() directly for a CURSOR plane, exactly mirroring what
 * SETCRTC/SETPLANE/CURSOR already do for their one object at a time.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "drm_core.h"
#include "../../../kernel/uaccess.h"
#include "../../../kernel/mm/kmalloc.h"
#include "../../../kernel/lib/string.h"

bool g_drm_atomic_enabled = true;

/* ── Well-known properties ───────────────────────────────────────────────── */

typedef enum {
    DRM_PROP_NONE = 0,
    DRM_PROP_CRTC_ACTIVE,
    DRM_PROP_CRTC_MODE_ID,
    DRM_PROP_CONNECTOR_CRTC_ID,
    DRM_PROP_PLANE_FB_ID,
    DRM_PROP_PLANE_CRTC_ID,
    DRM_PROP_PLANE_CRTC_X,
    DRM_PROP_PLANE_CRTC_Y,
    DRM_PROP_PLANE_CRTC_W,
    DRM_PROP_PLANE_CRTC_H,
    DRM_PROP_PLANE_SRC_X,
    DRM_PROP_PLANE_SRC_Y,
    DRM_PROP_PLANE_SRC_W,
    DRM_PROP_PLANE_SRC_H,
    DRM_PROP_PLANE_TYPE,
} drm_prop_id_t;

/* What a property's value refers to — plain range/enum, or another object
 * this code has to resolve (and, for FB, take a reference on) before the
 * value is usable. */
typedef enum {
    REF_NONE,     /* plain integer, RANGE-flagged                          */
    REF_SIGNED,   /* plain integer, transmitted as the bit pattern of an s32 */
    REF_FB,       /* names a drm_framebuffer_t (0 = none)                   */
    REF_CRTC,     /* names a drm_crtc_t (0 = none)                          */
    REF_BLOB,     /* names a drm_blob_t (0 = none)                         */
    REF_IMMUTABLE,/* read-only; SETPROPERTY/ATOMIC may not change it        */
} drm_prop_ref_t;

typedef struct {
    u32            id;
    const char    *name;
    u32            applies_to;   /* DRM_MODE_OBJECT_*                       */
    u32            uapi_flags;   /* DRM_MODE_PROP_* reported to userspace   */
    drm_prop_ref_t ref;
    u64            min, max;     /* meaningful for REF_NONE/REF_SIGNED      */
} drm_prop_def_t;

static const drm_prop_def_t g_props[] = {
    { DRM_PROP_CRTC_ACTIVE,       "ACTIVE",  DRM_MODE_OBJECT_CRTC,      DRM_MODE_PROP_RANGE,  REF_NONE,   0, 1 },
    { DRM_PROP_CRTC_MODE_ID,      "MODE_ID", DRM_MODE_OBJECT_CRTC,      DRM_MODE_PROP_BLOB,   REF_BLOB,   0, 0 },
    { DRM_PROP_CONNECTOR_CRTC_ID, "CRTC_ID", DRM_MODE_OBJECT_CONNECTOR, DRM_MODE_PROP_OBJECT, REF_CRTC,   0, 0 },
    { DRM_PROP_PLANE_FB_ID,       "FB_ID",   DRM_MODE_OBJECT_PLANE,     DRM_MODE_PROP_OBJECT, REF_FB,     0, 0 },
    { DRM_PROP_PLANE_CRTC_ID,     "CRTC_ID", DRM_MODE_OBJECT_PLANE,     DRM_MODE_PROP_OBJECT, REF_CRTC,   0, 0 },
    { DRM_PROP_PLANE_CRTC_X,      "CRTC_X",  DRM_MODE_OBJECT_PLANE,     DRM_MODE_PROP_RANGE,  REF_SIGNED, 0, 0 },
    { DRM_PROP_PLANE_CRTC_Y,      "CRTC_Y",  DRM_MODE_OBJECT_PLANE,     DRM_MODE_PROP_RANGE,  REF_SIGNED, 0, 0 },
    { DRM_PROP_PLANE_CRTC_W,      "CRTC_W",  DRM_MODE_OBJECT_PLANE,     DRM_MODE_PROP_RANGE,  REF_NONE,   0, 0x7fffffff },
    { DRM_PROP_PLANE_CRTC_H,      "CRTC_H",  DRM_MODE_OBJECT_PLANE,     DRM_MODE_PROP_RANGE,  REF_NONE,   0, 0x7fffffff },
    { DRM_PROP_PLANE_SRC_X,       "SRC_X",   DRM_MODE_OBJECT_PLANE,     DRM_MODE_PROP_RANGE,  REF_NONE,   0, 0xffffffff },
    { DRM_PROP_PLANE_SRC_Y,       "SRC_Y",   DRM_MODE_OBJECT_PLANE,     DRM_MODE_PROP_RANGE,  REF_NONE,   0, 0xffffffff },
    { DRM_PROP_PLANE_SRC_W,       "SRC_W",   DRM_MODE_OBJECT_PLANE,     DRM_MODE_PROP_RANGE,  REF_NONE,   0, 0xffffffff },
    { DRM_PROP_PLANE_SRC_H,       "SRC_H",   DRM_MODE_OBJECT_PLANE,     DRM_MODE_PROP_RANGE,  REF_NONE,   0, 0xffffffff },
    { DRM_PROP_PLANE_TYPE,        "type",    DRM_MODE_OBJECT_PLANE,     DRM_MODE_PROP_RANGE,  REF_IMMUTABLE, 0, 2 },
};
#define NUM_PROPS ((u32)(sizeof(g_props) / sizeof(g_props[0])))

static const drm_prop_def_t *prop_find(u32 id)
{
    for (u32 i = 0; i < NUM_PROPS; i++) if (g_props[i].id == id) return &g_props[i];
    return NULL;
}

/* s32 <-> u64 the same way every 16.16-or-plain signed KMS property does:
 * the wire value is the bit pattern of the s32, sign-extended back out. */
static u64 s32_to_u64(s32 v) { return (u64)(s64)v; }
static s32 u64_to_s32(u64 v) { return (s32)(u32)v; }

/* ── Blobs (CREATEPROPBLOB / DESTROYPROPBLOB / GETPROPBLOB) ─────────────── */

static drm_blob_t *blob_find_get(drm_device_t *dev, u32 id)
{
    if (!dev || id == 0) return NULL;
    spinlock_lock(&dev->lock);
    drm_blob_t *b = NULL;
    for (drm_blob_t *p = dev->blob_list; p; p = p->next) {
        if (p->id == id) { b = p; break; }
    }
    if (b) b->refcount++;
    spinlock_unlock(&dev->lock);
    return b;
}

static void blob_put(drm_device_t *dev, drm_blob_t *b)
{
    if (!dev || !b) return;
    spinlock_lock(&dev->lock);
    if (--b->refcount > 0) { spinlock_unlock(&dev->lock); return; }
    drm_blob_t **pp = &dev->blob_list;
    while (*pp) { if (*pp == b) { *pp = b->next; break; } pp = &(*pp)->next; }
    spinlock_unlock(&dev->lock);
    kfree(b);
}

s64 drm_ioctl_createpropblob(drm_device_t *dev, u64 arg)
{
    struct drm_mode_create_blob req;
    if (copy_from_user(&req, (void *)(uintptr_t)arg, sizeof(req)) != 0) return -(s64)EFAULT;
    if (req.length == 0 || req.length > 4096 || !req.data) return -(s64)EINVAL;

    u32 count = 0;
    spinlock_lock(&dev->lock);
    for (drm_blob_t *p = dev->blob_list; p; p = p->next) count++;
    spinlock_unlock(&dev->lock);
    if (count >= DRM_MAX_BLOBS) return -(s64)ENOSPC;

    drm_blob_t *b = (drm_blob_t *)kzalloc(sizeof(*b) + req.length);
    if (!b) return -(s64)ENOMEM;
    if (copy_from_user(b->data, (void *)(uintptr_t)req.data, req.length) != 0) {
        kfree(b);
        return -(s64)EFAULT;
    }
    b->length   = req.length;
    b->refcount = 1;

    spinlock_lock(&dev->lock);
    b->id             = ++dev->next_blob_id;
    b->next           = dev->blob_list;
    dev->blob_list    = b;
    spinlock_unlock(&dev->lock);

    req.blob_id = b->id;
    if (copy_to_user((void *)(uintptr_t)arg, &req, sizeof(req)) != 0) return -(s64)EFAULT;
    return 0;
}

s64 drm_ioctl_destroypropblob(drm_device_t *dev, u64 arg)
{
    struct drm_mode_destroy_blob req;
    if (copy_from_user(&req, (void *)(uintptr_t)arg, sizeof(req)) != 0) return -(s64)EFAULT;

    drm_blob_t *b = blob_find_get(dev, req.blob_id);
    if (!b) return -(s64)ENOENT;
    blob_put(dev, b);   /* our lookup reference   */
    blob_put(dev, b);   /* the creation reference */
    return 0;
}

s64 drm_ioctl_getpropblob(drm_device_t *dev, u64 arg)
{
    struct drm_mode_get_blob req;
    if (copy_from_user(&req, (void *)(uintptr_t)arg, sizeof(req)) != 0) return -(s64)EFAULT;

    drm_blob_t *b = blob_find_get(dev, req.blob_id);
    if (!b) return -(s64)ENOENT;

    if (req.data && req.length >= b->length) {
        if (copy_to_user((void *)(uintptr_t)req.data, b->data, b->length) != 0) {
            blob_put(dev, b);
            return -(s64)EFAULT;
        }
    }
    req.length = b->length;
    blob_put(dev, b);

    if (copy_to_user((void *)(uintptr_t)arg, &req, sizeof(req)) != 0) return -(s64)EFAULT;
    return 0;
}

/* ── Object resolution ────────────────────────────────────────────────────
 * Unlike drm_mode_object_find(), this only ever sees CRTC/PLANE/CONNECTOR
 * ids — the static lists a plain unlocked walk is safe against — so it can
 * reuse the ordinary drm_*_find() wrappers directly. */

static drm_mode_object_t *obj_resolve(drm_device_t *dev, u32 obj_id, u32 obj_type)
{
    switch (obj_type) {
    case DRM_MODE_OBJECT_CRTC:      { drm_crtc_t      *o = drm_crtc_find(dev, obj_id);      return o ? &o->base : NULL; }
    case DRM_MODE_OBJECT_PLANE:     { drm_plane_t     *o = drm_plane_find(dev, obj_id);     return o ? &o->base : NULL; }
    case DRM_MODE_OBJECT_CONNECTOR: { drm_connector_t *o = drm_connector_find(dev, obj_id); return o ? &o->base : NULL; }
    default: return NULL;
    }
}

/* ── GETPROPERTY / OBJ_GETPROPERTIES ─────────────────────────────────────── */

static u64 prop_get_value(drm_mode_object_t *obj, u32 obj_type, const drm_prop_def_t *def)
{
    switch (obj_type) {
    case DRM_MODE_OBJECT_CRTC: {
        drm_crtc_t *crtc = container_of(obj, drm_crtc_t, base);
        if (def->id == DRM_PROP_CRTC_ACTIVE)  return crtc->enabled ? 1 : 0;
        if (def->id == DRM_PROP_CRTC_MODE_ID) return 0;   /* blobs are opaque ids we don't retain */
        break;
    }
    case DRM_MODE_OBJECT_CONNECTOR: {
        drm_connector_t *conn = container_of(obj, drm_connector_t, base);
        if (def->id == DRM_PROP_CONNECTOR_CRTC_ID) {
            return (conn->encoder && conn->encoder->crtc) ? conn->encoder->crtc->base.id : 0;
        }
        break;
    }
    case DRM_MODE_OBJECT_PLANE: {
        drm_plane_t *plane = container_of(obj, drm_plane_t, base);
        switch (def->id) {
        case DRM_PROP_PLANE_FB_ID:   return plane->fb   ? plane->fb->base.id   : 0;
        case DRM_PROP_PLANE_CRTC_ID: return plane->crtc ? plane->crtc->base.id : 0;
        case DRM_PROP_PLANE_CRTC_X:  return s32_to_u64(plane->crtc_x);
        case DRM_PROP_PLANE_CRTC_Y:  return s32_to_u64(plane->crtc_y);
        case DRM_PROP_PLANE_CRTC_W:  return plane->crtc_w;
        case DRM_PROP_PLANE_CRTC_H:  return plane->crtc_h;
        case DRM_PROP_PLANE_SRC_X: case DRM_PROP_PLANE_SRC_Y:
        case DRM_PROP_PLANE_SRC_W: case DRM_PROP_PLANE_SRC_H:
            return 0;   /* not tracked — see the SRC_* note in the header comment */
        case DRM_PROP_PLANE_TYPE:    return plane->plane_type;
        }
        break;
    }
    }
    return 0;
}

s64 drm_ioctl_obj_getproperties(drm_device_t *dev, u64 arg)
{
    struct drm_mode_obj_get_properties req;
    if (copy_from_user(&req, (void *)(uintptr_t)arg, sizeof(req)) != 0) return -(s64)EFAULT;

    drm_mode_object_t *obj = obj_resolve(dev, req.obj_id, req.obj_type);
    if (!obj) return -(s64)ENOENT;

    u32 ids[NUM_PROPS];
    u64 vals[NUM_PROPS];
    u32 n = 0;
    for (u32 i = 0; i < NUM_PROPS; i++) {
        if (g_props[i].applies_to != req.obj_type) continue;
        ids[n]  = g_props[i].id;
        vals[n] = prop_get_value(obj, req.obj_type, &g_props[i]);
        n++;
    }

    if (req.props_ptr && req.count_props >= n && n) {
        if (copy_to_user((void *)(uintptr_t)req.props_ptr, ids, n * sizeof(u32)) != 0)
            return -(s64)EFAULT;
    }
    if (req.prop_values_ptr && req.count_props >= n && n) {
        if (copy_to_user((void *)(uintptr_t)req.prop_values_ptr, vals, n * sizeof(u64)) != 0)
            return -(s64)EFAULT;
    }
    req.count_props = n;

    if (copy_to_user((void *)(uintptr_t)arg, &req, sizeof(req)) != 0) return -(s64)EFAULT;
    return 0;
}

s64 drm_ioctl_getproperty(drm_device_t *dev, u64 arg)
{
    (void)dev;
    struct drm_mode_get_property req;
    if (copy_from_user(&req, (void *)(uintptr_t)arg, sizeof(req)) != 0) return -(s64)EFAULT;

    const drm_prop_def_t *def = prop_find(req.prop_id);
    if (!def) return -(s64)ENOENT;

    memset(req.name, 0, sizeof(req.name));
    strncpy(req.name, def->name, sizeof(req.name) - 1);
    req.flags = def->uapi_flags;

    u64 range[2] = { def->min, def->max };
    if (req.values_ptr && req.count_values >= 2 && (def->uapi_flags & DRM_MODE_PROP_RANGE)) {
        if (copy_to_user((void *)(uintptr_t)req.values_ptr, range, sizeof(range)) != 0)
            return -(s64)EFAULT;
    }
    req.count_values     = (def->uapi_flags & DRM_MODE_PROP_RANGE) ? 2 : 0;
    req.count_enum_blobs = 0;

    if (copy_to_user((void *)(uintptr_t)arg, &req, sizeof(req)) != 0) return -(s64)EFAULT;
    return 0;
}

/* ── Validate + apply a single (object, property, value) ─────────────────
 *
 * OBJ_SETPROPERTY is exactly a one-triple atomic commit without the
 * TEST_ONLY/ALLOW_MODESET ceremony, so it is built on the same
 * resolve-validate-apply helpers ATOMIC uses, with a batch of one.
 */

#define DRM_ATOMIC_MAX_OPS 64

typedef struct {
    drm_mode_object_t *obj;
    u32                 obj_type;
    const drm_prop_def_t *def;
    u64                 value;      /* validated, ready to apply verbatim   */
    drm_framebuffer_t  *fb_ref;     /* held reference, for REF_FB props     */
    drm_blob_t         *blob_ref;   /* held reference, for REF_BLOB props   */
} drm_atomic_op_t;

/* Resolve and validate one triple; on success @out is filled in and, for a
 * REF_FB property, holds a reference that drm_atomic_release() must drop.
 * Nothing is written to @out->obj yet — that is apply()'s job, once every
 * op in the whole batch has validated clean. */
static int atomic_stage(drm_device_t *dev, u32 obj_id, u32 obj_type, u32 prop_id,
                        u64 value, bool allow_modeset, drm_atomic_op_t *out)
{
    drm_mode_object_t *obj = obj_resolve(dev, obj_id, obj_type);
    if (!obj) return -ENOENT;

    const drm_prop_def_t *def = prop_find(prop_id);
    if (!def || def->applies_to != obj_type) return -EINVAL;
    if (def->ref == REF_IMMUTABLE) return -EACCES;
    if ((def->id == DRM_PROP_CRTC_ACTIVE || def->id == DRM_PROP_CRTC_MODE_ID) && !allow_modeset)
        return -EINVAL;   /* Linux: ALLOW_MODESET is required to touch these */

    out->obj      = obj;
    out->obj_type = obj_type;
    out->def      = def;
    out->fb_ref   = NULL;
    out->blob_ref = NULL;

    switch (def->ref) {
    case REF_NONE:
        if (value < def->min || value > def->max) return -EINVAL;
        out->value = value;
        return 0;
    case REF_SIGNED:
        out->value = value;   /* range on x/y is "the whole plane fits somewhere"; nothing to reject */
        return 0;
    case REF_CRTC: {
        drm_crtc_t *target_crtc = value ? drm_crtc_find(dev, (u32)value) : NULL;
        if (value != 0 && !target_crtc) return -ENOENT;
        if (def->id == DRM_PROP_PLANE_CRTC_ID && target_crtc) {
            drm_plane_t *plane = container_of(obj, drm_plane_t, base);
            if (!(plane->possible_crtcs & (1U << target_crtc->index))) return -EINVAL;
        }
        /* A connector's CRTC_ID is fixed at whichever CRTC its encoder was
         * bound to in the driver's load() (see drm_encoder_create()) — this
         * simple, single-headed pipeline model has no way to rewire that at
         * runtime. Rather than accept any other value and silently do
         * nothing with it (which used to be this case's behaviour), refuse
         * it outright: an atomic client that actually depends on the
         * reassignment taking effect deserves an error, not silent success. */
        if (def->id == DRM_PROP_CONNECTOR_CRTC_ID) {
            drm_connector_t *conn = container_of(obj, drm_connector_t, base);
            u32 fixed = (conn->encoder && conn->encoder->crtc) ? conn->encoder->crtc->base.id : 0;
            if (value != fixed) return -EINVAL;
        }
        out->value = value;
        return 0;
    }
    case REF_BLOB:
        /* Held from here through apply() exactly like a REF_FB's fb_ref —
         * without it, a concurrent DESTROYPROPBLOB between this check and
         * apply() dereferencing the blob's contents would free it out from
         * under this commit, the very TOCTOU shape the rest of this
         * session's work exists to close everywhere else. */
        if (value != 0) {
            out->blob_ref = blob_find_get(dev, (u32)value);
            if (!out->blob_ref) return -ENOENT;
        }
        out->value = value;
        return 0;
    case REF_FB:
        if (value == 0) { out->value = 0; return 0; }
        out->fb_ref = drm_framebuffer_find(dev, (u32)value);
        if (!out->fb_ref) return -ENOENT;
        out->value = value;
        return 0;
    default:
        return -EINVAL;
    }
}

static void atomic_release(drm_device_t *dev, drm_atomic_op_t *ops, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        if (ops[i].fb_ref)   drm_framebuffer_put(dev, ops[i].fb_ref);
        if (ops[i].blob_ref) blob_put(dev, ops[i].blob_ref);
    }
}

/* Apply one already-validated op to its object's native field. Called only
 * once the whole batch has staged clean, so this cannot itself fail. */
static void atomic_apply_one(drm_device_t *dev, drm_atomic_op_t *op)
{
    switch (op->obj_type) {
    case DRM_MODE_OBJECT_CRTC: {
        drm_crtc_t *crtc = container_of(op->obj, drm_crtc_t, base);
        if (op->def->id == DRM_PROP_CRTC_ACTIVE) {
            crtc->enabled = op->value != 0;
        } else if (op->def->id == DRM_PROP_CRTC_MODE_ID) {
            if (op->value == 0 || !op->blob_ref) {
                crtc->mode_valid = false;
            } else if (op->blob_ref->length >= sizeof(struct drm_mode_modeinfo)) {
                struct drm_mode_modeinfo umode;
                memcpy(&umode, op->blob_ref->data, sizeof(umode));
                drm_display_mode_t mode;
                memset(&mode, 0, sizeof(mode));
                mode.clock = umode.clock;
                mode.hdisplay = umode.hdisplay; mode.hsync_start = umode.hsync_start;
                mode.hsync_end = umode.hsync_end; mode.htotal = umode.htotal; mode.hskew = umode.hskew;
                mode.vdisplay = umode.vdisplay; mode.vsync_start = umode.vsync_start;
                mode.vsync_end = umode.vsync_end; mode.vtotal = umode.vtotal; mode.vscan = umode.vscan;
                mode.vrefresh = umode.vrefresh; mode.flags = umode.flags; mode.type = umode.type;
                memcpy(mode.name, umode.name, sizeof(mode.name) - 1);
                crtc->mode       = mode;
                crtc->mode_valid = true;
            }
        }
        break;
    }
    case DRM_MODE_OBJECT_PLANE: {
        drm_plane_t *plane = container_of(op->obj, drm_plane_t, base);
        switch (op->def->id) {
        case DRM_PROP_PLANE_FB_ID:
            /* fb_ref (if any) is dropped by atomic_release() once the whole
             * commit — including the driver calls below — is done; plane->fb
             * itself stays the bare pointer it always was. */
            plane->fb = op->fb_ref;
            break;
        case DRM_PROP_PLANE_CRTC_ID:
            plane->crtc = op->value ? drm_crtc_find(dev, (u32)op->value) : NULL;
            break;
        case DRM_PROP_PLANE_CRTC_X: plane->crtc_x = u64_to_s32(op->value); break;
        case DRM_PROP_PLANE_CRTC_Y: plane->crtc_y = u64_to_s32(op->value); break;
        case DRM_PROP_PLANE_CRTC_W: plane->crtc_w = (u32)op->value; break;
        case DRM_PROP_PLANE_CRTC_H: plane->crtc_h = (u32)op->value; break;
        default: break;   /* SRC_* accepted, not stored — see header comment */
        }
        break;
    }
    /* Connectors: CRTC_ID is read-only here (single fixed pipeline; see
     * atomic_stage()'s REF_CRTC case, which already refuses a value other
     * than the connector's current one before this is ever reached). */
    default: break;
    }
}

/* Once every op is applied, push each touched CRTC to its driver exactly
 * once — a mode_set() if ACTIVE/MODE_ID moved, else a queued flip if a
 * PRIMARY plane's FB_ID did, else a direct cursor_set()/cursor_move() for a
 * CURSOR plane. Mirrors SETCRTC/SETPLANE/CURSOR's own driver calls. */
static void atomic_commit_crtc(drm_device_t *dev, drm_file_t *file, drm_crtc_t *crtc,
                               bool touched_mode, bool touched_primary_fb,
                               bool touched_cursor, u64 user_data, bool want_event)
{
    if (touched_mode) {
        int ret = 0;
        if (dev->driver->mode_set) {
            ret = dev->driver->mode_set(crtc, crtc->enabled && crtc->primary ? crtc->primary->fb : NULL,
                                        crtc->mode_valid ? &crtc->mode : NULL, crtc->x, crtc->y);
        }
        if (ret == 0) {
            if (crtc->enabled && crtc->mode_valid) {
                drm_vblank_crtc_reset(crtc, &crtc->mode);
                crtc->damage_count = 0;
                crtc->damage_full  = false;
            }
            if (want_event) {
                drm_send_event(file, crtc, DRM_EVENT_FLIP_COMPLETE, user_data,
                               drm_now_ns(), crtc->vblank_count);
            }
        }
        return;
    }
    if (touched_primary_fb && crtc->primary && crtc->primary->fb && crtc->enabled) {
        drm_vblank_queue_flip(crtc, file, crtc->primary->fb, user_data, want_event, false);
    }
    if (touched_cursor && crtc->cursor) {
        drm_gem_object_t *bo = crtc->cursor->fb ? crtc->cursor->fb->obj : NULL;
        if (dev->driver->cursor_set) {
            dev->driver->cursor_set(crtc, bo, crtc->cursor->fb ? crtc->cursor->fb->width : 0,
                                    crtc->cursor->fb ? crtc->cursor->fb->height : 0);
        }
        if (dev->driver->cursor_move) {
            dev->driver->cursor_move(crtc, crtc->cursor->crtc_x, crtc->cursor->crtc_y);
        }
    }
}

s64 drm_ioctl_obj_setproperty(drm_device_t *dev, drm_file_t *file, u64 arg)
{
    if (!drm_can_modeset(file)) return -(s64)EACCES;
    if (!g_drm_atomic_enabled) return -(s64)EOPNOTSUPP;

    struct drm_mode_obj_set_property req;
    if (copy_from_user(&req, (void *)(uintptr_t)arg, sizeof(req)) != 0) return -(s64)EFAULT;

    drm_atomic_op_t op;
    int ret = atomic_stage(dev, req.obj_id, req.obj_type, req.prop_id, req.value, true, &op);
    if (ret != 0) return (s64)ret;

    atomic_apply_one(dev, &op);

    bool mode_touched = (op.def->id == DRM_PROP_CRTC_ACTIVE || op.def->id == DRM_PROP_CRTC_MODE_ID);
    bool fb_touched    = (op.def->id == DRM_PROP_PLANE_FB_ID);
    if (op.obj_type == DRM_MODE_OBJECT_CRTC && mode_touched) {
        atomic_commit_crtc(dev, file, container_of(op.obj, drm_crtc_t, base), true, false, false, 0, false);
    } else if (op.obj_type == DRM_MODE_OBJECT_PLANE && fb_touched) {
        drm_plane_t *plane = container_of(op.obj, drm_plane_t, base);
        if (plane->crtc) {
            bool is_cursor = plane->plane_type == DRM_PLANE_TYPE_CURSOR;
            atomic_commit_crtc(dev, file, plane->crtc, false, !is_cursor, is_cursor, 0, false);
        }
    }

    atomic_release(dev, &op, 1);
    return 0;
}

s64 drm_ioctl_atomic(drm_device_t *dev, drm_file_t *file, u64 arg)
{
    if (!drm_can_modeset(file)) return -(s64)EACCES;
    if (!g_drm_atomic_enabled) return -(s64)EOPNOTSUPP;

    struct drm_mode_atomic req;
    if (copy_from_user(&req, (void *)(uintptr_t)arg, sizeof(req)) != 0) return -(s64)EFAULT;
    if (req.count_objs == 0) return 0;
    if (req.count_objs > DRM_ATOMIC_MAX_OPS) return -(s64)E2BIG;

    bool test_only     = (req.flags & DRM_MODE_ATOMIC_TEST_ONLY) != 0;
    bool allow_modeset = (req.flags & DRM_MODE_ATOMIC_ALLOW_MODESET) != 0;
    bool want_event    = !test_only;   /* see the doc comment on this in drm_core.h's ATOMIC declaration */

    u32 obj_ids[DRM_ATOMIC_MAX_OPS];
    u32 nprops[DRM_ATOMIC_MAX_OPS];
    if (copy_from_user(obj_ids, (void *)(uintptr_t)req.objs_ptr, req.count_objs * sizeof(u32)) != 0)
        return -(s64)EFAULT;
    if (copy_from_user(nprops, (void *)(uintptr_t)req.count_props_ptr, req.count_objs * sizeof(u32)) != 0)
        return -(s64)EFAULT;

    u32 total = 0;
    for (u32 i = 0; i < req.count_objs; i++) {
        if (nprops[i] > DRM_ATOMIC_MAX_OPS - total) return -(s64)E2BIG;
        total += nprops[i];
    }
    if (total == 0) return 0;

    u32 *prop_ids  = (u32 *)kzalloc(total * sizeof(u32));
    u64 *prop_vals = (u64 *)kzalloc(total * sizeof(u64));
    if (!prop_ids || !prop_vals) {
        kfree(prop_ids); kfree(prop_vals);
        return -(s64)ENOMEM;
    }
    if (copy_from_user(prop_ids, (void *)(uintptr_t)req.props_ptr, total * sizeof(u32)) != 0 ||
        copy_from_user(prop_vals, (void *)(uintptr_t)req.prop_values_ptr, total * sizeof(u64)) != 0) {
        kfree(prop_ids); kfree(prop_vals);
        return -(s64)EFAULT;
    }

    drm_atomic_op_t ops[DRM_ATOMIC_MAX_OPS];
    u32 nops = 0;
    s64 err  = 0;

    u32 idx = 0;
    for (u32 i = 0; i < req.count_objs && err == 0; i++) {
        /* Object type isn't in the request — resolve it by probing each
         * kind, same as a real client would already know it from a prior
         * GETRESOURCES/GETPLANERESOURCES. */
        u32 obj_type = DRM_MODE_OBJECT_CRTC;
        if (!obj_resolve(dev, obj_ids[i], obj_type)) {
            obj_type = DRM_MODE_OBJECT_PLANE;
            if (!obj_resolve(dev, obj_ids[i], obj_type)) {
                obj_type = DRM_MODE_OBJECT_CONNECTOR;
                if (!obj_resolve(dev, obj_ids[i], obj_type)) { err = -ENOENT; break; }
            }
        }
        for (u32 j = 0; j < nprops[i]; j++, idx++) {
            int ret = atomic_stage(dev, obj_ids[i], obj_type, prop_ids[idx], prop_vals[idx],
                                   allow_modeset, &ops[nops]);
            if (ret != 0) { err = ret; break; }
            nops++;
        }
    }

    kfree(prop_ids);
    kfree(prop_vals);

    if (err != 0) {
        atomic_release(dev, ops, nops);
        return err;
    }
    if (test_only) {
        atomic_release(dev, ops, nops);
        return 0;
    }

    for (u32 i = 0; i < nops; i++) atomic_apply_one(dev, &ops[i]);

    /* One driver call per distinct CRTC actually touched by this batch —
     * directly, a plane's crtc, or (for a connector) its fixed encoder. */
    for (drm_crtc_t *crtc = dev->crtc_list; crtc; crtc = crtc->next) {
        bool mode_touched = false, fb_touched = false, cursor_touched = false;
        for (u32 i = 0; i < nops; i++) {
            if (ops[i].obj_type == DRM_MODE_OBJECT_CRTC && &crtc->base == ops[i].obj &&
                (ops[i].def->id == DRM_PROP_CRTC_ACTIVE || ops[i].def->id == DRM_PROP_CRTC_MODE_ID)) {
                mode_touched = true;
            }
            if (ops[i].obj_type == DRM_MODE_OBJECT_PLANE && ops[i].def->id == DRM_PROP_PLANE_FB_ID) {
                drm_plane_t *plane = container_of(ops[i].obj, drm_plane_t, base);
                if (plane->crtc == crtc) {
                    if (plane->plane_type == DRM_PLANE_TYPE_CURSOR) cursor_touched = true;
                    else fb_touched = true;
                }
            }
        }
        if (mode_touched || fb_touched || cursor_touched) {
            atomic_commit_crtc(dev, file, crtc, mode_touched, fb_touched, cursor_touched,
                               req.user_data, want_event);
        }
    }

    atomic_release(dev, ops, nops);
    return 0;
}
