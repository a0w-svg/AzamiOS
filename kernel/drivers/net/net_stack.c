/**
 * kernel/drivers/net_stack.c — Kernel network stack bridge
 *
 * The canonical TCP/IP implementation lives in lib/net/net_stack.c.
 * This file:
 *   1. Includes the portable stack.
 *   2. Provides the kernel-side HAL wiring that connects g_net_hal to
 *      the RTL8139 driver, so the stack can send/receive packets.
 */
#include "../../lib/net/net_stack.c"

/* ── Kernel HAL glue for the RTL8139 NIC ─────────────────────────── */
#include "./include/rtl8139.h"
#include "../klibc/include/stdio.h"

static void k_nic_send(const uint8_t *buf, uint32_t len) {
    rtl8139_send_packet(buf, len);
}

static void k_nic_get_mac(uint8_t mac[6]) {
    rtl8139_dev_t *dev = rtl8139_get_device();
    if (dev) {
        for (int i = 0; i < 6; i++) mac[i] = dev->mac[i];
    }
}

static bool k_nic_is_enabled(void) {
    return rtl8139_is_enabled();
}

static net_hal_t k_nic_hal = {
    .send       = k_nic_send,
    .get_mac    = k_nic_get_mac,
    .is_enabled = k_nic_is_enabled,
    .log        = kprintf,
};

/**
 * net_stack_kernel_init – call once at boot to wire the HAL before
 * calling net_stack_init().
 */
void net_stack_kernel_init(void) {
    g_net_hal = &k_nic_hal;
    net_stack_init();
}
