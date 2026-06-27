#include "include/gui.h"

void init_graphics(void) {
    asm volatile("int $128" : : "a"(5));
}

void gfx_flip(void) {
    asm volatile("int $128" : : "a"(6));
}

void draw_rect(int x, int y, int w, int h, uint32_t color) {
    asm volatile("int $128" : : "a"(7), "b"(x), "c"(y), "d"(w), "S"(h), "D"(color) : "memory");
}

void get_mouse_state(mouse_state_t *state) {
    asm volatile("int $128" : : "a"(8), "b"(state) : "memory");
}

void draw_text(int x, int y, const char *str, uint32_t color, uint32_t bg_color) {
    asm volatile("int $128" : : "a"(9), "b"(x), "c"(y), "d"(str), "S"(color), "D"(bg_color) : "memory");
}

bool has_char(void) {
    uint32_t ret;
    asm volatile("int $128" : "=a"(ret) : "a"(11) : "memory");
    return ret != 0;
}

void draw_pixel(int x, int y, uint32_t color) {
    asm volatile("int $128" : : "a"(12), "b"(x), "c"(y), "d"(color) : "memory");
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
    asm volatile("int $128" : : "a"(26), "b"(x0), "c"(y0), "d"(x1), "S"(y1), "D"(color) : "memory");
}

void draw_circle(int xc, int yc, int r, uint32_t color) {
    asm volatile("int $128" : : "a"(27), "b"(xc), "c"(yc), "d"(r), "S"(color) : "memory");
}

void fill_circle(int xc, int yc, int r, uint32_t color) {
    asm volatile("int $128" : : "a"(28), "b"(xc), "c"(yc), "d"(r), "S"(color) : "memory");
}

