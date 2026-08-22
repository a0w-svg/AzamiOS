/* ============================================================================
 * AzamiOS Desktop Environment — Compositor Extension
 * File: userland/apps/azwm/de_compositor.c
 *
 * This translation unit extends the main azwm compositor with the server-side
 * logic required to support the DE protocol defined in de_protocol.h:
 *
 *   1. Broadcast subscriber list  -- az_wm_sub_*
 *   2. Z-order hint enforcement   -- de_comp_enforce_zorder()
 *   3. Strut reservation table    -- de_comp_set_strut()
 *   4. App-launch delegation      -- de_comp_launch_app()
 *   5. Extended message dispatch  -- de_comp_handle_message()
 *
 * Integration
 * -----------
 * In azwm/main.c, after compositor_init() call de_comp_init(&de_state).
 * Add the following to the main switch-statement before the default: label:
 *
 *   case AZ_WM_SET_ZORDER_HINT:
 *   case AZ_WM_SUBSCRIBE_EVENTS:
 *   case AZ_WM_UNSUBSCRIBE_EVENTS:
 *   case AZ_WM_LAUNCH_APP:
 *   case AZ_WM_SET_STRUT:
 *       if (de_comp_handle_message(&comp, &de_state, &msg))
 *           redraw_needed = true;
 *       break;
 *
 * Also call de_comp_broadcast_created() / de_comp_broadcast_destroyed() /
 * de_comp_broadcast_focus() from the existing CREATE / DESTROY / FOCUS paths.
 * ============================================================================ */

/* de_compositor.h is the canonical definition source for de_strut_t and
 * de_comp_state_t.  Including it here keeps the layout in sync with any
 * external code (e.g. azwm/main.c) that also includes the header.     */
#include "de_compositor.h"
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"
#include "../shared/de_log.h"

/* de_comp_state_t and de_strut_t are defined in de_compositor.h (included above). */

/* ---- Initialisation ------------------------------------------------------- */

void de_comp_init(de_comp_state_t *de)
{
    unsigned int i;
    de->sub_count   = 0;
    de->strut_count = 0;
    for (i = 0; i < AZ_WM_MAX_SUBSCRIBERS; i++) {
        de->subscribers[i] = 0;
        de->sub_active[i]  = 0;
    }
    for (i = 0; i < AZWM_MAX_WINDOWS; i++) {
        de->zorder_hints[i] = AZ_WM_ZORDER_NORMAL;
    }
    for (i = 0; i < DE_STRUT_MAX_ENTRIES; i++) {
        de->struts[i].active = 0;
    }
}

/* ---- Broadcast helpers ---------------------------------------------------- */

static void broadcast(de_comp_state_t *de, az_wm_msg_t *msg)
{
    unsigned int i;
    for (i = 0; i < AZ_WM_MAX_SUBSCRIBERS; i++) {
        if (!de->sub_active[i]) continue;
        int ret = az_channel_send_nb(de->subscribers[i], (az_ipc_msg_t *)msg);
        if (ret == -32) {
            de->sub_active[i] = 0;
            de->subscribers[i] = 0;
            if (de->sub_count > 0) de->sub_count--;
        }
    }
}

/* ---- Public broadcast entry-points --------------------------------------- */

void de_comp_broadcast_created(de_comp_state_t *de, az_window_t *win)
{
    az_wm_msg_t msg;
    unsigned int j;
    az_wm_evt_created_payload_t *pl;

    memset(&msg, 0, sizeof(msg));
    msg.type = AZ_WM_EVT_WINDOW_CREATED;
    pl = AZ_WM_MSG_EVT_CREATED(&msg);
    pl->wid       = win->wid;
    pl->owner_pid = win->owner_pid;
    pl->x         = win->x;
    pl->y         = win->y;
    pl->w         = win->width;
    pl->h         = win->height;

    for (j = 0; j < 63 && win->title[j]; j++)
        pl->title[j] = win->title[j];
    pl->title[j] = '\0';

    broadcast(de, &msg);
}

void de_comp_broadcast_destroyed(de_comp_state_t *de, unsigned int wid)
{
    az_wm_msg_t msg;
    az_wm_evt_destroyed_payload_t *pl;

    memset(&msg, 0, sizeof(msg));
    msg.type = AZ_WM_EVT_WINDOW_DESTROYED;
    pl = AZ_WM_MSG_EVT_DESTROYED(&msg);
    pl->wid = wid;

    broadcast(de, &msg);
    de_comp_remove_strut(de, wid);
}

