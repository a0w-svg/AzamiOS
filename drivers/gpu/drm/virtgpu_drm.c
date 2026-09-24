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

/* Guards the shared img[] staging buffer below. drm_ioctl_cursor() only ever
 * allows this to be called by the current DRM master, and only one file can
 * be master at a time — but a multi-monitor compositor's master fd can still
 * legitimately have more than one thread issuing CURSOR ioctls for different
 * CRTCs at once, and virtio-gpu's cursor queue itself is already shared
 * across every scanout (see virtio_gpu.h). Without this, two concurrent
 * cursor_set() calls could interleave their writes into img[] and each
 * upload the other's half-written image. */
static spinlock_t g_cursor_img_lock = SPINLOCK_INIT;

static int virtgpu_cursor_set(drm_crtc_t *crtc, drm_gem_object_t *bo, u32 w, u32 h)
{
    virtgpu_device_t *vg = (virtgpu_device_t *)crtc->dev->dev_private;
    virtgpu_output_t *out = virtgpu_find_output(vg, crtc);
    if (!out) return -EINVAL;

    if (!bo)
        return virtio_gpu_cursor_hide(out->scanout_id) ? -EIO : 0;

    /* virtio-gpu's hardware cursor is a fixed 64x64 image; a client asking
     * for another size falls back to a software cursor by getting -EINVAL. */
    if ((w && w != VIRTIO_GPU_CURSOR_W) || (h && h != VIRTIO_GPU_CURSOR_H))
        return -EINVAL;

    /* Gather the ARGB8888 image into a linear 64x64 buffer. drm_gem_blit_rect
     * copes with a bo whose backing pages are not contiguous. */
    static u32 img[VIRTIO_GPU_CURSOR_W * VIRTIO_GPU_CURSOR_H];
    drm_rect_t all = { 0, 0, VIRTIO_GPU_CURSOR_W, VIRTIO_GPU_CURSOR_H };

    spinlock_lock(&g_cursor_img_lock);
    __builtin_memset(img, 0, sizeof(img));
    drm_gem_blit_rect(bo, img, VIRTIO_GPU_CURSOR_W * 4, &all, 32);
    /* The device only ever holds one cursor image, shared across every
     * scanout's independent position (see virtio_gpu.h) — every monitor
     * shows the same shape, which is what a compositor actually wants. */
    int ret = virtio_gpu_cursor_define(img, (u32)crtc->cursor_hot_x,
                                       (u32)crtc->cursor_hot_y, out->scanout_id);
    spinlock_unlock(&g_cursor_img_lock);
    return ret ? -EIO : 0;
}

