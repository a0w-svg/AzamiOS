/* ============================================================================
 * AzamiOS libc — <linux/input.h>: the evdev interface (/dev/input/eventN)
 * ============================================================================ */
#ifndef _LINUX_INPUT_H
#define _LINUX_INPUT_H

#include <stdint.h>
#include <sys/time.h>

/* One record.  A burst of them is terminated by EV_SYN/SYN_REPORT. */
struct input_event {
    struct timeval time;
    uint16_t       type;
    uint16_t       code;
    int32_t        value;
};

struct input_id {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
};

/* ── Event types ─────────────────────────────────────────────────────────── */
#define EV_SYN  0x00
#define EV_KEY  0x01
#define EV_REL  0x02
#define EV_ABS  0x03
#define EV_MSC  0x04
#define EV_LED  0x11
#define EV_CNT  0x20

#define KEY_CNT 0x300

#define SYN_REPORT 0

/* ── Relative axes ───────────────────────────────────────────────────────── */
#define REL_X     0x00
#define REL_Y     0x01
#define REL_WHEEL 0x08

#define MSC_SCAN  0x04

/* ── LEDs ────────────────────────────────────────────────────────────────── */
#define LED_NUML    0x00
#define LED_CAPSL   0x01
#define LED_SCROLLL 0x02

/* ── Mouse buttons ───────────────────────────────────────────────────────── */
#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112
#define BTN_SIDE   0x113
#define BTN_EXTRA  0x114

/* ── ioctls.  The length-carrying ones encode it in the request. ─────────── */
#define EVIOCGVERSION    0x80044501
#define EVIOCGID         0x80084502
#define EVIOCGREP        0x80084503   /* get {delay_ms, period_ms} */
#define EVIOCSREP        0x40084503   /* set {delay_ms, period_ms} */
#define EVIOCGKEYCODE    0x80084504   /* get {scancode, keycode} (V1) */
#define EVIOCSKEYCODE    0x40084504   /* set {scancode, keycode} (V1) */
#define EVIOCGNAME(len)  (0x80000000u | ((unsigned)(len) << 16) | 0x4506u)
#define EVIOCGPHYS(len)  (0x80000000u | ((unsigned)(len) << 16) | 0x4507u)
#define EVIOCGKEY(len)   (0x80000000u | ((unsigned)(len) << 16) | 0x4518u)
#define EVIOCGBIT(ev, len) \
    (0x80000000u | ((unsigned)(len) << 16) | 0x4500u | (0x20u + (unsigned)(ev)))
#define EVIOCGRAB        0x40044590

/* Test bit @n in a bitmap returned by EVIOCGBIT/EVIOCGKEY. */
#define input_bit_is_set(map, n) (((const unsigned char *)(map))[(n) / 8] & (1u << ((n) % 8)))

#endif /* _LINUX_INPUT_H */
