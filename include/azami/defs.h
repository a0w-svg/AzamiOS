/* ============================================================================
 * AzamiOS — Kernel Utility Macros
 * File: include/azami/defs.h
 *
 * Architecture-independent macros for alignment, containers, barriers,
 * compile-time assertions, and branch-prediction hints.
 * ============================================================================ */
#pragma once

#include "types.h"

/* --------------------------------------------------------------------------
 * Standard POSIX / Kernel Error Codes
 * -------------------------------------------------------------------------- */
#define EPERM        1  /* Operation not permitted */
#define ENOENT       2  /* No such file or directory */
#define ESRCH        3  /* No such process */
#define EINTR        4  /* Interrupted system call */
#define EIO          5  /* I/O error */
#define ENXIO        6  /* No such device or address */
#define E2BIG        7  /* Argument list too long */
#define ENOEXEC      8  /* Exec format error */
#define EBADF        9  /* Bad file number */
#define ECHILD      10  /* No child processes */
#define EAGAIN      11  /* Try again */
#define ENOMEM      12  /* Out of memory */
#define EACCES      13  /* Permission denied */
#define EFAULT      14  /* Bad address */
#define ENOTBLK     15  /* Block device required */
#define EBUSY       16  /* Device or resource busy */
#define EEXIST      17  /* File exists */
#define EXDEV       18  /* Cross-device link */
#define ENODEV      19  /* No such device */
#define ENOTDIR     20  /* Not a directory */
#define EISDIR      21  /* Is a directory */
#define EINVAL      22  /* Invalid argument */
#define ENFILE      23  /* File table overflow */
#define EMFILE      24  /* Too many open files */
#define ENOTTY      25  /* Not a typewriter */
#define ETXTBSY     26  /* Text file busy */
#define EFBIG       27  /* File too large */
#define ENOSPC      28  /* No space left on device */
#define ESPIPE      29  /* Illegal seek */
#define EROFS       30  /* Read-only file system */
#define EMLINK      31  /* Too many links */
#define EPIPE       32  /* Broken pipe */
#define EDOM        33  /* Math argument out of domain */
#define ERANGE      34  /* Math result not representable */
#define EDEADLK     35  /* Resource deadlock would occur */
#define ENAMETOOLONG 36  /* File name too long */
#define ENOLCK      37  /* No record locks available */
#define ENOSYS      38  /* Function not implemented */
#define ENOTEMPTY   39  /* Directory not empty */
#define ELOOP       40  /* Too many symbolic links encountered */
#define ENOTSOCK    88  /* Socket operation on non-socket */
#define EDESTADDRREQ 89 /* Destination address required */
#define EMSGSIZE    90  /* Message too long */
#define EPROTOTYPE  91  /* Protocol wrong type for socket */
#define ENOPROTOOPT 92  /* Protocol not available */
#define EPROTONOSUPPORT 93 /* Protocol not supported */
#define EOPNOTSUPP  95  /* Operation not supported on transport endpoint */
#define EAFNOSUPPORT 97 /* Address family not supported by protocol */
#define EADDRINUSE  98  /* Address already in use */
#define EADDRNOTAVAIL 99 /* Cannot assign requested address */
#define ENETDOWN    100 /* Network is down */
#define ENETUNREACH 101 /* Network is unreachable */
#define ECONNRESET  104 /* Connection reset by peer */
#define ENOBUFS     105 /* No buffer space available */
#define EISCONN     106 /* Transport endpoint is already connected */
#define ENOTCONN    107 /* Transport endpoint is not connected */
#define ESHUTDOWN   108 /* Cannot send after transport endpoint shutdown */
#define ETIMEDOUT   110 /* Connection timed out */
#define ECONNREFUSED 111 /* Connection refused */
#define EALREADY    114 /* Operation already in progress */
#define EINPROGRESS 115 /* Operation now in progress */

/* --------------------------------------------------------------------------
 * Compiler attribute shorthands
 * -------------------------------------------------------------------------- */
