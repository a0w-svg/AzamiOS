/**
 * lpc_client.c — AzamiOS Userspace ALPC / LPC Client Implementation
 * Optimized for zero-copy section mapping and low context switch overhead.
 */

#include <lpc_client.h>
#include <sys/syscall.h>

static inline int lpc_syscall_fast(uint32_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    int ret;
    asm volatile("int $128" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2), "d"(a3) : "memory");
    return ret;
}

int lpc_connect(const char *port_name) {
    if (!port_name) return -1;
    return lpc_syscall_fast(SYS_LPC_CONNECT, (uint64_t)(uintptr_t)port_name, 0, 0);
}

int lpc_send(int port_id, lpc_msg_t *req, lpc_msg_t *reply) {
    if (port_id < 0 || !req || !reply) return -1;
    return lpc_syscall_fast(SYS_LPC_SEND, (uint64_t)port_id, (uint64_t)(uintptr_t)req, (uint64_t)(uintptr_t)reply);
}

int lpc_send_zerocopy(int port_id, uint32_t req_type, void *section_ptr, uint32_t section_size, uint32_t arg1, uint32_t arg2, lpc_msg_t *reply) {
    if (port_id < 0 || !reply) return -1;
    lpc_msg_t req;
    req.msg_id       = 0;
    req.sender_pid   = 0;
    req.type         = req_type;
    req.arg1         = arg1;
    req.arg2         = arg2;
    req.arg3         = 0;
    req.arg4         = 0;
    req.section_ptr  = (uintptr_t)section_ptr;
    req.section_size = section_size;
    req.status       = 0;
    return lpc_send(port_id, &req, reply);
}

int lpc_stat(char *buf, int max_len) {
    if (!buf || max_len <= 0) return -1;
    return lpc_syscall_fast(SYS_LPC_STAT, (uint64_t)(uintptr_t)buf, (uint64_t)max_len, 0);
}
