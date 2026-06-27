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
void draw_pixel(int x, int y, uint32_t color);
void sys_acpi_info(void);
void sys_reboot(void);
void sys_net_status(void);
void sys_net_test(void);
void sys_net_ping(void);
void sys_net_arp(void);
int sys_lsmod(char *buf, int max_len);
void draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void draw_circle(int xc, int yc, int r, uint32_t color);
void fill_circle(int xc, int yc, int r, uint32_t color);

#endif

