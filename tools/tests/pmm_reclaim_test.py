#!/usr/bin/env python3
"""Host the actual PMM callback/sweep with scripted CPU, buddy and RPC stubs."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    position = source.index("{", start)
    depth, end = 1, position + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def define(source, name):
    lines = source.splitlines()
    start = next(i for i, line in enumerate(lines)
                 if re.match(r"#define " + name + r"(?:\s|\()", line))
    end = start
    while lines[end].endswith("\\"):
        end += 1
    return "\n".join(lines[start:end + 1])


PRELUDE = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint64_t phys_addr_t;
typedef unsigned long irqflags_t;
#define SMP_MAX_CPUS 64
#define PMM_ZONE_COUNT 2
#define PMM_ZONE_DMA32 0
#define PMM_ZONE_NORMAL 1
#define PANIC(...) assert(!"unexpected production panic")
"""

PLATFORM = r"""
static pcp_t g_pcp[SMP_MAX_CPUS][PMM_ZONE_COUNT], saved[SMP_MAX_CPUS][PMM_ZONE_COUNT];
static bool g_pmm_percpu_ready = true, irq_enabled, lock_allowed, locked;
static int g_pmm_lock;
static u32 g_pmm_pressure, g_pmm_reclaim_running, g_pmm_reclaim_next_cpu;
static u32 g_pmm_reclaim_retry_zones, g_pmm_reclaim_remaining[PMM_ZONE_COUNT];
static pmm_reclaim_stat_t g_pmm_reclaim_stat;
static unsigned current_cpu, cpus, lock_attempts, unlocks, freed[64][2];
static u64 online, clock_value, clock_step;
static unsigned clock_calls, clock_fail_at, rpc_calls, issued_pages;
static unsigned issued_cpu[64];
static bool publish_normal;
enum { EXECUTE, TIMEOUT, BUSY };
static unsigned rpc_mode;
static pmm_reclaim_rpc_t late_payload[64];
static void (*late_callback[64])(void *);
static irqflags_t irq_save(void) {
    irqflags_t flags = irq_enabled ? 0x202 : 2; irq_enabled = false; return flags;
}
static void irq_restore(irqflags_t flags) { irq_enabled = !!(flags & 0x200); }
static u32 smp_current_cpu_id(void) { assert(!irq_enabled); return current_cpu; }
static bool spinlock_try_lock(int *lock) {
    assert(lock == &g_pmm_lock && !irq_enabled && !locked); lock_attempts++;
    if (lock_allowed) locked = true;
    return lock_allowed;
}
static void spinlock_unlock(int *lock) {
    assert(lock == &g_pmm_lock && locked && !irq_enabled); locked = false; unlocks++;
}
static bool zone_free_locked(phys_addr_t phys, u32 order) {
    unsigned owner = (unsigned)(phys / 1000000) - 1;
    unsigned zone = (unsigned)(phys % 1000000) / 10000;
    assert(!irq_enabled && locked && order == 0 && owner == current_cpu && zone < 2);
    freed[owner][zone]++;
    if (publish_normal) {
        publish_normal = false;
        __atomic_fetch_or(&g_pmm_pressure, 2U, __ATOMIC_RELAXED);
    }
    return true;
}
static u32 smp_cpu_count(void) { return cpus; }
static bool smp_cpu_online(u32 cpu) { return cpu < 64 && (online & (1ULL << cpu)); }
static bool ktime_try_get_ns(u64 *now) {
    clock_calls++;
    if (clock_fail_at && clock_calls >= clock_fail_at) return false;
    *now = clock_value; clock_value += clock_step; return true;
}
static u64 pcp_cached(u32 zone) {
    u64 pages = 0;
    for (u32 cpu = 0; cpu < cpus; cpu++) pages += g_pcp[cpu][zone].count;
    return pages;
}
static int smp_rpc_call(u32 cpu, void (*callback)(void *), void *value,
                        size_t size, u64 deadline) {
    assert(!locked && size == sizeof(pmm_reclaim_rpc_t) && rpc_calls < 64);
    pmm_reclaim_rpc_t payload; memcpy(&payload, value, size);
    assert(payload.budget <= 32 && payload.zones && !(payload.zones & ~3U));
    assert(deadline > 0 && deadline <= 50000100ULL);
    issued_cpu[rpc_calls] = cpu; issued_pages += payload.budget;
    late_payload[cpu] = payload; late_callback[cpu] = callback; rpc_calls++;
    if (rpc_mode == TIMEOUT) return -ETIMEDOUT;
    if (rpc_mode == BUSY) return -EBUSY;
    unsigned sender = current_cpu; current_cpu = cpu;
    callback(&payload); current_cpu = sender;
    memcpy(value, &payload, size);
    return 0;
}
"""

