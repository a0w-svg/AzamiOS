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
