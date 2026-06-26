#ifndef MOUSE_H
#define MOUSE_H
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int32_t x;
    int32_t y;
    bool left_btn;
    bool right_btn;
    bool middle_btn;
} mouse_state_t;

void init_mouse(void);
mouse_state_t* mouse_get_state(void);

#endif