CASES = r"""
static void reset(bool interrupts) {
    memset(g_pcp, 0, sizeof(g_pcp)); memset(freed, 0, sizeof(freed));
    memset(&g_pmm_reclaim_stat, 0, sizeof(g_pmm_reclaim_stat));
    g_pmm_pressure = g_pmm_reclaim_running = g_pmm_reclaim_next_cpu = 0;
    g_pmm_reclaim_retry_zones = 0;
    memset(g_pmm_reclaim_remaining, 0, sizeof(g_pmm_reclaim_remaining));
    irq_enabled = interrupts; lock_allowed = true; locked = false;
    current_cpu = 3; cpus = 16; online = 0xffff;
    lock_attempts = unlocks = rpc_calls = issued_pages = 0;
    clock_calls = clock_fail_at = 0; clock_value = 100; clock_step = 0;
    publish_normal = false; rpc_mode = EXECUTE;
    for (unsigned cpu = 0; cpu < 64; cpu++)
        for (unsigned zone = 0; zone < 2; zone++) {
            g_pcp[cpu][zone].count = 64;
            for (unsigned page = 0; page < 64; page++)
                g_pcp[cpu][zone].page[page] = (cpu + 1) * 1000000 + zone * 10000 + page + 1;
        }
    memcpy(saved, g_pcp, sizeof(saved));
}
static void other_owners_unchanged(void) {
    for (unsigned cpu = 0; cpu < 64; cpu++)
        if (cpu != current_cpu) assert(!memcmp(saved[cpu], g_pcp[cpu], sizeof(saved[cpu])));
}
int main(void) {
    for (unsigned incoming = 0; incoming < 2; incoming++) {
        reset(incoming);
        pmm_reclaim_rpc_t both = {.zones = 3, .budget = 32};
        pmm_reclaim_callback(&both);
        assert(both.moved == 32 && freed[3][0] == 16 && freed[3][1] == 16);
        assert(g_pcp[3][0].count == 48 && g_pcp[3][1].count == 48);
        assert(g_pmm_reclaim_stat.reclaimed_pages == 32 && unlocks == 1);
        assert(irq_enabled == !!incoming && !locked); other_owners_unchanged();

        reset(incoming);
        pmm_reclaim_rpc_t dma = {.zones = 1, .budget = 32};
        pmm_reclaim_callback(&dma);
        assert(dma.moved == 32 && freed[3][0] == 32 && !freed[3][1]);
        assert(g_pcp[3][1].count == 64 && irq_enabled == !!incoming);
        other_owners_unchanged();

        reset(incoming); lock_allowed = false;
        pmm_reclaim_rpc_t busy = {.zones = 3, .budget = 32};
        pmm_reclaim_callback(&busy);
        assert(!busy.moved && busy.busy == 1 && lock_attempts == 1 && !unlocks);
        assert(!memcmp(saved, g_pcp, sizeof(saved)) && !g_pmm_reclaim_stat.reclaimed_pages);
        assert(g_pmm_reclaim_stat.lock_busy == 1 && irq_enabled == !!incoming);

        reset(incoming);
        pmm_reclaim_rpc_t oversized = {.zones = 3, .budget = 1000};
        pmm_reclaim_callback(&oversized);
        assert(oversized.moved == 32 && freed[3][0] == 16 && freed[3][1] == 16);
        assert(irq_enabled == !!incoming); other_owners_unchanged();

        reset(incoming); rpc_mode = TIMEOUT; g_pmm_pressure = 3;
        assert(pmm_reclaim_sweep() && rpc_calls == 8 && issued_pages == 256);
        for (unsigned i = 0; i < 8; i++) assert(issued_cpu[i] == i);
        assert(g_pmm_reclaim_stat.rpc_timeouts == 8 && !g_pmm_pressure && g_pmm_reclaim_retry_zones == 3);
        assert(!memcmp(saved, g_pcp, sizeof(saved)) && !g_pmm_reclaim_running);
        rpc_calls = issued_pages = 0;
        assert(pmm_reclaim_sweep() && rpc_calls == 8 && issued_pages == 256);
        for (unsigned i = 0; i < 8; i++) assert(issued_cpu[i] == i + 8);
        /* Complete copied payloads after both callers have returned. */
        for (unsigned cpu = 0; cpu < 16; cpu++) {
            current_cpu = cpu; late_callback[cpu](&late_payload[cpu]);
        }
        assert(g_pmm_reclaim_stat.reclaimed_pages == 512 && irq_enabled == !!incoming);

        reset(incoming); cpus = 1; online = 1; current_cpu = 0;
        g_pcp[0][0].count = 1; g_pcp[0][1].count = 0;
        g_pmm_pressure = 1; publish_normal = true;
        assert(pmm_reclaim_sweep() && rpc_calls == 1 && g_pmm_pressure == 2);
        assert(g_pmm_reclaim_stat.reclaimed_pages == 1 && irq_enabled == !!incoming);

        reset(incoming); g_pmm_pressure = 3; clock_fail_at = 1;
        assert(pmm_reclaim_sweep() && !rpc_calls && clock_calls == 1);
        assert(g_pmm_reclaim_stat.time_unavailable == 1 && g_pmm_reclaim_retry_zones == 3);
        assert(!g_pmm_reclaim_running && irq_enabled == !!incoming);

        reset(incoming); g_pmm_pressure = 3; clock_fail_at = 3;
        assert(pmm_reclaim_sweep() && rpc_calls == 1 && clock_calls == 3);
        assert(g_pmm_reclaim_stat.time_unavailable == 1 && g_pmm_reclaim_retry_zones == 3);
        reset(incoming); g_pmm_pressure = 3; clock_step = 20000000;
        assert(pmm_reclaim_sweep() && rpc_calls == 2 && clock_calls == 4);
        assert(!g_pmm_reclaim_running && irq_enabled == !!incoming);

        reset(incoming); g_pmm_pressure = 3; online &= ~(1ULL << 2);
        assert(pmm_reclaim_sweep() && rpc_calls == 7 && issued_pages == 224);
        assert(g_pmm_reclaim_stat.offline_cpus == 1);
        for (unsigned i = 0; i < rpc_calls; i++) assert(issued_cpu[i] != 2);

        reset(incoming); g_pmm_pressure = 3; rpc_mode = BUSY;
        assert(pmm_reclaim_sweep() && g_pmm_reclaim_stat.rpc_busy == 8 && g_pmm_reclaim_retry_zones == 3);
        assert(!memcmp(saved, g_pcp, sizeof(saved)));

        reset(incoming); cpus = 4; online = 15; rpc_mode = BUSY; g_pmm_pressure = 3;
        for (unsigned pass = 0; pass < 8; pass++) assert(pmm_reclaim_sweep());
        assert(g_pmm_reclaim_stat.sweeps == 8 && g_pmm_reclaim_stat.pressure_batches == 1);
        assert(!g_pmm_pressure && !g_pmm_reclaim_retry_zones && !pmm_reclaim_sweep());
        assert(rpc_calls == 32 && !memcmp(saved, g_pcp, sizeof(saved)));
        g_pmm_pressure = 1; /* A genuinely new failure rearms only its zone. */
        assert(pmm_reclaim_sweep() && g_pmm_reclaim_stat.pressure_batches == 2);
        assert(g_pmm_reclaim_retry_zones == 1 && g_pmm_reclaim_remaining[0] == 7);
        assert(!g_pmm_reclaim_remaining[1] && irq_enabled == !!incoming);

        reset(incoming); cpus = 64; online = UINT64_MAX; rpc_mode = BUSY; g_pmm_pressure = 3;
        for (unsigned pass = 0; pass < 64; pass++) {
            rpc_calls = issued_pages = 0;
            assert(pmm_reclaim_sweep() && rpc_calls == 8 && issued_pages == 256);
        }
        assert(g_pmm_reclaim_stat.sweeps == 64 && g_pmm_reclaim_stat.pressure_batches == 1);
        assert(!g_pmm_reclaim_retry_zones && !pmm_reclaim_sweep());

        reset(incoming); g_pmm_pressure = 3; g_pmm_reclaim_running = 1;
        assert(!pmm_reclaim_sweep() && !rpc_calls && !clock_calls && g_pmm_pressure == 3);
        reset(incoming); assert(!pmm_reclaim_sweep() && !rpc_calls && !clock_calls);
    }
    puts("PASS production PMM callback: zone fairness, DMA32 quota, busy lock, owner isolation, cap, IRQ restore");
    puts("PASS production PMM sweep: timeout budget, CPU rotation, copied late payload, concurrent pressure, clock failure, offline/busy targets");
    puts("PASS production PMM epoch: persistent caches expire at 8/64 sweeps; new failure rearms only its zone");
    return 0;
}
"""