void de_comp_broadcast_focus(de_comp_state_t *de,
                              unsigned int prev_wid, unsigned int new_wid)
{
    az_wm_msg_t msg;
    az_wm_evt_focus_payload_t *pl;

    memset(&msg, 0, sizeof(msg));
    msg.type = AZ_WM_EVT_FOCUS_CHANGED;
    pl = AZ_WM_MSG_EVT_FOCUS(&msg);
    pl->prev_wid = prev_wid;
    pl->new_wid  = new_wid;

    broadcast(de, &msg);
}

/* ---- Z-order enforcement ------------------------------------------------- */

/*
 * de_comp_enforce_zorder(comp, de)
 *
 * Called after every window create / raise / focus change event.
 * Enforces the Z-order band hints stored in de->zorder_hints[] by walking
 * the compositor window doubly-linked list and repositioning windows:
 *
 *   ZORDER_BOTTOM  -> always at list tail  (drawn first = behind everything)
 *   ZORDER_TOP     -> always at list head  (drawn last  = in front of everything)
 *   ZORDER_NORMAL  -> undisturbed middle section
 *
 * Complexity: O(n^2), n <= AZWM_MAX_WINDOWS = 32 — acceptable.
 */
void de_comp_enforce_zorder(az_compositor_t *comp, de_comp_state_t *de)
{
    az_window_t *win, *nxt;

    az_window_t *top_head = 0, *top_tail = 0;
    az_window_t *norm_head = 0, *norm_tail = 0;
    az_window_t *bot_head = 0, *bot_tail = 0;

    win = comp->list_head;
    while (win) {
        nxt = win->next;
        win->prev = 0;
        win->next = 0;

        /* Bug 2 fix: only skip truly unallocated slots (wid == 0).
         * Invisible (minimized) windows still belong in their z-band so
         * they are not lost from the list and can be restored later. */
        if (win->wid == 0) {
            win = nxt;
            continue;
        }

        int slot = (int)(win - comp->window_pool);
        unsigned char hint = AZ_WM_ZORDER_NORMAL;
        if (slot >= 0 && slot < AZWM_MAX_WINDOWS) {
            hint = de->zorder_hints[slot];
        }

        if (hint == AZ_WM_ZORDER_TOP) {
            if (!top_head) {
                top_head = win;
                top_tail = win;
            } else {
                top_tail->next = win;
                win->prev = top_tail;
                top_tail = win;
            }
        } else if (hint == AZ_WM_ZORDER_BOTTOM) {
            if (!bot_head) {
                bot_head = win;
                bot_tail = win;
            } else {
                bot_tail->next = win;
                win->prev = bot_tail;
                bot_tail = win;
            }
        } else {
            if (!norm_head) {
                norm_head = win;
                norm_tail = win;
            } else {
                norm_tail->next = win;
                win->prev = norm_tail;
                norm_tail = win;
            }
        }

        win = nxt;
    }

    comp->list_head = 0;
    comp->list_tail = 0;

    if (top_head) {
        comp->list_head = top_head;
        comp->list_tail = top_tail;
    }

    if (norm_head) {
        if (!comp->list_head) {
            comp->list_head = norm_head;
            comp->list_tail = norm_tail;
        } else {
            comp->list_tail->next = norm_head;
            norm_head->prev = comp->list_tail;
            comp->list_tail = norm_tail;
        }
    }

    if (bot_head) {
        if (!comp->list_head) {
            comp->list_head = bot_head;
            comp->list_tail = bot_tail;
        } else {
            comp->list_tail->next = bot_head;
            bot_head->prev = comp->list_tail;
            comp->list_tail = bot_tail;
        }
    }
}

/* ---- Strut management ---------------------------------------------------- */

void de_comp_set_strut(de_comp_state_t *de, unsigned int wid,
                        unsigned int bottom, unsigned int top,
                        unsigned int left,   unsigned int right)
{
    unsigned int i;
    for (i = 0; i < DE_STRUT_MAX_ENTRIES; i++) {
        if (de->struts[i].active && de->struts[i].wid == wid) {
            de->struts[i].bottom = bottom;
            de->struts[i].top    = top;
            de->struts[i].left   = left;
            de->struts[i].right  = right;
            return;
        }
    }
    for (i = 0; i < DE_STRUT_MAX_ENTRIES; i++) {
        if (!de->struts[i].active) {
            de->struts[i].wid    = wid;
            de->struts[i].bottom = bottom;
            de->struts[i].top    = top;
            de->struts[i].left   = left;
            de->struts[i].right  = right;
            de->struts[i].active = 1;
            de->strut_count++;
            return;
        }
    }
    puts("[azwm/de] WARNING: strut table full");
}

