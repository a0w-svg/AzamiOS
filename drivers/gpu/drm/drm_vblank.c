/* ============================================================================
 * AzamiOS — DRM: the vblank clock, page-flip queue and damage tracking
 * File: drivers/gpu/drm/drm_vblank.c
 *
 * Everything on screen changes at a frame boundary, and this file is what
 * defines one.
 *
 * None of the adapters here (Bochs, VMware SVGA, virtio-gpu, a bootloader
 * framebuffer) raises a scanout interrupt, so vblank is a software clock —
 * the same trick Linux's vkms uses.  A worker thread wakes on every scheduler
 * tick, asks the monotonic clock how much time has passed, and advances each
 * CRTC's frame counter by however many frame periods are due.  On each frame
 * boundary it, in order:
 *
 *     1. swaps in the framebuffer a client queued with PAGE_FLIP,
 *     2. copies out any damage a shadow-buffered driver still owes,
 *     3. queues the flip-complete event on the file that asked for it,
 *     4. wakes everything blocked waiting for that frame.
 *
 * That ordering is what removes tearing and flicker.  A client never draws
 * into the buffer being scanned out: it renders into the one it is not
 * showing, queues a flip, and gets told when the swap actually happened.  The
 * kernel never copies a partial frame to the display either, because copies
 * happen here, between frames, and not from inside an ioctl.
 *
 * ── On the frame period ─────────────────────────────────────────────────────
 * The scheduler tick is the finest interval a kernel thread can wake on, so a
 * period that is not a whole number of ticks would deliver frames in an uneven
 * 10/20 ms pattern — which reads as stutter even when the average rate is
 * right.  The engine therefore snaps each CRTC's period to the nearest whole
 * tick (a 60 Hz mode runs its clock at 50 Hz) and delivers frames exactly on
 * tick boundaries.  Evenly spaced frames look smooth; correctly averaged but
 * jittery ones do not.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include <azami/vsync.h>
#include "drm_core.h"
#include "../../../kernel/sched/sched.h"
#include "../../misc/hpet.h"

#define NS_PER_SEC          1000000000ULL
#define DRM_TICK_NS         10000000ULL      /* one scheduler tick, 10 ms     */
#define DRM_DEFAULT_REFRESH 60U
#define DRM_MAX_CATCHUP     4                /* frames a stall may replay     */

/* A wait that outlives this has hit a stalled or disabled CRTC; returning late
 * is always better than never returning at all. */
#define DRM_WAIT_TIMEOUT_NS (250ULL * 1000000ULL)

static bool g_vblank_running;

/* ── Wait channels ───────────────────────────────────────────────────────────
 * A tiny sleep/wake facility keyed on any kernel address: a CRTC to wait for
 * the next frame on, a drm_file_t to wait for an event on.  Sleepers use a
 * one-tick sleep rather than an indefinite block, so a missed wake-up costs a
 * late return instead of a hang, and a wake-up that does arrive is immediate.
 */
#define DRM_WAIT_SLOTS 32

static struct {
    const void *chan;
    thread_t   *thread;
    bool        used;
} g_waiters[DRM_WAIT_SLOTS];

static spinlock_t g_wait_lock = SPINLOCK_INIT;

void drm_wait_on(const void *chan)
{
    thread_t *self = sched_current_thread();
    int slot = -1;

    if (self) {
        irqflags_t f = spinlock_lock_irqsave(&g_wait_lock);
        for (int i = 0; i < DRM_WAIT_SLOTS; i++) {
            if (!g_waiters[i].used) {
                g_waiters[i].chan   = chan;
                g_waiters[i].thread = self;
                g_waiters[i].used   = true;
                slot = i;
                break;
            }
        }
        spinlock_unlock_irqrestore(&g_wait_lock, f);
    }

    sched_sleep(1);

    if (slot >= 0) {
        irqflags_t f = spinlock_lock_irqsave(&g_wait_lock);
        g_waiters[slot].used   = false;
        g_waiters[slot].chan   = NULL;
        g_waiters[slot].thread = NULL;
        spinlock_unlock_irqrestore(&g_wait_lock, f);
    }
}

