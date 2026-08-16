/* ============================================================================
 * AzamiOS — Framebuffer ioctl definitions
 * File: include/azami/fb.h
 * ============================================================================ */
#pragma once

#include <azami/types.h>

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

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
};

struct fb_fix_screeninfo {
    char id[16];        /* identification string eg "TT Builtin" */
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
};
