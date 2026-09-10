/* ============================================================================
 * AzamiOS — Kernel Function Tracer (ktrace)
 * File: kernel/perf/ktrace.c
 * ============================================================================ */

#include "ktrace.h"
#include "../lib/string.h"
#include "../sched/sched.h"
#include "../../arch/x86_64/cpu/smp.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../drivers/misc/hpet.h"
#include "../../include/azami/defs.h"

volatile bool g_ktrace_any_enabled = false;

static ktrace_entry_t g_ktrace_ring[KTRACE_RING_SIZE];
static u32            g_ring_head = 0;
static u32            g_ring_tail = 0;
static u32            g_ring_count = 0;
static spinlock_t     g_ktrace_lock = SPINLOCK_INIT;

static char           g_filters[KTRACE_FILTER_MAX][KTRACE_NAME_MAX];
static int            g_filter_count = 0;

void ktrace_init(void)
{
    irqflags_t flags = spinlock_lock_irqsave(&g_ktrace_lock);
    g_ring_head = 0;
    g_ring_tail = 0;
    g_ring_count = 0;
    g_filter_count = 0;
    g_ktrace_any_enabled = false;
    spinlock_unlock_irqrestore(&g_ktrace_lock, flags);
}

static bool ktrace_name_matches_locked(const char *name)
{
    if (g_filter_count == 0) return false;
    for (int i = 0; i < g_filter_count; i++) {
        if (strcmp(g_filters[i], "all") == 0) return true;
        if (strcmp(g_filters[i], name) == 0) return true;
    }
    return false;
}

bool ktrace_is_enabled(const char *name)
{
    if (!g_ktrace_any_enabled || !name) return false;
    irqflags_t flags = spinlock_lock_irqsave(&g_ktrace_lock);
    bool match = ktrace_name_matches_locked(name);
    spinlock_unlock_irqrestore(&g_ktrace_lock, flags);
    return match;
}

void ktrace_record(const char *name, u64 arg0)
{
    if (!g_ktrace_any_enabled || !name) return;

    irqflags_t flags = spinlock_lock_irqsave(&g_ktrace_lock);
    if (!ktrace_name_matches_locked(name)) {
        spinlock_unlock_irqrestore(&g_ktrace_lock, flags);
        return;
    }

    u64 ts = hpet_available() ? hpet_now_ns() : (sched_get_ticks() * 10000000ULL);
    u32 cpu = smp_current_cpu_id();
    process_t *p = sched_current_process();
    u32 pid = p ? p->pid : 0;

    ktrace_entry_t *e = &g_ktrace_ring[g_ring_head];
    e->ts = ts;
    e->cpu = cpu;
    e->pid = pid;
    strncpy(e->name, name, KTRACE_NAME_MAX - 1);
    e->name[KTRACE_NAME_MAX - 1] = '\0';
    e->arg0 = arg0;

    g_ring_head = (g_ring_head + 1) % KTRACE_RING_SIZE;
    if (g_ring_count < KTRACE_RING_SIZE) {
        g_ring_count++;
    } else {
        g_ring_tail = (g_ring_tail + 1) % KTRACE_RING_SIZE;
    }

    spinlock_unlock_irqrestore(&g_ktrace_lock, flags);
}

int ktrace_enable(const char *name)
{
    if (!name || name[0] == '\0') return -(int)EINVAL;

    irqflags_t flags = spinlock_lock_irqsave(&g_ktrace_lock);
    for (int i = 0; i < g_filter_count; i++) {
        if (strcmp(g_filters[i], name) == 0) {
            g_ktrace_any_enabled = true;
            spinlock_unlock_irqrestore(&g_ktrace_lock, flags);
            return 0;
        }
    }

    if (g_filter_count >= KTRACE_FILTER_MAX) {
        spinlock_unlock_irqrestore(&g_ktrace_lock, flags);
        return -(int)ENOSPC;
    }

    strncpy(g_filters[g_filter_count], name, KTRACE_NAME_MAX - 1);
    g_filters[g_filter_count][KTRACE_NAME_MAX - 1] = '\0';
    g_filter_count++;
    g_ktrace_any_enabled = true;

    spinlock_unlock_irqrestore(&g_ktrace_lock, flags);
    return 0;
}