void drm_wake(const void *chan)
{
    thread_t *wake[DRM_WAIT_SLOTS];
    int n = 0;

    irqflags_t f = spinlock_lock_irqsave(&g_wait_lock);
    for (int i = 0; i < DRM_WAIT_SLOTS; i++) {
        if (g_waiters[i].used && g_waiters[i].chan == chan) {
            wake[n++] = g_waiters[i].thread;
        }
    }
    spinlock_unlock_irqrestore(&g_wait_lock, f);

    /* Unblocking takes the scheduler lock, so it happens outside ours. */
    for (int i = 0; i < n; i++) sched_unblock(wake[i]);
}

/* ── The clock ───────────────────────────────────────────────────────────── */

u64 drm_now_ns(void)
{
    u64 ns = hpet_available() ? hpet_now_ns() : 0;
    if (ns) return ns;
    return sched_get_ticks() * DRM_TICK_NS;
}

bool drm_vblank_running(void) { return g_vblank_running; }

/* Snap a refresh rate to a whole number of scheduler ticks — see the note at
 * the top of the file on why an even cadence beats an exact one. */
static u64 drm_period_from_refresh(u32 vrefresh)
{
    if (vrefresh == 0) vrefresh = DRM_DEFAULT_REFRESH;

    u64 want  = NS_PER_SEC / vrefresh;
    u64 ticks = (want + DRM_TICK_NS / 2) / DRM_TICK_NS;
    if (ticks == 0) ticks = 1;
    return ticks * DRM_TICK_NS;
}

void drm_vblank_crtc_reset(drm_crtc_t *crtc, const drm_display_mode_t *mode)
{
    if (!crtc) return;

    u64 period = drm_period_from_refresh(mode ? mode->vrefresh : 0);
    if (period == crtc->vblank_period_ns) return;

    crtc->vblank_period_ns = period;
    crtc->last_vblank_ns   = drm_now_ns();

    pr_debug("[DRM] CRTC %u vblank clock: %llu Hz (%llu ms period)\n",
             crtc->base.id,
             (unsigned long long)(NS_PER_SEC / period),
             (unsigned long long)(period / 1000000ULL));
}

/* ── Damage ──────────────────────────────────────────────────────────────── */

/* Two rects that touch or overlap: one covering rectangle beats pushing both. */
static bool drm_rect_mergeable(const drm_rect_t *a, const drm_rect_t *b)
{
    return !(b->x1 > a->x2 || b->x2 < a->x1 ||
             b->y1 > a->y2 || b->y2 < a->y1);
}

static void drm_rect_union(drm_rect_t *a, const drm_rect_t *b)
{
    if (b->x1 < a->x1) a->x1 = b->x1;
    if (b->y1 < a->y1) a->y1 = b->y1;
    if (b->x2 > a->x2) a->x2 = b->x2;
    if (b->y2 > a->y2) a->y2 = b->y2;
}

/* Caller holds dev->lock. */
static void drm_crtc_add_damage_locked(drm_crtc_t *crtc, const drm_rect_t *r)
{
    /* A NULL rectangle means "all of it", which no set of rects can narrow. */
    if (!r) {
        crtc->damage_full  = true;
        crtc->damage_count = 0;
        return;
    }
    if (r->x2 <= r->x1 || r->y2 <= r->y1) return;
    if (crtc->damage_full) return;

    /* Fold into the first rect it touches; the grown rect may now reach a
     * later one, so coalesce the tail too. */
    for (u32 i = 0; i < crtc->damage_count; i++) {
        if (!drm_rect_mergeable(&crtc->damage[i], r)) continue;
        drm_rect_union(&crtc->damage[i], r);
        for (u32 j = i + 1; j < crtc->damage_count; ) {
            if (drm_rect_mergeable(&crtc->damage[i], &crtc->damage[j])) {
                drm_rect_union(&crtc->damage[i], &crtc->damage[j]);
                crtc->damage[j] = crtc->damage[--crtc->damage_count];
            } else {
                j++;
            }
        }
        return;
    }

    if (crtc->damage_count < DRM_MAX_DAMAGE) {
        crtc->damage[crtc->damage_count++] = *r;
        return;
    }

    /* Set is full and nothing merged: collapse everything, plus r, to one
     * bounding box. Still a rectangle — never a whole-screen push. */
    drm_rect_t bb = crtc->damage[0];
    for (u32 i = 1; i < crtc->damage_count; i++) drm_rect_union(&bb, &crtc->damage[i]);
    drm_rect_union(&bb, r);
    crtc->damage[0]    = bb;
    crtc->damage_count = 1;
}

