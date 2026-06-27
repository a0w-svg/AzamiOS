/**
 * kernel/drivers/include/net_stack.h — Kernel net_stack header bridge
 *
 * The canonical definitions live in lib/net/net_stack.h.
 * Re-exported here so existing kernel includes are unaffected.
 */
#include "../../../lib/net/net_stack.h"

/* Kernel-only: wires g_net_hal to RTL8139 and calls net_stack_init() */
void net_stack_kernel_init(void);
