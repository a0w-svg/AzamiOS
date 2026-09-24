#pragma once
#include <stdbool.h>
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/time.h"
#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/sys/sysinfo.h"
#include "../../libc/include/sys/statvfs.h"
#include "../../libc/include/sys/reboot.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/sys/ioctl.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#if __has_include(<azami/font.h>)
#include <azami/font.h>
#elif __has_include("../shared/az_font.h")
#include "../shared/az_font.h"
#elif __has_include("../../libc/include/azami/font.h")
#include "../../libc/include/azami/font.h"
#endif
#include "../shared/ui_kit.h"
#include "../shared/sys_config.h"

extern uk_window_t g_win;
extern int g_active_tab;

#define SERVER_CHAN 1

void draw_settings(void);
int hit_toggle(int tx, int ty, int mx, int my);
int hit_toggle_wide(int tx, int ty, int mx, int my, int width);
void draw_toggle(int x, int y, int on, const char *label);

extern int g_vsync;
extern int g_composit;
extern int g_cursor_aa;
extern int g_volume_pct;
extern int g_theme_selected;
extern int g_selected_tz_idx;
extern int g_net_focus;
extern char g_net_ip[32];
extern char g_net_gw[32];
extern char g_net_dns[32];
extern char g_power_status_msg[128];
extern int g_sec_dmesg;
extern int g_sec_kptr;
extern int g_sec_mmap;
extern int g_sec_yama;
extern int g_sec_hlinks;
extern int g_sec_slinks;
extern int g_sec_auto_ipc;
extern int g_sec_auto_admin;
extern int g_sec_auto_dhcp;
extern int g_sec_auto_trace;

void draw_display_tab(void);
void handle_display_mouse(int mx, int my);
void draw_audio_tab(void);
void handle_audio_mouse(int mx, int my);
void draw_theme_tab(void);
void handle_theme_mouse(int mx, int my);
void draw_time_tab(void);
void handle_time_mouse(int mx, int my);
void draw_network_tab(void);
void handle_network_mouse(int mx, int my);
void draw_power_tab(void);
void handle_power_mouse(int mx, int my);
void draw_disks_tab(void);
void handle_disks_mouse(int mx, int my);
void draw_security_tab(void);
void handle_security_mouse(int mx, int my);
void draw_system_tab(void);
void handle_system_mouse(int mx, int my);

void apply_volume(int pct);
void play_test_chime(void);
void apply_theme(int theme_id);
void save_display_settings(void);
const char *theme_brightness_label(const az_theme_t *t);
void init_timezone_setting(void);
void apply_timezone(int idx);
void refresh_network_stats(void);
void apply_static_network(void);
void apply_dhcp_network(void);
void apply_power_profile(int profile_idx);
void apply_screen_timeout(int mins);
void clean_temp_files(void);
void load_security_config(void);
void save_security_config(void);
void write_proc_val(const char *path, int val);

extern int g_net_dhcp;
extern char g_net_netmask[32];
extern char g_net_gateway[32];
void draw_input_box(int x, int y, int w, int h, const char *text, int focus);
void init_network_settings(void);
void load_power_config(void);
extern char g_net_status_msg[128];
extern unsigned int g_net_status_col;
extern int g_power_profile;
extern char g_disk_status_msg[128];
extern int g_screen_timeout;
void draw_storage_card(int x, int y, int w, int h, const char *title, const char *mnt, const char *type, const char *path);

typedef struct {
    const char *label;
    const char *tz_id;
    const char *offset_desc;
} tz_setting_item_t;
extern const tz_setting_item_t g_tz_settings_list[8];
void init_security_settings(void);