void drm_crtc_add_damage(drm_crtc_t *crtc, const drm_rect_t *r)
{
    if (!crtc) return;

    spinlock_lock(&crtc->dev->lock);
    drm_crtc_add_damage_locked(crtc, r);
    spinlock_unlock(&crtc->dev->lock);
}

/* Take the pending damage and clear it. Copies up to DRM_MAX_DAMAGE rects into
 * @out, returns the count; *full is set when the whole framebuffer is owed.
 * Count 0 with *full false means nothing changed. Caller holds dev->lock. */
static u32 drm_crtc_take_damage(drm_crtc_t *crtc, drm_rect_t *out, bool *full)
{
    *full = crtc->damage_full;
    u32 n = crtc->damage_count;
    for (u32 i = 0; i < n; i++) out[i] = crtc->damage[i];
    crtc->damage_full  = false;
    crtc->damage_count = 0;
    return n;
}

/* Run @hook (a driver page_flip or dirty_fb) once per damage rectangle. @flip
 * is true for a page flip, where the scanout target itself changes and the
 * hook must fire at least once even with no damage. Returns the first non-zero
 * hook result, else 0. */
static int drm_present(drm_crtc_t *crtc, drm_framebuffer_t *fb,
                       int (*hook)(drm_crtc_t *, drm_framebuffer_t *, const drm_rect_t *),
                       const drm_rect_t *rects, u32 n, bool full, bool flip)
{
    if (!hook || !fb) return 0;
    if (full || (n == 0 && flip))
        return hook(crtc, fb, NULL);
    int ret = 0;
    for (u32 i = 0; i < n && ret == 0; i++)
        ret = hook(crtc, fb, &rects[i]);
    return ret;
}

int drm_crtc_flush_damage(drm_crtc_t *crtc)
{
    if (!crtc || !crtc->dev->driver->dirty_fb) return 0;
    if (!crtc->fb) return 0;

    drm_device_t *dev = crtc->dev;

    drm_rect_t rects[DRM_MAX_DAMAGE];
    bool full;
    spinlock_lock(&dev->lock);
    u32 n = drm_crtc_take_damage(crtc, rects, &full);
    drm_framebuffer_t *fb = crtc->fb;
    spinlock_unlock(&dev->lock);

    if (!full && n == 0) return 0;
    return drm_present(crtc, fb, dev->driver->dirty_fb, rects, n, full, false);
}

/* ── Page flips ──────────────────────────────────────────────────────────── */

/*
 * Retire a flip: publish the new scanout, tell the client, and release the
 * reference the queued flip was holding.
 *
 * crtc->fb is a plain pointer, not a counted reference — RMFB detaches it —
 * so the reference taken at queue time exists only to keep the buffer alive
 * across the wait, and is dropped here.
 */
static void drm_flip_complete(drm_device_t *dev, drm_crtc_t *crtc,
                              drm_framebuffer_t *fb, drm_file_t *file,
                              u64 user_data, bool want_event, u64 now)
{
    spinlock_lock(&dev->lock);
    crtc->fb = fb;
    if (crtc->primary) crtc->primary->fb = fb;
    spinlock_unlock(&dev->lock);

    if (want_event && file) {
        drm_send_event(file, crtc, DRM_EVENT_FLIP_COMPLETE, user_data,
                       now, crtc->vblank_count);
    }

    drm_framebuffer_put(dev, fb);
}

