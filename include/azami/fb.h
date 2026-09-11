/* ============================================================================
 * AzamiOS — Framebuffer ioctl definitions (Linux fbdev compatible)
 * File: include/azami/fb.h
 * ============================================================================ */
#pragma once

#include <azami/types.h>

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOGETCMAP         0x4604
#define FBIOPUTCMAP         0x4605
#define FBIOPAN_DISPLAY     0x4606
#define FBIOBLANK           0x4611
#define FBIOGET_CON2FBMAP   0x460F
#define FBIOPUT_CON2FBMAP   0x4610
#define FBIO_WAITFORVSYNC   0x4620

/* ── AzamiOS hardware-cursor plane (VirtIO-GPU) ─────────────────────────────
 * A backend with a real cursor overlay lets a client hand the pointer sprite
 * to the display once and then just move it: no framebuffer redraw, no
 * transfer to the host per pointer step. FBIOAZ_HWCURSOR_SET returns 0 when
 * the backend took the cursor, -ENOTTY when it has none (the client then
 * composites the pointer itself, as before). */
#define FBIOAZ_HWCURSOR_SET   0x4680   /* arg: struct fb_az_hwcursor *      */
#define FBIOAZ_HWCURSOR_MOVE  0x4681   /* arg: struct fb_az_hwcursor_pos *  */
#define FBIOAZ_HWCURSOR_HIDE  0x4682   /* arg: ignored                      */

/* FBIOAZ_DAMAGE — report the rectangle a client just drew. On a backend that
 * pushes frames to a host (VirtIO-GPU) the present worker then transfers only
 * those rows instead of the whole scanout, which is the difference between a
 * 4 MiB copy per frame and a few KiB. Rectangles union until the next flush.
 * A no-op (harmless) on a direct-scanout backend. */
#define FBIOAZ_DAMAGE         0x4683   /* arg: struct fb_az_rect *          */

/* ── Hardware 2D Acceleration & Capabilities ──────────────────────────────── */
#define FBIOAZ_ACCEL_FILL     0x4684   /* arg: struct fb_az_fill *          */
#define FBIOAZ_ACCEL_COPY     0x4685   /* arg: struct fb_az_copy *          */
#define FBIOAZ_GET_CAPS       0x4686   /* arg: struct fb_az_caps *          */

#define FB_AZ_CAP_DOUBLEBUF   (1U << 0)
#define FB_AZ_CAP_HW_FLIP     (1U << 1)
#define FB_AZ_CAP_HW_CURSOR   (1U << 2)
#define FB_AZ_CAP_ACCEL_2D    (1U << 3)
#define FB_AZ_CAP_VIRTIO      (1U << 4)

struct fb_az_fill {
    u32 x;
    u32 y;
    u32 w;
    u32 h;
    u32 color;      /* 32-bit ARGB8888 pixel value */
    u32 buffer_idx; /* 0 for primary/front, 1 for backbuffer */
};

struct fb_az_copy {
    u32 src_x;
    u32 src_y;
    u32 dst_x;
    u32 dst_y;
    u32 w;
    u32 h;
    u32 src_buf;
    u32 dst_buf;
};

struct fb_az_caps {
    u32 caps;
    u32 buffers;
    u32 max_width;
    u32 max_height;
    u32 pitch;
};

struct fb_az_rect {
    u32 x;
    u32 y;
    u32 w;
    u32 h;
};

#define FB_AZ_HWCURSOR_MAX 64          /* image is at most 64x64, BGRA8888  */

struct fb_az_hwcursor {
    u32 width;
    u32 height;
    u32 hot_x;
    u32 hot_y;
    u64 image;      /* userspace pointer to width*height BGRA8888 pixels */
};

struct fb_az_hwcursor_pos {
    s32 x;
    s32 y;
};

/* fb_var_screeninfo::activate — when a change takes effect. */
#define FB_ACTIVATE_NOW     0    /* apply immediately (or at the next vblank) */
#define FB_ACTIVATE_NXTOPEN 1    /* apply on the next open                    */
#define FB_ACTIVATE_TEST    2    /* validate only, change nothing             */
#define FB_ACTIVATE_MASK    15
#define FB_ACTIVATE_VBL     16   /* apply at the next vblank                  */

#define FB_TYPE_PACKED_PIXELS      0
#define FB_VISUAL_TRUECOLOR        2

#define FB_BLANK_UNBLANK           0
#define FB_BLANK_NORMAL            1
#define FB_BLANK_POWERDOWN         4

struct fb_cmap {
    u32 start;          /* First entry */
    u32 len;            /* Number of entries */
    u16 *red;           /* Red values */
    u16 *green;
    u16 *blue;
    u16 *transp;        /* transparency, can be NULL */
};

struct fb_con2fbmap {
    u32 console;
    u32 framebuffer;
};


struct fb_bitfield {
    u32 offset;         /* beginning of bitfield */
    u32 length;         /* length of bitfield */
    u32 msb_right;      /* != 0 : Most significant bit is right */ 
};

struct fb_var_screeninfo {
    u32 xres;           /* visible resolution */
    u32 yres;
    u32 xres_virtual;   /* virtual resolution */
    u32 yres_virtual;
    u32 xoffset;        /* offset from virtual to visible */
    u32 yoffset;
    u32 bits_per_pixel; /* guess what */
    u32 grayscale;      /* 0 = color, 1 = grayscale, >1 = FOURCC */
    struct fb_bitfield red;
    struct fb_bitfield green;
    struct fb_bitfield blue;
    struct fb_bitfield transp;
    u32 nonstd;
    u32 activate;
    u32 height;
    u32 width;
    u32 accel_flags;
    u32 pixclock;
    u32 left_margin;
    u32 right_margin;
    u32 upper_margin;
    u32 lower_margin;
    u32 hsync_len;
    u32 vsync_len;
    u32 sync;
    u32 vmode;
    u32 rotate;
    u32 colorspace;
    u32 reserved[4];
};

struct fb_fix_screeninfo {
    char id[16];        /* identification string eg "AzamiFB" */
    unsigned long smem_start; /* Start of frame buffer mem (physical address) */
    u32 smem_len;       /* Length of frame buffer mem */
    u32 type;           /* see FB_TYPE_* */
    u32 type_aux;       /* Interleave for interleaved Planes */
    u32 visual;         /* see FB_VISUAL_* */ 
    u16 xpanstep;       /* zero if no hardware panning */
    u16 ypanstep;       /* zero if no hardware panning */
    u16 ywrapstep;      /* zero if no hardware ywrap */
    u32 line_length;    /* length of a line in bytes */
    unsigned long mmio_start; /* Start of Memory Mapped I/O (physical address) */
    u32 mmio_len;       /* Length of Memory Mapped I/O */
    u32 accel;          /* Indicate to driver which specific chip/card we have */
    u16 capabilities;
    u16 reserved[2];
};
