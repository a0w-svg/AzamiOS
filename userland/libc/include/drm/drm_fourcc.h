/* ============================================================================
 * AzamiOS C Library — Linux DRM/KMS FourCC Pixel Format Definitions
 * File: userland/libc/include/drm/drm_fourcc.h
 * ============================================================================ */
#ifndef _DRM_FOURCC_H
#define _DRM_FOURCC_H

#include <stdint.h>

#define fourcc_code(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
                                 ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define DRM_FORMAT_INVALID      0

/* 8 bpp */
#define DRM_FORMAT_C8           fourcc_code('C', '8', ' ', ' ')
#define DRM_FORMAT_R8           fourcc_code('R', '8', ' ', ' ')

/* 16 bpp */
#define DRM_FORMAT_RGB565       fourcc_code('R', 'G', '1', '6')
#define DRM_FORMAT_BGR565       fourcc_code('B', 'G', '1', '6')
#define DRM_FORMAT_ARGB1555     fourcc_code('A', 'R', '1', '5')
#define DRM_FORMAT_XRGB1555     fourcc_code('X', 'R', '1', '5')

/* 24 bpp */
#define DRM_FORMAT_RGB888       fourcc_code('R', 'G', '2', '4')
#define DRM_FORMAT_BGR888       fourcc_code('B', 'G', '2', '4')

/* 32 bpp */
#define DRM_FORMAT_XRGB8888     fourcc_code('X', 'R', '2', '4')
#define DRM_FORMAT_XBGR8888     fourcc_code('X', 'B', '2', '4')
#define DRM_FORMAT_RGBX8888     fourcc_code('R', 'X', '2', '4')
#define DRM_FORMAT_BGRX8888     fourcc_code('B', 'X', '2', '4')

#define DRM_FORMAT_ARGB8888     fourcc_code('A', 'R', '2', '4')
#define DRM_FORMAT_ABGR8888     fourcc_code('A', 'B', '2', '4')
#define DRM_FORMAT_RGBA8888     fourcc_code('R', 'A', '2', '4')
#define DRM_FORMAT_BGRA8888     fourcc_code('B', 'A', '2', '4')

#endif /* _DRM_FOURCC_H */