int drm_vblank_queue_flip(drm_crtc_t *crtc, drm_file_t *file,
                          drm_framebuffer_t *fb, u64 user_data,
                          bool want_event, bool async)
{
    if (!crtc || !fb) return -EINVAL;
    drm_device_t *dev = crtc->dev;

    /* An asynchronous flip is a request to tear on purpose (Linux's
     * DRM_MODE_PAGE_FLIP_ASYNC), and so is a flip with no worker to defer to. */
    if (async || !g_vblank_running) {
        drm_rect_t rects[DRM_MAX_DAMAGE];
        bool full;
        spinlock_lock(&dev->lock);
        u32 n = drm_crtc_take_damage(crtc, rects, &full);
        spinlock_unlock(&dev->lock);

        int ret = drm_present(crtc, fb, dev->driver->page_flip, rects, n, full, true);
        if (ret != 0) return ret;

        fb->refcount++;
        drm_flip_complete(dev, crtc, fb, file, user_data, want_event, drm_now_ns());
        return 0;
    }

    spinlock_lock(&dev->lock);
    if (crtc->flip_pending) {
        spinlock_unlock(&dev->lock);
        return -EBUSY;      /* one flip in flight, as on every KMS driver */
    }

    fb->refcount++;
    crtc->flip_fb        = fb;
    crtc->flip_file      = file;
    crtc->flip_user_data = user_data;
    crtc->flip_event     = want_event;
    crtc->flip_pending   = true;
    spinlock_unlock(&dev->lock);

    return 0;
}

void drm_vblank_file_closed(drm_device_t *dev, drm_file_t *file)
{
    if (!dev || !file) return;

    for (drm_crtc_t *c = dev->crtc_list; c; c = c->next) {
        spinlock_lock(&dev->lock);
        if (c->flip_file == file) {
            /* Keep the flip — the picture it carries is already what the
             * client wanted on screen — but drop the dead event target. */
            c->flip_file  = NULL;
            c->flip_event = false;
        }
        spinlock_unlock(&dev->lock);
    }
    drm_wake(file);
}

/* ── Waiting for a frame ─────────────────────────────────────────────────── */

int drm_vblank_wait(drm_crtc_t *crtc, u64 target, u64 *out_seq, u64 *out_ns)
{
    if (!crtc) return -EINVAL;

    if (target == 0) target = __atomic_load_n(&crtc->vblank_count, __ATOMIC_ACQUIRE) + 1;

    u64 deadline = drm_now_ns() + DRM_WAIT_TIMEOUT_NS;
    int ret = 0;

    while (__atomic_load_n(&crtc->vblank_count, __ATOMIC_ACQUIRE) < target) {
        if (drm_now_ns() >= deadline) { ret = -EAGAIN; break; }
        drm_wait_on(crtc);
    }

    if (out_seq) *out_seq = __atomic_load_n(&crtc->vblank_count, __ATOMIC_ACQUIRE);
    if (out_ns)  *out_ns  = crtc->last_vblank_ns;
    return ret;
}

/* ── The worker ──────────────────────────────────────────────────────────── */

/* Advance one CRTC to @now, doing the frame's work on every boundary crossed. */
static void drm_vblank_advance(drm_device_t *dev, drm_crtc_t *crtc, u64 now)
{
    if (!crtc->enabled) return;

    if (crtc->vblank_period_ns == 0) {
        drm_vblank_crtc_reset(crtc, crtc->mode_valid ? &crtc->mode : NULL);
    }
    u64 period = crtc->vblank_period_ns;

    if (crtc->last_vblank_ns == 0 || now < crtc->last_vblank_ns) {
        crtc->last_vblank_ns = now;
        return;
    }
    u64 elapsed = (now - crtc->last_vblank_ns) / period;
    if (elapsed == 0) return;

    /* After a long stall (a heavy blit, a paused VM) the counter jumps rather
     * than replaying every missed frame — a client pacing on it should see
     * dropped frames, not a burst of instant ones. */
    if (elapsed > DRM_MAX_CATCHUP) {
        crtc->last_vblank_ns = now;
        elapsed = DRM_MAX_CATCHUP;
    } else {
        crtc->last_vblank_ns += elapsed * period;
    }

    /* Claim the pending work before doing any of it: the copy below can take
     * milliseconds and must not run under the device lock. */
    spinlock_lock(&dev->lock);
    drm_framebuffer_t *fb        = crtc->flip_pending ? crtc->flip_fb : NULL;
    drm_file_t        *file      = crtc->flip_file;
    u64                user_data = crtc->flip_user_data;
    bool               want_ev   = crtc->flip_event;

    crtc->flip_pending   = false;
    crtc->flip_fb        = NULL;
    crtc->flip_file      = NULL;
    crtc->flip_event     = false;

    drm_rect_t rects[DRM_MAX_DAMAGE];
    bool dmg_full;
    u32  dmg_n = drm_crtc_take_damage(crtc, rects, &dmg_full);
    drm_framebuffer_t *cur = crtc->fb;
    spinlock_unlock(&dev->lock);

    if (fb) {
        int ret = drm_present(crtc, fb, dev->driver->page_flip,
                              rects, dmg_n, dmg_full, true);
        __atomic_add_fetch(&crtc->vblank_count, elapsed, __ATOMIC_RELEASE);
        dev->vblank_count += elapsed;

        if (ret == 0) {
            drm_flip_complete(dev, crtc, fb, file, user_data, want_ev,
                              crtc->last_vblank_ns);
        } else {
            /* The swap failed; give the buffer back and tell the client the
             * frame happened anyway so it is never left waiting. */
            drm_framebuffer_put(dev, fb);
            if (want_ev && file) {
                drm_send_event(file, crtc, DRM_EVENT_FLIP_COMPLETE, user_data,
                               crtc->last_vblank_ns, crtc->vblank_count);
            }
        }
    } else {
        /* No flip this frame — push whatever damage a shadow driver still
         * owes, so a client that only ever calls DIRTYFB still gets its
         * updates at a frame boundary instead of mid-scanout. */
        if ((dmg_full || dmg_n) && cur && dev->driver->dirty_fb) {
            drm_present(crtc, cur, dev->driver->dirty_fb,
                        rects, dmg_n, dmg_full, false);
        }
        __atomic_add_fetch(&crtc->vblank_count, elapsed, __ATOMIC_RELEASE);
        dev->vblank_count += elapsed;
    }

    drm_wake(crtc);
}

