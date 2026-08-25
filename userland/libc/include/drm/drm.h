/* ============================================================================
 * AzamiOS C Library — Linux Direct Rendering Manager (DRM) UAPI Header
 * File: userland/libc/include/drm/drm.h
 * ============================================================================ */
#ifndef _DRM_H
#define _DRM_H

#include <stdint.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include "drm_mode.h"
#include "drm_fourcc.h"

#define DRM_NAME           "drm"

#define DRM_IOCTL_BASE                  'd'
#define DRM_IO(nr)                      _IO(DRM_IOCTL_BASE,nr)
#define DRM_IOR(nr,type)                _IOR(DRM_IOCTL_BASE,nr,type)
#define DRM_IOW(nr,type)                _IOW(DRM_IOCTL_BASE,nr,type)
#define DRM_IOWR(nr,type)               _IOWR(DRM_IOCTL_BASE,nr,type)

#define DRM_IOCTL_VERSION               DRM_IOWR(0x00, struct drm_version)
#define DRM_IOCTL_GET_UNIQUE            DRM_IOWR(0x01, struct drm_unique)
#define DRM_IOCTL_GET_MAGIC             DRM_IOR( 0x02, struct drm_auth)
#define DRM_IOCTL_AUTH_MAGIC            DRM_IOW( 0x11, struct drm_auth)
#define DRM_IOCTL_SET_MASTER            DRM_IO(  0x1e)
#define DRM_IOCTL_DROP_MASTER           DRM_IO(  0x1f)
#define DRM_IOCTL_GET_CAP               DRM_IOWR(0x0c, struct drm_get_cap)
#define DRM_IOCTL_SET_CLIENT_CAP        DRM_IOW( 0x0d, struct drm_set_client_cap)

#define DRM_IOCTL_PRIME_HANDLE_TO_FD    DRM_IOWR(0x2d, struct drm_prime_handle)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE    DRM_IOWR(0x2e, struct drm_prime_handle)

#define DRM_IOCTL_MODE_GETRESOURCES     DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC          DRM_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC          DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_CURSOR           DRM_IOWR(0xA3, struct drm_mode_cursor)
#define DRM_IOCTL_MODE_GETGAMMA         DRM_IOWR(0xA4, struct drm_mode_crtc_lut)
#define DRM_IOCTL_MODE_SETGAMMA         DRM_IOWR(0xA5, struct drm_mode_crtc_lut)
#define DRM_IOCTL_MODE_GETENCODER       DRM_IOWR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR     DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_ADDFB            DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_RMFB             DRM_IOWR(0xAF, unsigned int)
#define DRM_IOCTL_MODE_PAGE_FLIP        DRM_IOWR(0xB0, struct drm_mode_crtc_page_flip)
#define DRM_IOCTL_MODE_CREATE_DUMB      DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB         DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB     DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)
#define DRM_IOCTL_MODE_GETPLANERESOURCES DRM_IOWR(0xB5, struct drm_mode_get_plane_res)
#define DRM_IOCTL_MODE_GETPLANE         DRM_IOWR(0xB6, struct drm_mode_get_plane)
#define DRM_IOCTL_MODE_SETPLANE         DRM_IOWR(0xB7, struct drm_mode_set_plane)
#define DRM_IOCTL_MODE_ADDFB2           DRM_IOWR(0xB8, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_MODE_CURSOR2          DRM_IOWR(0xBB, struct drm_mode_cursor2)

#define DRM_CAP_DUMB_BUFFER             0x1
#define DRM_CAP_VBLANK_HIGH_CRTC        0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH    0x3
#define DRM_CAP_DUMB_PREFER_SHADOW      0x4
#define DRM_CAP_PRIME                   0x5
#define DRM_CAP_TIMESTAMP_MONOTONIC     0x6
#define DRM_CAP_ASYNC_PAGE_FLIP         0x7
#define DRM_CAP_CURSOR_WIDTH            0x8
#define DRM_CAP_CURSOR_HEIGHT           0x9
#define DRM_CAP_ADDFB2_MODIFIERS        0x10

#define DRM_CLIENT_CAP_STEREO_3D        1
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#define DRM_CLIENT_CAP_ATOMIC           3
#define DRM_CLIENT_CAP_ASPECT_RATIO     4

#define DRM_PRIME_READ_WRITE            0x00000002

struct drm_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
    size_t name_len;
    char *name;
    size_t date_len;
    char *date;
    size_t desc_len;
    char *desc;
};

struct drm_unique {
    size_t unique_len;
    char *unique;
};

struct drm_auth {
    unsigned int magic;
};

struct drm_get_cap {
    uint64_t capability;
    uint64_t value;
};

struct drm_set_client_cap {
    uint64_t capability;
    uint64_t value;
};

struct drm_prime_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t  fd;
};

#endif /* _DRM_H */