int ktrace_disable(const char *name)
{
    if (!name) return -(int)EINVAL;

    irqflags_t flags = spinlock_lock_irqsave(&g_ktrace_lock);
    if (strcmp(name, "all") == 0 || strcmp(name, "*") == 0 || name[0] == '\0') {
        g_filter_count = 0;
        g_ktrace_any_enabled = false;
        spinlock_unlock_irqrestore(&g_ktrace_lock, flags);
        return 0;
    }

    int found = -1;
    for (int i = 0; i < g_filter_count; i++) {
        if (strcmp(g_filters[i], name) == 0) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        for (int i = found; i < g_filter_count - 1; i++) {
            memcpy(g_filters[i], g_filters[i + 1], KTRACE_NAME_MAX);
        }
        g_filter_count--;
        if (g_filter_count == 0) {
            g_ktrace_any_enabled = false;
        }
    }

    spinlock_unlock_irqrestore(&g_ktrace_lock, flags);
    return (found >= 0) ? 0 : -(int)ENOENT;
}

void ktrace_ring_clear(void)
{
    irqflags_t flags = spinlock_lock_irqsave(&g_ktrace_lock);
    g_ring_head = 0;
    g_ring_tail = 0;
    g_ring_count = 0;
    spinlock_unlock_irqrestore(&g_ktrace_lock, flags);
}

size_t ktrace_read(ktrace_entry_t *out, size_t max)
{
    if (!out || max == 0) return 0;

    irqflags_t flags = spinlock_lock_irqsave(&g_ktrace_lock);
    size_t count = 0;
    while (count < max && g_ring_count > 0) {
        out[count] = g_ktrace_ring[g_ring_tail];
        g_ring_tail = (g_ring_tail + 1) % KTRACE_RING_SIZE;
        g_ring_count--;
        count++;
    }
    spinlock_unlock_irqrestore(&g_ktrace_lock, flags);
    return count;
}

size_t ktrace_read_text(char *buf, size_t max_bytes)
{
    if (!buf || max_bytes == 0) return 0;

    irqflags_t flags = spinlock_lock_irqsave(&g_ktrace_lock);
    size_t written = 0;
    while (g_ring_count > 0) {
        ktrace_entry_t *e = &g_ktrace_ring[g_ring_tail];
        char line[128];
        u64 sec = e->ts / 1000000000ULL;
        u64 nsec = e->ts % 1000000000ULL;
        int n = scnprintf(line, sizeof(line), "[%llu.%06llu] cpu%u pid%u %s arg=0x%llx\n",
                          sec, nsec / 1000, e->cpu, e->pid, e->name, e->arg0);
        if (n <= 0) break;
        if (written + (size_t)n >= max_bytes) break;

        memcpy(buf + written, line, n);
        written += n;

        g_ring_tail = (g_ring_tail + 1) % KTRACE_RING_SIZE;
        g_ring_count--;
    }
    buf[written] = '\0';
    spinlock_unlock_irqrestore(&g_ktrace_lock, flags);
    return written;
}

size_t ktrace_get_enabled_list(char *buf, size_t max_bytes)
{
    if (!buf || max_bytes == 0) return 0;

    irqflags_t flags = spinlock_lock_irqsave(&g_ktrace_lock);
    size_t written = 0;
    for (int i = 0; i < g_filter_count; i++) {
        int n = scnprintf(buf + written, max_bytes - written, "%s\n", g_filters[i]);
        if (n <= 0 || written + (size_t)n >= max_bytes) break;
        written += n;
    }
    buf[written] = '\0';
    spinlock_unlock_irqrestore(&g_ktrace_lock, flags);
    return written;
}
