#ifndef GUI_H
#define GUI_H
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int32_t x;
    int32_t y;
    bool left_btn;
    bool right_btn;
    bool middle_btn;
} mouse_state_t;

void init_graphics(void);
void gfx_flip(void);
void draw_rect(int x, int y, int w, int h, uint32_t color);
void get_mouse_state(mouse_state_t *state);
void draw_text(int x, int y, const char *str, uint32_t color, uint32_t bg_color);
bool has_char(void);

#endif
