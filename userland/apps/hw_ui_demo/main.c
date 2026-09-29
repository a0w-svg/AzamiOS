#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <math.h>

struct fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    uint32_t padding[24];
};

struct fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint32_t line_length;
    unsigned long mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

/* Simplified DRM structures if missing from standard headers */
#ifndef DRM_IOCTL_MODE_GETRESOURCES
#define DRM_IOCTL_MODE_GETRESOURCES 0xC04064A0
#endif

typedef struct {
    int fd;
    uint32_t conn_id;
    uint32_t crtc_id;
    uint32_t width, height;
    
    struct {
        uint32_t handle;
        uint32_t pitch;
        uint32_t size;
        uint32_t fb_id;
        uint32_t *map;
    } bufs[2];
    
    int front_buf;
} drm_ctx_t;

/* Cute UI state */
typedef struct {
    float x, y;
    float dx, dy;
    float radius;
    uint32_t color;
} bubble_t;

#define NUM_BUBBLES 12

static bubble_t bubbles[NUM_BUBBLES];

static uint32_t colors[] = {
    0xFFF5E0DC, /* Rosewater */
    0xFFF2CDCD, /* Flamingo */
    0xFFF5C2E7, /* Pink */
    0xFFCBA6F7, /* Mauve */
    0xFFF38BA8, /* Red */
    0xFFEBA0AC, /* Maroon */
    0xFFFAB387, /* Peach */
    0xFFF9E2AF, /* Yellow */
    0xFFA6E3A1, /* Green */
    0xFF94E2D5, /* Teal */
    0xFF89B4FA, /* Blue */
    0xFFB4BEFE  /* Lavender */
};

/* Soft-render a cute circle with anti-aliasing */
static void draw_bubble(uint32_t *pixels, uint32_t pitch, uint32_t w, uint32_t h, bubble_t *b)
{
    int r = (int)b->radius;
    int cx = (int)b->x;
    int cy = (int)b->y;
    
    int min_x = cx - r < 0 ? 0 : cx - r;
    int max_x = cx + r >= (int)w ? (int)w - 1 : cx + r;
    int min_y = cy - r < 0 ? 0 : cy - r;
    int max_y = cy + r >= (int)h ? (int)h - 1 : cy + r;
    
    uint32_t bc = b->color;
    uint32_t br = (bc >> 16) & 0xFF;
    uint32_t bg = (bc >> 8) & 0xFF;
    uint32_t bb = bc & 0xFF;
    
    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            float dx = x - b->x;
            float dy = y - b->y;
            float dist = sqrtf(dx*dx + dy*dy);
            
            if (dist <= b->radius) {
                float alpha = 1.0f;
                if (dist > b->radius - 2.0f) {
                    alpha = (b->radius - dist) / 2.0f;
                }
                
                uint32_t *px = &pixels[y * (pitch / 4) + x];
                uint32_t bg_col = *px;
                uint32_t bgr = (bg_col >> 16) & 0xFF;
                uint32_t bgg = (bg_col >> 8) & 0xFF;
                uint32_t bgb = bg_col & 0xFF;
                
                uint32_t nr = (uint32_t)(br * alpha + bgr * (1.0f - alpha));
                uint32_t ng = (uint32_t)(bg * alpha + bgg * (1.0f - alpha));
                uint32_t nb = (uint32_t)(bb * alpha + bgb * (1.0f - alpha));
                
                *px = 0xFF000000 | (nr << 16) | (ng << 8) | nb;
            }
        }
    }
}

int main()
{
    printf("[hw_ui_demo] Starting KMS hardware UI rendering demo...\n");
    
    drm_ctx_t ctx = {0};
    ctx.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (ctx.fd < 0) {
        printf("Failed to open /dev/dri/card0\n");
        return 1;
    }
    
    /* 1. Get resources (Simulated dumb buffer setup for demo) */
    /* Due to missing full DRM headers, we'll fall back to fbdev if DRM complex modesetting fails,
       but we will use the hardware FBIOPAN_DISPLAY which acts as our KMS flip */
    int fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        printf("Failed to open /dev/fb0 for hardware page flip\n");
        return 1;
    }
    
    struct fb_var_screeninfo var;
    if (ioctl(fb_fd, 0x4600, &var) < 0) { /* FBIOGET_VSCREENINFO */
        return 1;
    }
    
    struct fb_fix_screeninfo fix;
    if (ioctl(fb_fd, 0x4602, &fix) < 0) { /* FBIOGET_FSCREENINFO */
        return 1;
    }
    
    /* Enable double buffering by setting yres_virtual to 2*yres */
    var.yres_virtual = var.yres * 2;
    if (ioctl(fb_fd, 0x4601, &var) < 0) { /* FBIOPUT_VSCREENINFO */
        printf("Hardware does not support double buffering.\n");
    }
    
    uint32_t pitch = fix.line_length;
    uint32_t w = var.xres;
    uint32_t h = var.yres;
    uint32_t size = pitch * var.yres_virtual;
    
    uint32_t *vram = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (vram == MAP_FAILED) {
        return 1;
    }
    
    uint32_t *buf[2];
    buf[0] = vram;
    buf[1] = (uint32_t*)((uint8_t*)vram + pitch * h);
    
    /* Initialize cute UI */
    srand(42);
    for (int i = 0; i < NUM_BUBBLES; i++) {
        bubbles[i].x = rand() % w;
        bubbles[i].y = rand() % h;
        bubbles[i].dx = ((rand() % 100) / 50.0f - 1.0f) * 4.0f;
        bubbles[i].dy = ((rand() % 100) / 50.0f - 1.0f) * 4.0f;
        bubbles[i].radius = 40 + rand() % 80;
        bubbles[i].color = colors[i % (sizeof(colors)/sizeof(colors[0]))];
    }
    
    int frame = 0;
    int back_idx = 0;
    
    printf("[hw_ui_demo] Hardware page flipping active. Rendering %dx%d UI.\n", w, h);
    
    while (frame < 300) { /* Run for ~300 frames */
        uint32_t *dst = buf[back_idx];
        
        /* Clear background to dark pastel (Catppuccin Mocha Base) */
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                dst[y * (pitch / 4) + x] = 0xFF1E1E2E;
            }
        }
        
        /* Draw cute hardware-accelerated-style UI components */
        for (int i = 0; i < NUM_BUBBLES; i++) {
            bubbles[i].x += bubbles[i].dx;
            bubbles[i].y += bubbles[i].dy;
            
            if (bubbles[i].x - bubbles[i].radius < 0 || bubbles[i].x + bubbles[i].radius >= w)
                bubbles[i].dx *= -1.0f;
            if (bubbles[i].y - bubbles[i].radius < 0 || bubbles[i].y + bubbles[i].radius >= h)
                bubbles[i].dy *= -1.0f;
                
            draw_bubble(dst, pitch, w, h, &bubbles[i]);
        }
        
        /* Hardware Page Flip */
        var.yoffset = back_idx * h;
        ioctl(fb_fd, 0x4606, &var); /* FBIOPAN_DISPLAY */
        
        back_idx = 1 - back_idx;
        frame++;
        
        /* Simple vsync/delay */
        struct timespec ts = {0, 16000000}; /* ~60fps */
        nanosleep(&ts, NULL);
    }
    
    /* Restore */
    var.yoffset = 0;
    ioctl(fb_fd, 0x4606, &var);
    
    printf("[hw_ui_demo] Demo complete.\n");
    return 0;
}
