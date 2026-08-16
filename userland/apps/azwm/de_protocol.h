/* ============================================================================
 * AzamiOS — Desktop Environment Extended Protocol
 * File: userland/apps/azwm/de_protocol.h
 *
 * Defines the supplemental IPC message types exchanged between the Desktop
 * Environment suite (wallpaper, taskbar) and the azwm Display Server.
 *
 * All messages reuse the existing az_wm_msg_t envelope that already fits
 * within the 256-byte IPC_MSG_MAX_SIZE limit.  New type values are chosen
 * well above the currently-used range (1–21) to avoid collisions.
 *
 * Z-order model
 * ─────────────
 *   The compositor's doubly-linked window list is ordered front→back.
 *   A window may request a permanent Z-order "hint" that the compositor
 *   honours after every create / raise / focus event:
 *
 *     AZ_WM_ZORDER_BOTTOM (0)  — always at tail (wallpaper layer)
 *     AZ_WM_ZORDER_NORMAL (1)  — default application layer
 *     AZ_WM_ZORDER_TOP    (2)  — always at head (panel / overlay layer)
 *
 * Broadcast model
 * ───────────────
 *   The compositor maintains a small broadcast subscriber list.  Any client
 *   may register with AZ_WM_SUBSCRIBE_EVENTS; the server then pushes
 *   AZ_WM_EVT_WINDOW_CREATED / AZ_WM_EVT_WINDOW_DESTROYED frames to all
 *   subscribers whenever a window is created or destroyed.
 *
 * App-launch model
 * ────────────────
 *   AZ_WM_LAUNCH_APP asks the server process (which runs as a privileged
 *   helper capable of calling az_spawn) to fork-exec a named ELF binary on
 *   behalf of the requesting client (e.g. the taskbar).
 * ============================================================================ */
#pragma once

#include "protocol.h"   /* az_wm_msg_t, az_wm_msg_type */

/* ── Supplemental message type constants ─────────────────────────────────── */
/* NOTE: enum extension is achieved via plain #defines to stay compatible with
 *       the existing az_wm_msg_type enum without re-declaring it.            */

/* Client -> Server */
#define AZ_WM_SET_ZORDER_HINT      30  /* Set permanent Z-order band for a window   */
#define AZ_WM_SUBSCRIBE_EVENTS     31  /* Subscribe to broadcast window events      */
#define AZ_WM_UNSUBSCRIBE_EVENTS   32  /* Cancel broadcast subscription             */
#define AZ_WM_LAUNCH_APP           33  /* Ask server to spawn an ELF binary         */
#define AZ_WM_SET_STRUT            34  /* Reserve screen edge (panel geometry)      */

/* Server -> Client (broadcasts) */
#define AZ_WM_EVT_WINDOW_CREATED   40  /* Broadcast: a new window was created       */
#define AZ_WM_EVT_WINDOW_DESTROYED 41  /* Broadcast: a window was destroyed         */
#define AZ_WM_EVT_FOCUS_CHANGED    42  /* Broadcast: focus moved to another window  */
#define AZ_WM_SESSION_READY        50  /* sessiond -> subscribers: DE fully started */
#define AZ_WM_TIMER_TICK           51  /* Kernel timer -> client: periodic tick     */

/* ── Z-order band values (sent in set_zorder.band) ──────────────────────── */
#define AZ_WM_ZORDER_BOTTOM   0   /* Wallpaper / root window */
#define AZ_WM_ZORDER_NORMAL   1   /* Regular application     */
#define AZ_WM_ZORDER_TOP      2   /* Panel / always-on-top   */

/* ── Maximum length of a path passed in AZ_WM_LAUNCH_APP ────────────────── */
#define AZ_WM_LAUNCH_PATH_MAX  128

/* ── Maximum broadcast subscribers ──────────────────────────────────────── */
#define AZ_WM_MAX_SUBSCRIBERS  8

/* ── Strut table size (independent of broadcast subscriber limit) ──────── */
#define DE_STRUT_MAX_ENTRIES   16

/* ── Cursor sprite dimensions (shared between desktop.c and compositor.c) */
#define AZ_WM_CURSOR_W  12
#define AZ_WM_CURSOR_H  19

/* ── Maximum windows tracked by the taskbar ─────────────────────────────── */
#define DE_TASKBAR_MAX_WINDOWS  24

/* ── Payload overlays for the az_wm_msg_t._raw[200] union region ─────────── *
 *
 * These structs are cast from/into msg._raw[] -- all fit within 200 bytes.
 * They are declared here for use in both the server extension and the DE
 * client apps.
 * ─────────────────────────────────────────────────────────────────────────── */

