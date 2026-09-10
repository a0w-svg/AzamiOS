/* ============================================================================
 * AzamiOS — DRM/KMS User API (Linux ABI compatible)
 * File: include/azami/drm.h
 *
 * Request codes and structure layouts match Linux's <drm/drm.h> and
 * <drm/drm_mode.h> so that Mesa-style clients, libdrm and simple KMS
 * programs speak to /dev/dri/cardN unmodified.
 * ============================================================================ */
#pragma once

#include "types.h"

/* ── Core requests ───────────────────────────────────────────────────────── */
#define DRM_IOCTL_VERSION                0xC0406400
#define DRM_IOCTL_GET_UNIQUE             0xC0106401
#define DRM_IOCTL_GET_MAGIC              0x80046402
#define DRM_IOCTL_GET_CAP                0xC010640C
#define DRM_IOCTL_SET_CLIENT_CAP         0x4010640D
#define DRM_IOCTL_GEM_CLOSE              0x40086409
#define DRM_IOCTL_GEM_FLINK              0xC008640A
#define DRM_IOCTL_GEM_OPEN               0xC010640B
#define DRM_IOCTL_AUTH_MAGIC             0x40046411
#define DRM_IOCTL_SET_MASTER             0x0000641E
#define DRM_IOCTL_DROP_MASTER            0x0000641F
#define DRM_IOCTL_PRIME_HANDLE_TO_FD     0xC00C642D
#define DRM_IOCTL_PRIME_FD_TO_HANDLE     0xC00C642E
#define DRM_IOCTL_WAIT_VBLANK            0xC018643A

/* ── Mode setting requests ───────────────────────────────────────────────── */
#define DRM_IOCTL_MODE_GETRESOURCES      0xC04064A0
#define DRM_IOCTL_MODE_GETCRTC           0xC06864A1
#define DRM_IOCTL_MODE_SETCRTC           0xC06864A2
#define DRM_IOCTL_MODE_CURSOR            0xC01C64A3
#define DRM_IOCTL_MODE_GETENCODER        0xC01464A6
#define DRM_IOCTL_MODE_GETCONNECTOR      0xC05064A7
#define DRM_IOCTL_MODE_GETFB             0xC01C64AD
#define DRM_IOCTL_MODE_ADDFB             0xC01C64AE
#define DRM_IOCTL_MODE_RMFB              0xC00464AF
#define DRM_IOCTL_MODE_PAGE_FLIP         0xC01864B0
#define DRM_IOCTL_MODE_DIRTYFB           0xC01864B1
#define DRM_IOCTL_MODE_CREATE_DUMB       0xC02064B2
#define DRM_IOCTL_MODE_MAP_DUMB          0xC01064B3
#define DRM_IOCTL_MODE_DESTROY_DUMB      0xC00464B4
#define DRM_IOCTL_MODE_GETPLANERESOURCES 0xC01064B5
#define DRM_IOCTL_MODE_GETPLANE          0xC02064B6
#define DRM_IOCTL_MODE_SETPLANE          0xC03064B7
#define DRM_IOCTL_MODE_ADDFB2            0xC06064B8
#define DRM_IOCTL_MODE_CURSOR2           0xC02464BB

/* ── Capabilities ────────────────────────────────────────────────────────── */
#define DRM_CAP_DUMB_BUFFER              0x1
#define DRM_CAP_VBLANK_HIGH_CRTC         0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH     0x3
#define DRM_CAP_DUMB_PREFER_SHADOW       0x4
#define DRM_CAP_PRIME                    0x5
#define DRM_CAP_TIMESTAMP_MONOTONIC      0x6
#define DRM_CAP_ASYNC_PAGE_FLIP          0x7
#define DRM_CAP_CURSOR_WIDTH             0x8
#define DRM_CAP_CURSOR_HEIGHT            0x9
#define DRM_CAP_ADDFB2_MODIFIERS         0x10
#define DRM_CAP_CRTC_IN_VBLANK_EVENT     0x0E

#define DRM_CLIENT_CAP_STEREO_3D         1
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES  2
#define DRM_CLIENT_CAP_ATOMIC            3

#define DRM_PRIME_CAP_IMPORT             0x1
#define DRM_PRIME_CAP_EXPORT             0x2

/* ── Connector / encoder enumerations ────────────────────────────────────── */
#define DRM_MODE_CONNECTOR_Unknown       0
#define DRM_MODE_CONNECTOR_VGA           1
#define DRM_MODE_CONNECTOR_DVII          2
#define DRM_MODE_CONNECTOR_HDMIA         11
#define DRM_MODE_CONNECTOR_VIRTUAL       15

