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
 *
 * Canonical values live in uapi/errno.h, shared verbatim with the native
 * libc (userland/libc/include/errno.h -> a build-time copy at
 * userland/libc/include/azami/uapi/errno.h). Never redefine an E* value
 * anywhere else; add it to the canonical header instead. See
 * scripts/check_uapi_sync.sh.
 * -------------------------------------------------------------------------- */
#include "uapi/errno.h"

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