static int virtgpu_cursor_move(drm_crtc_t *crtc, s32 x, s32 y)
{
    virtgpu_device_t *vg = (virtgpu_device_t *)crtc->dev->dev_private;
    virtgpu_output_t *out = virtgpu_find_output(vg, crtc);
    if (!out) return -EINVAL;

    /* MOVE_CURSOR places the hotspot on the scanout; a cursor dragged past
     * the top or left edge is clamped to the origin. */
    u32 ux = x < 0 ? 0u : (u32)x;
    u32 uy = y < 0 ? 0u : (u32)y;
    return virtio_gpu_cursor_move(ux, uy, out->scanout_id) ? -EIO : 0;
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

/* ── Driver-private ioctl handler ────────────────────────────────────────────
 *
 * Nine virtgpu-specific ioctls are defined in include/azami/drm.h. Whether
 * they do real 3D work follows whether the host actually granted
 * VIRTIO_GPU_F_VIRGL at feature negotiation (virtio_gpu_init(), checked live
 * here rather than cached, since GETPARAM is how a client is supposed to
 * find this out too):
 *   - CONTEXT_INIT creates a real host Virgl context (CTX_CREATE) and
 *     remembers it on the file so later ioctls know which context owns
 *     this fd's resources.
 *   - RESOURCE_CREATE allocates a system-memory GEM object and, with 3D
 *     negotiated, also creates the matching host resource
 *     (RESOURCE_CREATE_3D), attaches its guest pages
 *     (RESOURCE_ATTACH_BACKING) and, if a context exists on this file,
 *     attaches the resource to it (CTX_ATTACH_RESOURCE).
 *   - EXECBUFFER submits a real Gallium/Virgl command stream (SUBMIT_3D).
 *   - TRANSFER_TO_HOST moves guest-written bytes into the host's copy of a
 *     3D resource (TRANSFER_TO_HOST_3D) when 3D is live, or falls back to
 *     the 2D path otherwise.
 *   - GET_CAPS queries the host's real capset data (GET_CAPSET_INFO +
 *     GET_CAPSET) when 3D is live, or reports zero bytes otherwise.
 *   - WAIT is still vacuous: this driver's virtqueue round trips are
 *     synchronous end to end, so by the time any ioctl here returns, the
 *     host has already retired the command — there is never an outstanding
 *     fence for a client to wait on.
 *   - MAP, RESOURCE_INFO all delegate to existing helpers.
 *   - GETPARAM reports 3D_FEATURES=1 only when VIRTIO_GPU_F_VIRGL was
 *     actually granted by the host, never unconditionally.
 * ────────────────────────────────────────────────────────────────────────── */

/* Copy helpers local to this file (same macros as drm_ioctl.c). */
#include "../../../kernel/uaccess.h"
#define VGPU_COPY_IN(dst, arg) \
    do { if (copy_from_user((dst), (void *)(uintptr_t)(arg), sizeof(*(dst))) != 0) \
             return -(s64)EFAULT; } while (0)
#define VGPU_COPY_OUT(arg, src) \
    do { if (copy_to_user((void *)(uintptr_t)(arg), (src), sizeof(*(src))) != 0) \
             return -(s64)EFAULT; } while (0)

static s64 virtgpu_ioctl(drm_device_t *dev, drm_file_t *file, u32 cmd, u64 arg)
{
    switch (cmd) {

    case DRM_IOCTL_VIRTGPU_GETPARAM: {
        struct drm_virtgpu_getparam p;
        VGPU_COPY_IN(&p, arg);
        if (p.param == VIRTGPU_PARAM_3D_FEATURES) {
            extern virtio_gpu_state_t g_gpu;
            p.value = (g_gpu.vpci.negotiated_features & (1ULL << VIRTIO_GPU_F_VIRGL)) ? 1 : 0;
        } else {
            return -(s64)EINVAL;
        }
        VGPU_COPY_OUT(arg, &p);
        return 0;
    }

    /* ── DRM_IOCTL_VIRTGPU_CONTEXT_INIT ─────────────────────────────────── */
    case DRM_IOCTL_VIRTGPU_CONTEXT_INIT: {
        struct drm_virtgpu_context_init ci;
        VGPU_COPY_IN(&ci, arg);
        
        extern virtio_gpu_state_t g_gpu;
        if (!(g_gpu.vpci.negotiated_features & (1ULL << VIRTIO_GPU_F_VIRGL))) {
            return -(s64)ENOTSUP;
        }
        
        if (virtio_gpu_cmd_context_create(ci.ctx_id, "azami_ctx") < 0) {
            return -(s64)EIO;
        }

        /* Remember this context on the file: RESOURCE_CREATE below uses it
         * to auto-attach every 3D resource this file creates from here on,
         * and TRANSFER_TO_HOST stamps it into the TRANSFER_TO_HOST_3D
         * command's ctx_id. AzamiOS gives each open DRM fd at most one
         * Virgl context, so there is no ambiguity about which one "this
         * file's context" means. */
        file->virtgpu_ctx_id = ci.ctx_id;
        file->virtgpu_ctx_valid = true;

        VGPU_COPY_OUT(arg, &ci);
        return 0;
    }

    /* ── DRM_IOCTL_VIRTGPU_MAP ────────────────────────────────────────────
     * Return the mmap() offset for a GEM handle, exactly as MODE_MAP_DUMB
     * does for dumb buffers.  The offset is assigned at object-creation time
     * by drm_gem_object_create() and is stable for the object's lifetime. */
    case DRM_IOCTL_VIRTGPU_MAP: {
        struct drm_virtgpu_map m;
        VGPU_COPY_IN(&m, arg);
        drm_gem_object_t *obj = drm_gem_handle_lookup(file, m.handle);
        if (!obj) return -(s64)ENOENT;
        m.offset = obj->mmap_offset;
        VGPU_COPY_OUT(arg, &m);
        return 0;
    }

    /* ── DRM_IOCTL_VIRTGPU_RESOURCE_CREATE ───────────────────────────────
     * Allocate a GEM buffer object representing a new virtio-gpu resource.
     * The wire protocol fields (target/format/bind/depth/array_size/…) are
     * stored for RESOURCE_INFO but otherwise forwarded to the host as-is
     * in a full 3D implementation; in 2D-only mode we just hand back a
     * system-memory shadow buffer and a synthetic res_handle. */
    case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE: {
        struct drm_virtgpu_resource_create rc;
        VGPU_COPY_IN(&rc, arg);

        if (rc.width == 0 || rc.height == 0) return -(s64)EINVAL;

        /* stride = width * 4 (BGRA8888); size = stride * height */
        u32 stride = rc.width * 4;
        drm_gem_object_t *obj = drm_gem_object_create(dev, rc.width, rc.height,
                                                      32, stride);
        if (!obj) return -(s64)ENOMEM;

        u32 handle = drm_gem_handle_create(file, obj);
        if (!handle) {
            drm_gem_object_put(dev, obj);
            return -(s64)ENOMEM;
        }

        /* If 3D is enabled, create the matching host-side resource, hand it
         * the guest pages backing this GEM object (individually allocated —
         * see drm_gem_object_create() — so this is a real scatter-gather
         * list, not the single-entry shortcut the 2D framebuffer path uses),
         * and, if this file already has a Virgl context, attach the
         * resource to it so the context's command stream may reference it. */
        extern virtio_gpu_state_t g_gpu;
        if (g_gpu.vpci.negotiated_features & (1ULL << VIRTIO_GPU_F_VIRGL)) {
            if (virtio_gpu_cmd_resource_create_3d(handle, rc.target, rc.format, rc.bind,
                                                  rc.width, rc.height, rc.depth, rc.array_size) < 0) {
                pr_debug("[VIRTIO-GPU-DRM] RESOURCE_CREATE_3D failed for handle %u\n", handle);
            } else if (virtio_gpu_resource_attach_backing_pages(handle, obj->pages, obj->npages,
                                                                 PAGE_SIZE) < 0) {
                pr_debug("[VIRTIO-GPU-DRM] attach_backing failed for resource %u\n", handle);
            } else if (file->virtgpu_ctx_valid &&
                      virtio_gpu_cmd_context_attach_resource(file->virtgpu_ctx_id, handle) < 0) {
                pr_debug("[VIRTIO-GPU-DRM] ctx %u attach resource %u failed\n",
                         file->virtgpu_ctx_id, handle);
            }
        }

        /* drm_gem_handle_create() took a reference; drop the creation ref. */
        drm_gem_object_put(dev, obj);

        rc.bo_handle  = handle;
        rc.res_handle = handle;   /* synthetic: 1-to-1 with the GEM handle */
        rc.size       = (u32)obj->size;
        rc.stride     = stride;
        VGPU_COPY_OUT(arg, &rc);
        return 0;
    }

    /* ── DRM_IOCTL_VIRTGPU_RESOURCE_INFO ─────────────────────────────────
     * Return the res_handle, byte size and stride for an existing GEM handle. */
    case DRM_IOCTL_VIRTGPU_RESOURCE_INFO: {
        struct drm_virtgpu_resource_info ri;
        VGPU_COPY_IN(&ri, arg);
        drm_gem_object_t *obj = drm_gem_handle_lookup(file, ri.bo_handle);
        if (!obj) return -(s64)ENOENT;
        ri.res_handle = ri.bo_handle;   /* synthetic: same value */
        ri.size       = (u32)obj->size;
        ri.stride     = obj->pitch;
        VGPU_COPY_OUT(arg, &ri);
        return 0;
    }

    /* ── DRM_IOCTL_VIRTGPU_EXECBUFFER ────────────────────────────────────
     * In a full virglrenderer implementation this submits a Gallium command
     * stream to a 3D context. */
    case DRM_IOCTL_VIRTGPU_EXECBUFFER: {
        struct drm_virtgpu_execbuffer ex;
        VGPU_COPY_IN(&ex, arg);
        
        extern virtio_gpu_state_t g_gpu;
        if (!(g_gpu.vpci.negotiated_features & (1ULL << VIRTIO_GPU_F_VIRGL))) {
            return -(s64)ENOTSUP;
        }
        
        void *buf = kmalloc(ex.size);
        if (!buf) return -(s64)ENOMEM;
        
        if (copy_from_user(buf, (void*)(uintptr_t)ex.command, ex.size) != 0) {
            kfree(buf);
            return -(s64)EFAULT;
        }
        
        int ret = virtio_gpu_cmd_submit_3d(ex.ring_idx, buf, ex.size);
        kfree(buf);
        return ret < 0 ? -(s64)EIO : 0;
    }

    /* ── DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST ──────────────────────────────
     * Copy a 3D box from guest backing pages into the host resource.  The
     * 2D driver only supports flat (z=0, d=1) resources; it maps the box
     * to a TRANSFER_TO_HOST_2D rectangle, which the existing helper already
     * implements with the correct offset calculation. */
    case DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST: {
        struct drm_virtgpu_3d_transfer xfer;
        VGPU_COPY_IN(&xfer, arg);
        drm_gem_object_t *obj = drm_gem_handle_lookup(file, xfer.bo_handle);
        if (!obj) return -(s64)ENOENT;

        /* Derive the resource id: synthetic, equals the GEM handle. */
        u32 res_id = xfer.bo_handle;

        extern virtio_gpu_state_t g_gpu;
        if (g_gpu.vpci.negotiated_features & (1ULL << VIRTIO_GPU_F_VIRGL)) {
            /* Real 3D transfer: the guest pages attached to this resource
             * (RESOURCE_ATTACH_BACKING, done at RESOURCE_CREATE time above)
             * get copied into the host's copy of the resource within the
             * caller's box, at the caller's stride/level. This is what
             * makes vertex/index/uniform data written into a 3D resource by
             * the guest actually visible to the host's Gallium driver — the
             * 2D-only TRANSFER_TO_HOST_2D this used to fall back to has no
             * concept of a 3D box, level or context and cannot target a
             * VIRGL resource at all. */
            u32 ctx_id = file->virtgpu_ctx_valid ? file->virtgpu_ctx_id : 0;
            int ret = virtio_gpu_cmd_transfer_to_host_3d(
                          ctx_id, res_id,
                          xfer.box.x, xfer.box.y, xfer.box.z,
                          xfer.box.w, xfer.box.h, xfer.box.d,
                          xfer.offset, xfer.level, xfer.stride, xfer.layer_stride);
            return ret < 0 ? -(s64)EIO : 0;
        }

        u64 off = (u64)xfer.box.y * obj->pitch + (u64)xfer.box.x * 4;
        int ret = virtio_gpu_transfer_to_host_2d_rect(
                      res_id, xfer.box.x, xfer.box.y,
                      xfer.box.w, xfer.box.h, off + xfer.offset);
        return ret < 0 ? -(s64)EIO : 0;
    }

    /* ── DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST ────────────────────────────
     * Read the host's copy of a 3D resource back into guest backing pages —
     * how a render target, a query result or any other host-computed data
     * actually becomes visible to the guest. The 2D device has no
     * TRANSFER_FROM_HOST_2D command, so without 3D negotiated this stays a
     * no-op (the backing pages already hold the last guest-rendered
     * content, which is the only content a 2D resource ever has). */
    case DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST: {
        struct drm_virtgpu_3d_transfer xfer;
        VGPU_COPY_IN(&xfer, arg);

        extern virtio_gpu_state_t g_gpu;
        if (!(g_gpu.vpci.negotiated_features & (1ULL << VIRTIO_GPU_F_VIRGL)))
            return 0;

        drm_gem_object_t *obj = drm_gem_handle_lookup(file, xfer.bo_handle);
        if (!obj) return -(s64)ENOENT;

        u32 ctx_id = file->virtgpu_ctx_valid ? file->virtgpu_ctx_id : 0;
        int ret = virtio_gpu_cmd_transfer_from_host_3d(
                      ctx_id, xfer.bo_handle,
                      xfer.box.x, xfer.box.y, xfer.box.z,
                      xfer.box.w, xfer.box.h, xfer.box.d,
                      xfer.offset, xfer.level, xfer.stride, xfer.layer_stride);
        return ret < 0 ? -(s64)EIO : 0;
    }

    /* ── DRM_IOCTL_VIRTGPU_WAIT ──────────────────────────────────────────
     * Wait for an async fence on a resource.  No async engine in 2D mode;
     * all operations are synchronous, so this is always satisfied. */
    case DRM_IOCTL_VIRTGPU_WAIT:
        return 0;

    /* ── DRM_IOCTL_VIRTGPU_GET_CAPS ──────────────────────────────────────
     * Return a real virglrenderer capability set fetched from the host via
     * GET_CAPSET_INFO (find the capset index whose id matches what the
     * client asked for) + GET_CAPSET (fetch its data). With no 3D feature
     * negotiated there is nothing to query; keep the old zero-fill so a
     * caller that probes GET_CAPS before checking GETPARAM still gets a
     * well-defined "no caps" answer instead of an error. */
    case DRM_IOCTL_VIRTGPU_GET_CAPS: {
        struct drm_virtgpu_get_caps gc;
        VGPU_COPY_IN(&gc, arg);

        extern virtio_gpu_state_t g_gpu;
        if (!(g_gpu.vpci.negotiated_features & (1ULL << VIRTIO_GPU_F_VIRGL))) {
            if (gc.addr && gc.size) {
                u32 n = gc.size;
                if (n > 4096) n = 4096;
                u8 zero = 0;
                for (u32 i = 0; i < n; i++)
                    copy_to_user((void *)(uintptr_t)(gc.addr + i), &zero, 1);
            }
            gc.size = 0;
            VGPU_COPY_OUT(arg, &gc);
            return 0;
        }

        /* The host doesn't index capsets by id — GET_CAPSET_INFO enumerates
         * them 0..num_capsets-1 and each slot reports its own id. Probe a
         * small bounded range (real hosts report 1-3 capsets: virgl,
         * virgl2, and sometimes venus/cross-domain) until the id matches or
         * the host answers capset_id 0 (past the end of its list). */
        u32 max_version = 0, max_size = 0;
        bool found = false;
        for (u32 idx = 0; idx < 8; idx++) {
            u32 id = 0, ver = 0, sz = 0;
            if (virtio_gpu_cmd_get_capset_info(idx, &id, &ver, &sz) < 0) break;
            if (id == 0) break;   /* past the end of the host's capset list */
            if (id == gc.cap_set_id) {
                max_version = ver;
                max_size = sz;
                found = true;
                break;
            }
        }
        if (!found) return -(s64)EINVAL;

        u32 want = gc.size;
        if (want > max_size) want = max_size;

        u32 got = 0;
        if (want > 0) {
            u8 *capbuf = kzalloc(want);
            if (!capbuf) return -(s64)ENOMEM;

            u32 ver = gc.cap_set_ver;
            if (ver > max_version) ver = max_version;

            if (virtio_gpu_cmd_get_capset(gc.cap_set_id, ver, capbuf, want, &got) < 0) {
                kfree(capbuf);
                return -(s64)EIO;
            }
            if (gc.addr)
                copy_to_user((void *)(uintptr_t)gc.addr, capbuf, got);
            kfree(capbuf);
        }

        gc.size = got;
        VGPU_COPY_OUT(arg, &gc);
        return 0;
    }

    default:
        return -(s64)EINVAL;
    }
}

/* ── Device bring-up ─────────────────────────────────────────────────────── */

static int virtgpu_load(drm_device_t *dev)
{
    virtgpu_device_t *vg = (virtgpu_device_t *)dev->dev_private;

    dev->min_width     = 64;
    dev->max_width     = 8192;
    dev->min_height    = 64;
    dev->max_height    = 8192;
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

        /* A cursor plane on every scanout, so universal-plane and atomic
         * clients see the hardware cursor the legacy CURSOR ioctl drives
         * through .cursor_set/.cursor_move — virtio_gpu_cursor_define()/
         * _move() now take the scanout id, so a second (or third, ...)
         * monitor gets a real hardware cursor instead of none at all. */
        drm_plane_create(dev, DRM_PLANE_TYPE_CURSOR, 1U << crtc->index,
                         cursor_formats, ARRAY_SIZE(cursor_formats));

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
    virtgpu_device_t *vg = (virtgpu_device_t *)dev->dev_private;
    if (!vg) return;

    /* Tear down every scanout resource on the host so it can reclaim VRAM.
     * Sequence for each output that was set up:
     *   1. Detach scanout (SET_SCANOUT with resource_id 0) so the host
     *      compositor stops compositing from this resource.
     *   2. Detach guest backing pages (RESOURCE_DETACH_BACKING).
     *   3. Free the host resource (RESOURCE_UNREF).
     * Scanout 0's resource is the global framebuffer (g_gpu.resource_id);
     * it is shared with fbdev.c which is torn down first, so we do not
     * touch its backing pages — just unref the host resource so the host
     * knows we're done with it. */
    for (u32 i = 0; i < vg->num_outputs; i++) {
        virtgpu_output_t *out = &vg->outputs[i];
        if (out->resource_id == 0) continue;

        /* Disconnect from the scanout head. */
        virtio_gpu_set_scanout(out->scanout_id, 0, 0, 0);

        /* For extra scanouts (resource_id > 1) we own the backing allocation
         * and must detach it before unreffing. Scanout 0's backing is the
         * global g_gpu framebuffer, managed separately. */
        if (out->scanout_id != 0)
            virtio_gpu_resource_detach_backing(out->resource_id);

        virtio_gpu_resource_unref(out->resource_id);
    }

    /* Tear down the shared hardware cursor resource if it was ever used. */
    extern virtio_gpu_state_t g_gpu;
    if (g_gpu.cursor_res_id) {
        virtio_gpu_resource_detach_backing(g_gpu.cursor_res_id);
        virtio_gpu_resource_unref(g_gpu.cursor_res_id);
        g_gpu.cursor_res_id = 0;
        g_gpu.cursor_virt   = NULL;
        g_gpu.cursor_phys   = 0;
    }

    kfree(vg);
    dev->dev_private = NULL;
}

static const drm_driver_t virtgpu_drm_driver = {
    .name      = "virtio_gpu",
    .desc      = "VirtIO GPU display (2D + Virgl 3D when negotiated)",
    .date      = "20260914",
    .major     = 0, .minor = 3, .patchlevel = 0,
    .features  = DRIVER_MODESET | DRIVER_GEM | DRIVER_RENDER | DRIVER_ATOMIC,
    .load      = virtgpu_load,
    .unload    = virtgpu_unload,
    .mode_set    = virtgpu_mode_set,
    .page_flip   = virtgpu_flush,
    .dirty_fb    = virtgpu_flush,
    .cursor_set  = virtgpu_cursor_set,
    .cursor_move = virtgpu_cursor_move,
    .ioctl       = virtgpu_ioctl,
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
