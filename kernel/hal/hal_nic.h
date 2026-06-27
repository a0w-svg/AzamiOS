/**
 * kernel/hal/hal_nic.h  –  Network Interface Card Hardware Abstraction Layer
 *
 * Defines the interface contract used by lib/net/net_stack.c so that the
 * entire TCP/IP stack compiles without any knowledge of RTL8139 registers,
 * PCI configuration space, or port I/O.
 *
 * The RTL8139 driver fills in g_hal_nic during rtl8139_init().
 * If no NIC is present, g_hal_nic remains NULL and the stack is a no-op.
 */
#ifndef HAL_NIC_H
#define HAL_NIC_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    /**
     * send – transmit a raw Ethernet frame.
     * @param buf   frame bytes (including Ethernet header)
     * @param len   total byte count
     */
    void (*send)(const uint8_t *buf, uint32_t len);

    /**
     * get_mac – fill mac[6] with the interface hardware address.
     */
    void (*get_mac)(uint8_t mac[6]);

    /**
     * is_enabled – returns true when the NIC is operational.
     */
    bool (*is_enabled)(void);
} hal_nic_t;

/**
 * Global NIC handle — set once during NIC driver initialisation.
 */
extern hal_nic_t *g_hal_nic;

#endif /* HAL_NIC_H */