static void drm_vblank_thread(void)
{
    g_vblank_running = true;

    for (;;) {
        u64 now = drm_now_ns();

        for (u32 i = 0; i < drm_dev_count(); i++) {
            drm_device_t *dev = drm_dev_nth(i);
            if (!dev) continue;
            for (drm_crtc_t *c = dev->crtc_list; c; c = c->next) {
                drm_vblank_advance(dev, c, now);
            }
        }
        sched_sleep(1);
    }
}

void drm_vblank_init(void)
{
    if (g_vblank_running) return;

    /* Give every CRTC a clock before the worker can look at one. */
    for (u32 i = 0; i < drm_dev_count(); i++) {
        drm_device_t *dev = drm_dev_nth(i);
        if (!dev) continue;
        for (drm_crtc_t *c = dev->crtc_list; c; c = c->next) {
            drm_vblank_crtc_reset(c, c->mode_valid ? &c->mode : NULL);
        }
    }

    process_t *kproc = sched_kernel_process();
    if (!kproc) {
        pr_debug("[DRM] no kernel process — vblank engine not started\n");
        return;
    }
    if (!thread_create(kproc, (uintptr_t)drm_vblank_thread, 0, true)) {
        pr_debug("[DRM] vblank worker could not be created\n");
        return;
    }
    pr_debug("[DRM] vblank engine started (%u card%s)\n",
             drm_dev_count(), drm_dev_count() == 1 ? "" : "s");
}

/* ── The vsync clock other subsystems see ────────────────────────────────── */

static drm_crtc_t *vsync_primary(void)
{
    drm_device_t *dev = drm_dev_nth(0);
    if (!dev) return NULL;

    for (drm_crtc_t *c = dev->crtc_list; c; c = c->next) {
        if (c->enabled) return c;
    }
    return dev->crtc_list;
}

bool vsync_available(void)
{
    return g_vblank_running && vsync_primary() != NULL;
}

u64 vsync_count(void)
{
    drm_crtc_t *c = vsync_primary();
    return c ? __atomic_load_n(&c->vblank_count, __ATOMIC_ACQUIRE) : 0;
}

u64 vsync_period_ns(void)
{
    drm_crtc_t *c = vsync_primary();
    return c ? c->vblank_period_ns : 0;
}

u64 vsync_wait(u64 target)
{
    drm_crtc_t *c = vsync_primary();
    if (!c || !g_vblank_running) {
        /* Nothing is driving a display; a tick is the best cadence there is. */
        sched_sleep(1);
        return 0;
    }
    u64 seq = 0;
    drm_vblank_wait(c, target, &seq, NULL);
    return seq;
}
