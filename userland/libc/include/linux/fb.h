/* ============================================================================
 * AzamiOS C Library — Linux-Compatible Framebuffer UAPI Header
 * File: userland/libc/include/linux/fb.h
 * ============================================================================ */
#ifndef _LINUX_FB_H
#define _LINUX_FB_H

#include <stdint.h>
#include <sys/ioctl.h>

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOGETCMAP         0x4604
#define FBIOPUTCMAP         0x4605
#define FBIOPAN_DISPLAY     0x4606
#define FBIO_CURSOR         0x4608
#define FBIOBLANK           0x4611
#define FBIO_WAITFORVSYNC   0x4620

#define FB_TYPE_PACKED_PIXELS      0 /* Packed Pixels */
#define FB_TYPE_PLANES             1 /* Non interleaved planes */
#define FB_TYPE_INTERLEAVED_PLANES 2 /* Interleaved planes */
#define FB_TYPE_TEXT               3 /* Text/CGA classes */
#define FB_TYPE_VGA_PLANES         4 /* EGA/VGA planes */

#define FB_VISUAL_MONO01           0 /* Monochr. 1=Black 0=White */
#define FB_VISUAL_MONO10           1 /* Monochr. 1=White 0=Black */
#define FB_VISUAL_TRUECOLOR        2 /* True color */
#define FB_VISUAL_PSEUDOCOLOR      3 /* Pseudo color (like atari) */
#define FB_VISUAL_DIRECTCOLOR      4 /* Direct color */
#define FB_VISUAL_STATIC_PSEUDOCOLOR 5 /* Pseudo color readonly */

#define FB_BLANK_UNBLANK           0
#define FB_BLANK_NORMAL            1
#define FB_BLANK_VSYNC_SUSPEND     2
#define FB_BLANK_HSYNC_SUSPEND     3
#define FB_BLANK_POWERDOWN         4

#define FB_ACTIVATE_NOW            0 /* set values immediately (or vbl)*/
#define FB_ACTIVATE_NXTOPEN        1 /* activate on next open */
#define FB_ACTIVATE_TEST           2 /* don't set, just test info */
#define FB_ACTIVATE_MASK           15
#define FB_ACTIVATE_VBL            16 /* activate values on next vbl  */
#define FB_CHANGE_CMAP_VBL         32 /* change colormap on vbl */
#define FB_ACTIVATE_ALL            64 /* change all drivers on same card */
#define FB_ACTIVATE_FORCE          128 /* force apply even if no change */
#define FB_ACTIVATE_INV_MODE       256 /* invalidate current mode */

struct fb_bitfield {
    uint32_t offset;    /* beginning of bitfield */
    uint32_t length;    /* length of bitfield */
    uint32_t msb_right; /* != 0 : Most significant bit is right */ 
};

struct fb_var_screeninfo {
    uint32_t xres;           /* visible resolution */
    uint32_t yres;
    uint32_t xres_virtual;   /* virtual resolution */
    uint32_t yres_virtual;
    uint32_t xoffset;        /* offset from virtual to visible */
    uint32_t yoffset;
    uint32_t bits_per_pixel; /* guess what */
    uint32_t grayscale;      /* 0 = color, 1 = grayscale, >1 = FOURCC */
    struct fb_bitfield red;
    struct fb_bitfield green;
    struct fb_bitfield blue;
    struct fb_bitfield transp;
    uint32_t nonstd;         /* != 0 Non standard pixel format */
    uint32_t activate;       /* see FB_ACTIVATE_* */
    uint32_t height;         /* height of picture in mm */
    uint32_t width;          /* width of picture in mm */
    uint32_t accel_flags;    /* (OBSOLETE) see fb_info.flags */
    uint32_t pixclock;       /* pixel clock in ps (pico seconds) */
    uint32_t left_margin;    /* time from sync to picture */
    uint32_t right_margin;   /* time from picture to sync */
    uint32_t upper_margin;   /* time from sync to picture */
    uint32_t lower_margin;
    uint32_t hsync_len;      /* length of horizontal sync */
    uint32_t vsync_len;      /* length of vertical sync */
    uint32_t sync;           /* see FB_SYNC_* */
    uint32_t vmode;          /* see FB_VMODE_* */
    uint32_t rotate;         /* angle we rotate counter clockwise */
    uint32_t colorspace;     /* colorspace for FOURCC-based modes */
    uint32_t reserved[4];    /* Reserved for future compatibility */
};

struct fb_fix_screeninfo {
    char id[16];             /* identification string eg "AzamiFB" */
    unsigned long smem_start;/* Start of frame buffer mem (physical address) */
    uint32_t smem_len;       /* Length of frame buffer mem */
    uint32_t type;           /* see FB_TYPE_* */
    uint32_t type_aux;       /* Interleave for interleaved Planes */
    uint32_t visual;         /* see FB_VISUAL_* */ 
    uint16_t xpanstep;       /* zero if no hardware panning */
    uint16_t ypanstep;       /* zero if no hardware panning */
    uint16_t ywrapstep;      /* zero if no hardware ywrap */
    uint32_t line_length;    /* length of a line in bytes */
    unsigned long mmio_start;/* Start of Memory Mapped I/O (physical address) */
    uint32_t mmio_len;       /* Length of Memory Mapped I/O */
    uint32_t accel;          /* Indicate to driver which specific chip/card we have */
    uint16_t capabilities;   /* see FB_CAP_* */
    uint16_t reserved[2];    /* Reserved for future compatibility */
};

struct fb_cmap {
    uint32_t start;          /* First entry */
    uint32_t len;            /* Number of entries */
    uint16_t *red;           /* Red values */
    uint16_t *green;
    uint16_t *blue;
    uint16_t *transp;        /* transparency, can be NULL */
};

#endif /* _LINUX_FB_H */
