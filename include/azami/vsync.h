/* ============================================================================
 * AzamiOS — Display vertical-sync clock
 * File: include/azami/vsync.h
 *
 * The one place in the kernel that answers "when does the next frame start?".
 * It is fed by the DRM vblank engine (drivers/gpu/drm/drm_vblank.c), which
 * owns the timing, so /dev/fb0 and /dev/dri/cardN pace themselves against the
 * same clock and can never fight over the scanout mid-frame.
 *
 * Anything that puts pixels on screen should present between frames rather
 * than during one:
 *
 *     vsync_wait(0);            // block until the next frame boundary
 *     flip_to(next_buffer);     // swap what the display scans out
 *
 * With no display registered the calls still work — vsync_wait() falls back to
 * a plain tick sleep — so a caller never needs to special-case headless.
 * ============================================================================ */
#pragma once

#include "types.h"

/** vsync_available() — true once a display with a running vblank clock exists. */
bool vsync_available(void);

/** vsync_count() — frames elapsed on the primary CRTC since boot. */
u64  vsync_count(void);

/** vsync_period_ns() — nanoseconds between frames, 0 if unknown. */
u64  vsync_period_ns(void);

/**
 * vsync_wait(target) — block until vsync_count() reaches @target.
 *
 * @target of 0 means "the next frame boundary".  Returns the frame number
 * actually reached.  The wait is bounded, so a stalled display degrades into
 * a late return rather than a hung caller.
 */
u64  vsync_wait(u64 target);
