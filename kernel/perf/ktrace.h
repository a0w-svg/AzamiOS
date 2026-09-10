/* ============================================================================
 * AzamiOS — Kernel Function Tracer (ktrace)
 * File: kernel/perf/ktrace.h
 *
 * Lightweight in-kernel tracing facility with a ring buffer and dynamic enable
 * filtering. Exposed via /sys/kernel/trace/ and syscalls SYS_AZ_KTRACE_*.
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../arch/x86_64/cpu/spinlock.h"

#define KTRACE_RING_SIZE    4096
#define KTRACE_NAME_MAX     48
#define KTRACE_FILTER_MAX   64

typedef struct {
    u64  ts;
    u32  cpu;
    u32  pid;
    char name[KTRACE_NAME_MAX];
    u64  arg0;
} ktrace_entry_t;

extern volatile bool g_ktrace_any_enabled;

void   ktrace_init(void);
void   ktrace_record(const char *name, u64 arg0);
bool   ktrace_is_enabled(const char *name);
int    ktrace_enable(const char *name);
int    ktrace_disable(const char *name);
void   ktrace_ring_clear(void);
size_t ktrace_read(ktrace_entry_t *out, size_t max);
size_t ktrace_read_text(char *buf, size_t max_bytes);
size_t ktrace_get_enabled_list(char *buf, size_t max_bytes);

#define KTRACE_CALL(fn_name, arg) \
    do { \
        if (__builtin_expect(g_ktrace_any_enabled, 0)) { \
            ktrace_record((fn_name), (u64)(arg)); \
        } \
    } while (0)
