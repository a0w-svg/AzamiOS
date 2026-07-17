/**
 * kernel/drivers/include/packet_buffer.h — Kernel Packet Buffer header bridge
 *
 * The canonical definitions live in lib/net/packet_buffer.h.
 * Re-exported here so existing kernel/driver includes can access it cleanly.
 */
#ifndef KERNEL_DRIVERS_INCLUDE_PACKET_BUFFER_H
#define KERNEL_DRIVERS_INCLUDE_PACKET_BUFFER_H

#include "../../../lib/net/packet_buffer.h"

/* Kernel-only initialization: wires kmalloc / kfree to the packet buffer allocator */
void packet_buffer_kernel_init(void);

#endif /* KERNEL_DRIVERS_INCLUDE_PACKET_BUFFER_H */