#define DRM_MODE_ENCODER_NONE            0
#define DRM_MODE_ENCODER_DAC             1
#define DRM_MODE_ENCODER_TMDS            2
#define DRM_MODE_ENCODER_VIRTUAL         5

#define DRM_MODE_CONNECTED               1
#define DRM_MODE_DISCONNECTED            2
#define DRM_MODE_UNKNOWNCONNECTION       3

#define DRM_MODE_SUBPIXEL_UNKNOWN        1
#define DRM_MODE_SUBPIXEL_HORIZONTAL_RGB 2

/* ── Plane types (universal planes) ──────────────────────────────────────── */
#define DRM_PLANE_TYPE_OVERLAY           0
#define DRM_PLANE_TYPE_PRIMARY           1
#define DRM_PLANE_TYPE_CURSOR            2

/* ── Mode flags/types ────────────────────────────────────────────────────── */
#define DRM_MODE_TYPE_PREFERRED          (1 << 3)
#define DRM_MODE_TYPE_DRIVER             (1 << 6)
#define DRM_MODE_FLAG_PHSYNC             (1 << 0)
#define DRM_MODE_FLAG_PVSYNC             (1 << 2)

/* ── Cursor / page-flip flags ────────────────────────────────────────────── */
#define DRM_MODE_CURSOR_BO               0x01
#define DRM_MODE_CURSOR_MOVE             0x02
#define DRM_MODE_PAGE_FLIP_EVENT         0x01
#define DRM_MODE_PAGE_FLIP_ASYNC         0x02

/* ── FourCC pixel formats ────────────────────────────────────────────────── */
#define DRM_FORMAT_XRGB8888              0x34325258  /* 'XR24' */
#define DRM_FORMAT_ARGB8888              0x34325241  /* 'AR24' */
#define DRM_FORMAT_RGB565                0x36314752  /* 'RG16' */

/* ── WAIT_VBLANK request types ───────────────────────────────────────────── */
#define _DRM_VBLANK_ABSOLUTE             0x0
#define _DRM_VBLANK_RELATIVE             0x1
#define _DRM_VBLANK_HIGH_CRTC_MASK       0x0000003E
#define _DRM_VBLANK_HIGH_CRTC_SHIFT      1
#define _DRM_VBLANK_EVENT                0x4000000
#define _DRM_VBLANK_FLIP                 0x8000000
#define _DRM_VBLANK_NEXTONMISS           0x10000000
#define _DRM_VBLANK_SECONDARY            0x20000000
#define _DRM_VBLANK_SIGNAL               0x40000000
#define _DRM_VBLANK_TYPES_MASK           (_DRM_VBLANK_ABSOLUTE | _DRM_VBLANK_RELATIVE)
#define _DRM_VBLANK_FLAGS_MASK           (_DRM_VBLANK_EVENT | _DRM_VBLANK_SIGNAL | \
                                          _DRM_VBLANK_SECONDARY | _DRM_VBLANK_NEXTONMISS)

/* ── DIRTYFB annotations ─────────────────────────────────────────────────── */
#define DRM_MODE_DIRTY_OFF               0
#define DRM_MODE_DIRTY_ON                1
#define DRM_MODE_DIRTY_ANNOTATE          2

/* A damage rectangle as DIRTYFB delivers it; x2/y2 are exclusive. */
struct drm_clip_rect {
    u16 x1, y1, x2, y2;
};

/* ── Event types delivered on the DRM fd ─────────────────────────────────── */
#define DRM_EVENT_VBLANK                 0x01
#define DRM_EVENT_FLIP_COMPLETE          0x02

struct drm_event {
    u32 type;
    u32 length;
};

struct drm_event_vblank {
    struct drm_event base;
    u64 user_data;
    u32 tv_sec;
    u32 tv_usec;
    u32 sequence;
    u32 crtc_id;
};

/* ── Core structures ─────────────────────────────────────────────────────── */
struct drm_version {
    int    version_major;
    int    version_minor;
    int    version_patchlevel;
    size_t name_len;
    char  *name;
    size_t date_len;
    char  *date;
    size_t desc_len;
    char  *desc;
};

struct drm_unique {
    size_t unique_len;
    char  *unique;
};

struct drm_auth {
    u32 magic;
};

struct drm_get_cap {
    u64 capability;
    u64 value;
};

