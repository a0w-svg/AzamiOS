#include "gui.h"
#include "gpu.h"
#include "gpu_accel.h"
#include <stdio.h>
#include <sys/syscall.h>

#include "../../lib/gfx/gfx_blit.c"
#include "../../lib/gfx/gfx_spooler.c"

static gfx_blit_ctx_t g_usr_ctx;
static bool g_usr_gfx_init = false;

void init_graphics(void) {
    /* Initialize GPU probing subsystem */
    gpu_init();
    printf("gui: probed active GPU adapter: %s\n", gpu_get_name());

    /* Initialize kernel hardware display mode */
    asm volatile("int $128" : : "a"(SYS_GFX_INIT));
    
    /* Map backbuffer into ring-3 usermode memory */
    uint32_t fb_addr = 0;
    asm volatile("int $128" : "=a"(fb_addr) : "a"(SYS_MAP_FB));
    if (fb_addr != 0 && fb_addr != (uint32_t)-1) {
        g_usr_ctx.backbuffer = (uint32_t*)(uintptr_t)fb_addr;
        g_usr_ctx.width = 640;
        g_usr_ctx.height = 480;
        g_usr_gfx_init = true;
        gpu_accel_init(g_usr_ctx.backbuffer, 640, 480);
    }
}

void gfx_flip(void) {
    asm volatile("int $128" : : "a"(SYS_GFX_FLIP));
}

void draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (!g_usr_gfx_init) {
        asm volatile("int $128" : : "a"(7), "b"(x), "c"(y), "d"(w), "S"(h), "D"(color) : "memory");
        return;
    }
    if (gpu_accel_fill_rect(x, y, w, h, color)) {
        return;
    }
    gfx_blit_rect(&g_usr_ctx, x, y, w, h, color);
}

void get_mouse_state(mouse_state_t *state) {
    asm volatile("int $128" : : "a"(8), "b"(state) : "memory");
}

void draw_text(int x, int y, const char *str, uint32_t color, uint32_t bg_color) {
    if (!g_usr_gfx_init) {
        asm volatile("int $128" : : "a"(9), "b"(x), "c"(y), "d"(str), "S"(color), "D"(bg_color) : "memory");
        return;
    }
    gfx_blit_text(&g_usr_ctx, x, y, str, color, bg_color);
}

bool has_char(void) {
    uint32_t ret;
    asm volatile("int $128" : "=a"(ret) : "a"(11) : "memory");
    return ret != 0;
}

void draw_pixel(int x, int y, uint32_t color) {
    if (!g_usr_gfx_init) {
        asm volatile("int $128" : : "a"(12), "b"(x), "c"(y), "d"(color) : "memory");
        return;
    }
    gfx_blit_put_pixel(&g_usr_ctx, x, y, color);
}

void sys_acpi_info(void) {
    asm volatile("int $128" : : "a"(13) : "memory");
}

void sys_reboot(void) {
    asm volatile("int $128" : : "a"(14) : "memory");
}

void sys_poweroff(void) {
    asm volatile("int $128" : : "a"(30) : "memory");
}

void sys_net_status(void) {
    asm volatile("int $128" : : "a"(15) : "memory");
}

void sys_net_test(void) {
    asm volatile("int $128" : : "a"(16) : "memory");
}

void sys_net_ping(void) {
    asm volatile("int $128" : : "a"(17) : "memory");
}

void sys_net_arp(void) {
    asm volatile("int $128" : : "a"(18) : "memory");
}

int sys_lsmod(char *buf, int max_len) {
    int ret;
    asm volatile("int $128" : "=a"(ret) : "a"(25), "b"(buf), "c"(max_len) : "memory");
    return ret;
}

int sys_modreload(const char *name) {
    int ret;
    asm volatile("int $128" : "=a"(ret) : "a"(29), "b"(name) : "memory");
    return ret;
}

void draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    if (!g_usr_gfx_init) {
        asm volatile("int $128" : : "a"(26), "b"(x0), "c"(y0), "d"(x1), "S"(y1), "D"(color) : "memory");
        return;
    }
    gfx_blit_line(&g_usr_ctx, x0, y0, x1, y1, color);
}

void draw_circle(int xc, int yc, int r, uint32_t color) {
    if (!g_usr_gfx_init) {
        asm volatile("int $128" : : "a"(27), "b"(xc), "c"(yc), "d"(r), "S"(color) : "memory");
        return;
    }
    gfx_blit_circle(&g_usr_ctx, xc, yc, r, color);
}

void fill_circle(int xc, int yc, int r, uint32_t color) {
    if (!g_usr_gfx_init) {
        asm volatile("int $128" : : "a"(28), "b"(xc), "c"(yc), "d"(r), "S"(color) : "memory");
        return;
    }
    gfx_blit_fill_circle(&g_usr_ctx, xc, yc, r, color);
}

void gui_scroll(int lines, uint32_t bg_color) {
    if (!g_usr_gfx_init) return;
    if (gpu_accel_scroll(lines, bg_color)) {
        return;
    }
    gfx_blit_scroll(&g_usr_ctx, lines, bg_color);
}

void sys_gfx_vsync(void) {
    asm volatile("int $128" : : "a"(42) : "memory");
}

int sys_socket(int proto) {
    int ret;
    asm volatile("int $128" : "=a"(ret) : "a"(43), "b"(proto) : "memory");
    return ret;
}

bool sys_bind(int sock, uint16_t port) {
    int ret;
    asm volatile("int $128" : "=a"(ret) : "a"(44), "b"(sock), "c"(port) : "memory");
    return ret != 0;
}

bool sys_listen(int sock) {
    int ret;
    asm volatile("int $128" : "=a"(ret) : "a"(45), "b"(sock) : "memory");
    return ret != 0;
}

bool sys_connect(int sock, uint32_t ip, uint16_t port) {
    int ret;
    asm volatile("int $128" : "=a"(ret) : "a"(46), "b"(sock), "c"(ip), "d"(port) : "memory");
    return ret != 0;
}

int sys_send(int sock, const uint8_t *buf, uint32_t len) {
    int ret;
    asm volatile("int $128" : "=a"(ret) : "a"(47), "b"(sock), "c"(buf), "d"(len) : "memory");
    return ret;
}

void sys_close(int sock) {
    asm volatile("int $128" : : "a"(48), "b"(sock) : "memory");
}

int sys_thread_create(uintptr_t entry, uintptr_t arg, uintptr_t user_stack) {
    int ret;
    asm volatile("int $128" : "=a"(ret) : "a"(49), "b"(entry), "c"(arg), "d"(user_stack) : "memory");
    return ret;
}

void sys_yield(void) {
    asm volatile("int $128" : : "a"(50) : "memory");
}

int sys_fork(void) {
    int ret;
    asm volatile("int $128" : "=a"(ret) : "a"(51) : "memory");
    return ret;
}
