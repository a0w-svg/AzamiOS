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
#define EV_CNT  0x20

#define SYN_REPORT 0

/* ── Relative axes ───────────────────────────────────────────────────────── */
#define REL_X     0x00
#define REL_Y     0x01
#define REL_WHEEL 0x08

#define MSC_SCAN  0x04

/* ── Mouse buttons ───────────────────────────────────────────────────────── */
#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112
#define BTN_SIDE   0x113
#define BTN_EXTRA  0x114

/* ── ioctls.  The length-carrying ones encode it in the request. ─────────── */
#define EVIOCGVERSION    0x80044501
#define EVIOCGID         0x80084502
#define EVIOCGNAME(len)  (0x80000000u | ((unsigned)(len) << 16) | 0x4506u)
#define EVIOCGPHYS(len)  (0x80000000u | ((unsigned)(len) << 16) | 0x4507u)
#define EVIOCGBIT(ev, len) \
    (0x80000000u | ((unsigned)(len) << 16) | 0x4500u | (0x20u + (unsigned)(ev)))
#define EVIOCGRAB        0x40044590

/* Test bit @n in a bitmap returned by EVIOCGBIT. */
#define input_bit_is_set(map, n) (((const unsigned char *)(map))[(n) / 8] & (1u << ((n) % 8)))

#endif /* _LINUX_INPUT_H */