struct drm_set_client_cap {
    u64 capability;
    u64 value;
};

struct drm_gem_close {
    u32 handle;
    u32 pad;
};

struct drm_gem_flink {
    u32 handle;
    u32 name;
};

struct drm_gem_open {
    u32 name;
    u32 handle;
    u64 size;
};

struct drm_prime_handle {
    u32 handle;
    u32 flags;
    s32 fd;
};

struct drm_wait_vblank_request {
    u32 type;
    u32 sequence;
    u64 signal;
};

struct drm_wait_vblank_reply {
    u32 type;
    u32 sequence;
    s64 tval_sec;
    s64 tval_usec;
};

union drm_wait_vblank {
    struct drm_wait_vblank_request request;
    struct drm_wait_vblank_reply   reply;
};

/* ── Mode setting structures ─────────────────────────────────────────────── */
struct drm_mode_modeinfo {
    u32  clock;
    u16  hdisplay, hsync_start, hsync_end, htotal, hskew;
    u16  vdisplay, vsync_start, vsync_end, vtotal, vscan;
    u32  vrefresh;
    u32  flags;
    u32  type;
    char name[32];
};

struct drm_mode_card_res {
    u64 fb_id_ptr;
    u64 crtc_id_ptr;
    u64 connector_id_ptr;
    u64 encoder_id_ptr;
    u32 count_fbs;
    u32 count_crtcs;
    u32 count_connectors;
    u32 count_encoders;
    u32 min_width, max_width;
    u32 min_height, max_height;
};

struct drm_mode_crtc {
    u64 set_connectors_ptr;
    u32 count_connectors;
    u32 crtc_id;
    u32 fb_id;
    u32 x, y;
    u32 gamma_size;
    u32 mode_valid;
    struct drm_mode_modeinfo mode;
};

struct drm_mode_get_connector {
    u64 encoders_ptr;
    u64 modes_ptr;
    u64 props_ptr;
    u64 prop_values_ptr;
    u32 count_modes;
    u32 count_props;
    u32 count_encoders;
    u32 encoder_id;
    u32 connector_id;
    u32 connector_type;
    u32 connector_type_id;
    u32 connection;
    u32 mm_width, mm_height;
    u32 subpixel;
    u32 pad;
};

struct drm_mode_get_encoder {
    u32 encoder_id;
    u32 encoder_type;
    u32 crtc_id;
    u32 possible_crtcs;
    u32 possible_clones;
};

struct drm_mode_create_dumb {
    u32 height, width, bpp, flags;
    u32 handle, pitch;
    u64 size;
};

struct drm_mode_map_dumb {
    u32 handle;
    u32 pad;
    u64 offset;
};

struct drm_mode_destroy_dumb {
    u32 handle;
};

struct drm_mode_fb_cmd {
    u32 fb_id;
    u32 width, height;
    u32 pitch, bpp, depth;
    u32 handle;
};

struct drm_mode_fb_cmd2 {
    u32 fb_id;
    u32 width, height;
    u32 pixel_format;
    u32 flags;
    u32 handles[4];
    u32 pitches[4];
    u32 offsets[4];
    u64 modifier[4];
};

struct drm_mode_fb_dirty_cmd {
    u32 fb_id;
    u32 flags;
    u32 color;
    u32 num_clips;
    u64 clips_ptr;
};

struct drm_mode_crtc_page_flip {
    u32 crtc_id;
    u32 fb_id;
    u32 flags;
    u32 reserved;
    u64 user_data;
};

struct drm_mode_cursor {
    u32 flags;
    u32 crtc_id;
    s32 x, y;
    u32 width, height;
    u32 handle;
};

struct drm_mode_cursor2 {
    u32 flags;
    u32 crtc_id;
    s32 x, y;
    u32 width, height;
    u32 handle;
    s32 hot_x, hot_y;
};

struct drm_mode_get_plane_res {
    u64 plane_id_ptr;
    u32 count_planes;
};

struct drm_mode_get_plane {
    u64 format_type_ptr;
    u32 plane_id;
    u32 crtc_id;
    u32 fb_id;
    u32 possible_crtcs;
    u32 gamma_size;
    u32 count_format_types;
};

struct drm_mode_set_plane {
    u32 plane_id;
    u32 crtc_id;
    u32 fb_id;
    u32 flags;
    s32 crtc_x, crtc_y;
    u32 crtc_w, crtc_h;
    u32 src_x, src_y;
    u32 src_h, src_w;
};