void de_comp_remove_strut(de_comp_state_t *de, unsigned int wid)
{
    unsigned int i;
    for (i = 0; i < DE_STRUT_MAX_ENTRIES; i++) {
        if (de->struts[i].active && de->struts[i].wid == wid) {
            de->struts[i].active = 0;
            if (de->strut_count > 0) de->strut_count--;
            return;
        }
    }
}

/* ---- Extended message dispatch ------------------------------------------- */

/*
 * de_comp_handle_message(comp, de, msg)
 *
 * Handle one DE-specific IPC message from a client.
 * Returns 1 if the compositor should redraw; 0 otherwise.
 */
int de_comp_handle_message(az_compositor_t *comp, de_comp_state_t *de,
                            az_wm_msg_t *msg)
{
    switch (msg->type) {

    case AZ_WM_SET_ZORDER_HINT: {
        az_wm_zorder_payload_t *pl = AZ_WM_MSG_ZORDER(msg);
        unsigned int i;
        for (i = 0; i < AZWM_MAX_WINDOWS; i++) {
            if (comp->window_pool[i].wid == pl->wid) {
                de->zorder_hints[i] = pl->band;
                de_comp_enforce_zorder(comp, de);
                return 1;
            }
        }
        break;
    }

    case AZ_WM_SUBSCRIBE_EVENTS: {
        az_wm_subscribe_payload_t *pl = AZ_WM_MSG_SUBSCRIBE(msg);
        unsigned int i;
        for (i = 0; i < AZ_WM_MAX_SUBSCRIBERS; i++) {
            if (de->sub_active[i] &&
                de->subscribers[i] == pl->subscriber_chan)
                return 0;  /* Already subscribed */
        }
        for (i = 0; i < AZ_WM_MAX_SUBSCRIBERS; i++) {
            if (!de->sub_active[i]) {
                de->subscribers[i] = pl->subscriber_chan;
                de->sub_active[i]  = 1;
                de->sub_count++;
                de_log("[azwm/de] Broadcast subscriber added");
                return 0;
            }
        }
        de_log("[azwm/de] WARNING: subscriber table full");
        break;
    }

    case AZ_WM_UNSUBSCRIBE_EVENTS: {
        az_wm_subscribe_payload_t *pl = AZ_WM_MSG_SUBSCRIBE(msg);
        unsigned int i;
        for (i = 0; i < AZ_WM_MAX_SUBSCRIBERS; i++) {
            if (de->sub_active[i] &&
                de->subscribers[i] == pl->subscriber_chan) {
                de->sub_active[i] = 0;
                if (de->sub_count > 0) de->sub_count--;
                return 0;
            }
        }
        break;
    }

    case AZ_WM_SET_STRUT: {
        az_wm_strut_payload_t *pl = AZ_WM_MSG_STRUT(msg);
        de_comp_set_strut(de, pl->wid,
                          pl->bottom, pl->top, pl->left, pl->right);
        return 0;
    }

    case AZ_WM_LAUNCH_APP: {
        az_wm_launch_payload_t *pl = AZ_WM_MSG_LAUNCH(msg);
        /* Defensive null-terminate */
        pl->path[AZ_WM_LAUNCH_PATH_MAX - 1] = '\0';
        de_log_fmt("[azwm/de] Launching: ", pl->path);
        if (az_spawn(pl->path) < 0)
            de_log("[azwm/de] ERROR: az_spawn failed");
        return 0;
    }

    case AZ_WM_SET_THEME: {
        az_wm_theme_payload_t *pl = AZ_WM_MSG_THEME(msg);
        az_wm_msg_t bmsg;
        memset(&bmsg, 0, sizeof(bmsg));
        bmsg.type = AZ_WM_EVT_THEME_CHANGED;
        AZ_WM_MSG_THEME(&bmsg)->theme_id = pl->theme_id;
        broadcast(de, &bmsg);
        return 1;
    }

    default:
        break;
    }

    return 0;
}