/* AZ_WM_SET_ZORDER_HINT payload (client -> server) */
typedef struct {
    unsigned int  wid;    /* Window whose hint to set                    */
    unsigned char band;   /* AZ_WM_ZORDER_BOTTOM / NORMAL / TOP          */
    unsigned char _pad[3];
} az_wm_zorder_payload_t;

/* AZ_WM_SUBSCRIBE_EVENTS payload (client -> server) */
typedef struct {
    unsigned int subscriber_chan;  /* Channel the server shall push events to */
} az_wm_subscribe_payload_t;

/* AZ_WM_SET_STRUT payload (client -> server)
 *   Informs the compositor that this window permanently reserves a strip of
 *   the screen (like a panel), so it is excluded from normal window placement
 *   and maximise calculations. */
typedef struct {
    unsigned int wid;          /* Window this strut belongs to              */
    unsigned int bottom;       /* Pixels reserved at the bottom of screen   */
    unsigned int top;          /* Pixels reserved at the top                */
    unsigned int left;         /* Pixels reserved on the left               */
    unsigned int right;        /* Pixels reserved on the right              */
} az_wm_strut_payload_t;

/* AZ_WM_LAUNCH_APP payload (client -> server) */
typedef struct {
    char path[AZ_WM_LAUNCH_PATH_MAX];  /* Absolute path to ELF binary       */
} az_wm_launch_payload_t;

/* AZ_WM_EVT_WINDOW_CREATED broadcast payload (server -> subscriber) */
typedef struct {
    unsigned int  wid;          /* New window ID                             */
    unsigned int  owner_pid;    /* PID of the owning process                 */
    int           x, y;         /* Initial screen position                   */
    unsigned int  w, h;         /* Dimensions                                */
    char          title[64];    /* Window title (may be truncated)           */
} az_wm_evt_created_payload_t;

/* AZ_WM_EVT_WINDOW_DESTROYED broadcast payload (server -> subscriber) */
typedef struct {
    unsigned int  wid;          /* Destroyed window ID                       */
} az_wm_evt_destroyed_payload_t;

/* AZ_WM_EVT_FOCUS_CHANGED broadcast payload (server -> subscriber) */
typedef struct {
    unsigned int  prev_wid;     /* Previously focused window (0 if none)     */
    unsigned int  new_wid;      /* Newly focused window (0 if none)          */
} az_wm_evt_focus_payload_t;

/* ── Convenience cast helpers ─────────────────────────────────────────────── */
#define AZ_WM_MSG_ZORDER(msg_ptr) \
    ((az_wm_zorder_payload_t *)((msg_ptr)->_raw))

#define AZ_WM_MSG_SUBSCRIBE(msg_ptr) \
    ((az_wm_subscribe_payload_t *)((msg_ptr)->_raw))

#define AZ_WM_MSG_STRUT(msg_ptr) \
    ((az_wm_strut_payload_t *)((msg_ptr)->_raw))

#define AZ_WM_MSG_LAUNCH(msg_ptr) \
    ((az_wm_launch_payload_t *)((msg_ptr)->_raw))

#define AZ_WM_MSG_EVT_CREATED(msg_ptr) \
    ((az_wm_evt_created_payload_t *)((msg_ptr)->_raw))

#define AZ_WM_MSG_EVT_DESTROYED(msg_ptr) \
    ((az_wm_evt_destroyed_payload_t *)((msg_ptr)->_raw))

#define AZ_WM_MSG_EVT_FOCUS(msg_ptr) \
    ((az_wm_evt_focus_payload_t *)((msg_ptr)->_raw))

/* ── Static assertion: payloads fit in _raw[200] ─────────────────────────── */
_Static_assert(sizeof(az_wm_zorder_payload_t)       <= 200, "zorder payload overflow");
_Static_assert(sizeof(az_wm_subscribe_payload_t)    <= 200, "subscribe payload overflow");
_Static_assert(sizeof(az_wm_strut_payload_t)         <= 200, "strut payload overflow");
_Static_assert(sizeof(az_wm_launch_payload_t)        <= 200, "launch payload overflow");
_Static_assert(sizeof(az_wm_evt_created_payload_t)   <= 200, "evt_created payload overflow");
_Static_assert(sizeof(az_wm_evt_destroyed_payload_t) <= 200, "evt_destroyed payload overflow");
_Static_assert(sizeof(az_wm_evt_focus_payload_t)     <= 200, "evt_focus payload overflow");
