#include "include/gui.h"

void init_graphics(void) {
    asm volatile("int $128" : : "a"(5));
}

void gfx_flip(void) {
    asm volatile("int $128" : : "a"(6));
}

void draw_rect(int x, int y, int w, int h, uint32_t color) {
    asm volatile("int $128" : : "a"(7), "b"(x), "c"(y), "d"(w), "S"(h), "D"(color));
}

void get_mouse_state(mouse_state_t *state) {
    asm volatile("int $128" : : "a"(8), "b"(state) : "memory");
}

void draw_text(int x, int y, const char *str, uint32_t color, uint32_t bg_color) {
    asm volatile("int $128" : : "a"(9), "b"(x), "c"(y), "d"(str), "S"(color), "D"(bg_color));
}

bool has_char(void) {
    uint32_t ret;
    asm volatile("int $128" : "=a"(ret) : "a"(11));
    return ret != 0;
}
