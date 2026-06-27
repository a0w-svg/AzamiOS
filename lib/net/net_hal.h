/**
 * lib/net/net_hal.h  –  Network Hardware Abstraction for lib/net/net_stack.c
 *
 * This header provides the only hardware-facing symbols that net_stack.c
 * needs.  The kernel wires up g_net_hal at boot; net_stack.c never calls
 * rtl8139_*() directly, so the entire stack can be compiled standalone.
 */
#ifndef NET_HAL_H
#define NET_HAL_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    void (*send)(const uint8_t *buf, uint32_t len);
    void (*get_mac)(uint8_t mac[6]);
    bool (*is_enabled)(void);
    /** Optional: kprintf-compatible log function (may be NULL in tests). */
    void (*log)(const char *fmt, ...);
} net_hal_t;

/** Set once by the kernel before calling net_stack_init(). */
extern net_hal_t *g_net_hal;

#endif /* NET_HAL_H */
