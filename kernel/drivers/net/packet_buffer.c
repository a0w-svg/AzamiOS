/**
 * kernel/drivers/net/packet_buffer.c — Kernel packet buffer bridge & allocator wiring
 *
 * Includes the canonical implementation from lib/net/packet_buffer.c and hooks
 * up the kernel's physical/virtual memory allocator (kmalloc/kfree).
 */

#include "../../lib/net/packet_buffer.c"
#include "../../mem/include/mmp.h"
#include "./include/packet_buffer.h"

static void *k_pkt_alloc_wrapper(size_t size)
{
    if (size > 0xFFFFFFFFUL) {
        return NULL;
    }
    return kmalloc((uint32_t)size);
}

static void k_pkt_free_wrapper(void *ptr)
{
    kfree(ptr);
}

/**
 * packet_buffer_kernel_init - Wire up kernel memory allocator for packet buffers.
 */
void packet_buffer_kernel_init(void)
{
    packet_buffer_init_allocator(k_pkt_alloc_wrapper, k_pkt_free_wrapper);
}
