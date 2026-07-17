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
void sys_poweroff(void);
void sys_net_status(void);
void sys_net_test(void);
void sys_net_ping(void);
void sys_net_arp(void);
int sys_lsmod(char *buf, int max_len);
int sys_modreload(const char *name);
void draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void draw_circle(int xc, int yc, int r, uint32_t color);
void fill_circle(int xc, int yc, int r, uint32_t color);
void gui_scroll(int lines, uint32_t bg_color);
void sys_gfx_vsync(void);
int  sys_socket(int proto);
bool sys_bind(int sock, uint16_t port);
bool sys_listen(int sock);
bool sys_connect(int sock, uint32_t ip, uint16_t port);
int  sys_send(int sock, const uint8_t *buf, uint32_t len);
void sys_close(int sock);
int  sys_thread_create(uintptr_t entry, uintptr_t arg, uintptr_t user_stack);
void sys_yield(void);
int  sys_fork(void);

#endif

