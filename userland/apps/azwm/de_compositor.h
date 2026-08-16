/* ============================================================================
 * AzamiOS — DE Compositor Extension Header
 * File: userland/apps/azwm/de_compositor.h
 *
 * Public API surface for de_compositor.c.  Include this from azwm/main.c
 * after making the integration changes described in de_compositor.c.
 * ============================================================================ */
#pragma once

#include "de_protocol.h"
#include "compositor.h"

/* ---- Strut entry (private — exposed here for completeness) --------------- */
typedef struct {
    unsigned int wid;
    unsigned int bottom, top, left, right;
    unsigned char active;
} de_strut_t;

/* ---- DE compositor state ------------------------------------------------- */
typedef struct {
    unsigned int  subscribers[AZ_WM_MAX_SUBSCRIBERS];
    unsigned char sub_active[AZ_WM_MAX_SUBSCRIBERS];
    unsigned int  sub_count;
    unsigned char zorder_hints[AZWM_MAX_WINDOWS];
    de_strut_t    struts[DE_STRUT_MAX_ENTRIES];  /* Bug 5 fix: own constant */
    unsigned int  strut_count;
} de_comp_state_t;

/* ---- API ------------------------------------------------------------------ */

/** de_comp_init — zero-initialise DE state.  Call before the event loop. */
void de_comp_init(de_comp_state_t *de);

/** de_comp_broadcast_created — push EVT_WINDOW_CREATED to all subscribers. */
void de_comp_broadcast_created(de_comp_state_t *de, az_window_t *win);

/** de_comp_broadcast_destroyed — push EVT_WINDOW_DESTROYED to all subscribers. */
void de_comp_broadcast_destroyed(de_comp_state_t *de, unsigned int wid);

/** de_comp_broadcast_focus — push EVT_FOCUS_CHANGED to all subscribers. */
void de_comp_broadcast_focus(de_comp_state_t *de,
                              unsigned int prev_wid, unsigned int new_wid);

/** de_comp_enforce_zorder — re-sort window list to honour Z-order band hints. */
void de_comp_enforce_zorder(az_compositor_t *comp, de_comp_state_t *de);

/** de_comp_set_strut — record a strut reservation for a panel window. */
void de_comp_set_strut(de_comp_state_t *de, unsigned int wid,
                        unsigned int bottom, unsigned int top,
                        unsigned int left,   unsigned int right);

/** de_comp_remove_strut — remove a strut reservation when a panel closes. */
void de_comp_remove_strut(de_comp_state_t *de, unsigned int wid);

/**
 * de_comp_handle_message — dispatch one DE-specific IPC message.
 * Returns 1 if compositor should redraw; 0 otherwise.
 */
int de_comp_handle_message(az_compositor_t *comp, de_comp_state_t *de,
                            az_wm_msg_t *msg);