#define __packed          __attribute__((packed))
#define __aligned(n)      __attribute__((aligned(n)))
#define __noreturn        __attribute__((noreturn))
#define __noinline        __attribute__((noinline))
#define __always_inline   __attribute__((always_inline)) inline
#define __used            __attribute__((used))
#define __section(s)      __attribute__((section(s)))
#define __weak            __attribute__((weak))
#define __unused          __attribute__((unused))
#define __printf(f,a)     __attribute__((format(printf, f, a)))

/* --------------------------------------------------------------------------
 * Branch prediction hints
 * -------------------------------------------------------------------------- */
#define likely(x)    __builtin_expect(!!(x), 1)
#define unlikely(x)  __builtin_expect(!!(x), 0)

/* --------------------------------------------------------------------------
 * Array cardinality
 * -------------------------------------------------------------------------- */
#define ARRAY_SIZE(a)  (sizeof(a) / sizeof((a)[0]))

/* --------------------------------------------------------------------------
 * Alignment helpers
 * -------------------------------------------------------------------------- */
#define ALIGN_UP(v, a)   (((v) + ((__typeof__(v))(a) - 1)) & ~((__typeof__(v))(a) - 1))
#define ALIGN_DOWN(v, a) ((v) & ~((__typeof__(v))(a) - 1))
#define IS_ALIGNED(v, a) (((v) & ((__typeof__(v))(a) - 1)) == 0)

/* --------------------------------------------------------------------------
 * Container-of — get a pointer to the struct containing a member.
 *   container_of(ptr, type, member)
 * -------------------------------------------------------------------------- */
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

/* --------------------------------------------------------------------------
 * Compile-time assertion (C11 _Static_assert wrapper with a message)
 * -------------------------------------------------------------------------- */
#define BUILD_ASSERT(cond, msg)  _Static_assert(cond, msg)

/* --------------------------------------------------------------------------
 * Memory / compiler barriers
 * -------------------------------------------------------------------------- */
#define barrier()        __asm__ volatile("" ::: "memory")
#define mb()             __asm__ volatile("mfence" ::: "memory")
#define rmb()            __asm__ volatile("lfence" ::: "memory")
#define wmb()            __asm__ volatile("sfence" ::: "memory")

/* --------------------------------------------------------------------------
 * Panic shorthand (declaration; defined in kernel/panic.c)
 * -------------------------------------------------------------------------- */
__noreturn void kernel_panic(const char *fmt, ...);
#define PANIC(...)  kernel_panic(__VA_ARGS__)
#define BUG_ON(cond) \
    do { if (unlikely(cond)) PANIC("BUG_ON(%s) at %s:%d", #cond, __FILE__, __LINE__); } while (0)

/* --------------------------------------------------------------------------
 * Bit manipulation macros
 * -------------------------------------------------------------------------- */
#define BIT(n)         (1ULL << (n))
#define BIT_SET(v, n)  ((v) |=  BIT(n))
#define BIT_CLR(v, n)  ((v) &= ~BIT(n))
#define BIT_TST(v, n)  (!!((v) & BIT(n)))

/* --------------------------------------------------------------------------
 * Min / Max (type-safe via GNU __typeof__ extension)
 * -------------------------------------------------------------------------- */
#define MIN(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#define MAX(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })

/* --------------------------------------------------------------------------
 * x86_64 I/O port helpers (inline, no dependency on port.c)
 * -------------------------------------------------------------------------- */
static __always_inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}
static __always_inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}
static __always_inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port) : "memory");
}
static __always_inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}
static __always_inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port) : "memory");
}
static __always_inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}
static __always_inline void io_wait(void) {
    outb(0x80, 0);  /* Write to unused POST port — consumes ~1 µs */
}

/* --------------------------------------------------------------------------
 * CPU pause / halt helpers
 * -------------------------------------------------------------------------- */
static __always_inline void cpu_pause(void) {
    __asm__ volatile("pause" ::: "memory");
}
static __always_inline void cpu_hlt(void) {
    __asm__ volatile("hlt");
}
static __always_inline void cpu_sti(void) {
    __asm__ volatile("sti" ::: "memory");
}
static __always_inline void cpu_cli(void) {
    __asm__ volatile("cli" ::: "memory");
}
static __always_inline void cpu_halt_loop(void) {
    for (;;) { cpu_cli(); cpu_hlt(); }
}