def main():
    source = (ROOT / "kernel/mm/pmm.c").read_text()
    header = (ROOT / "kernel/mm/pmm.h").read_text()
    names = ["PCP_CAP", "PMM_RECLAIM_CPUS", "PMM_RECLAIM_PAGES",
             "PMM_RECLAIM_CALLBACK_PAGES", "PMM_RECLAIM_SWEEP_NS",
             "PMM_RECLAIM_RPC_NS", "PMM_RECLAIM_ALL", "RECLAIM_ADD"]
    constants = "\n".join(define(source, name) for name in names)
    pcp = re.search(r"typedef struct \{\s*u32\s+count;.*?} pcp_t;", source, re.S).group(0)
    stats = re.search(r"typedef struct \{\s*u64 pressure_batches.*?} pmm_reclaim_stat_t;", header, re.S).group(0)
    payload = re.search(r"typedef struct \{ u32 zones,.*?} pmm_reclaim_rpc_t;", source).group(0)
    unit = "\n".join([PRELUDE, constants, pcp, stats, payload, PLATFORM,
                      function(source, "static void pmm_reclaim_callback("),
                      function(source, "static bool pmm_reclaim_sweep("), CASES])
    compiler = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="azami-pmm-reclaim-host-") as temporary:
        executable = Path(temporary) / "test"
        subprocess.run(compiler + ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                       "-fsanitize=undefined", "-fsanitize-undefined-trap-on-error",
                       "-x", "c", "-", "-o", str(executable)], input=unit,
                       text=True, cwd=temporary, check=True)
        subprocess.run([str(executable)], cwd=temporary, check=True)


if __name__ == "__main__":
    main()
