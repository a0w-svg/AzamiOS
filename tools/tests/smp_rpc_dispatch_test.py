#!/usr/bin/env python3
"""Compile the production bounded IPI/RPC paths with scripted hardware stubs.

Privileged register/IRQ operations are substituted; the actual dispatch loop,
SMP validation, RPC wait/cancellation and production ownership helpers run.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    begin = source.index("{", start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    lapic_header = (ROOT / "arch/x86_64/cpu/lapic.h").read_text()
    names = ["LAPIC_ICR_LO", "LAPIC_ICR_HI", "LAPIC_ICR_FIXED",
             "LAPIC_ICR_ASSERT", "LAPIC_ICR_BUSY", "MSR_X2APIC_ICR"]
    constants = "\n".join(re.search(r"^#define " + name + r"\s+.*$",
                                    lapic_header, re.M).group(0) for name in names)
    rpc = (ROOT / "arch/x86_64/cpu/smp_rpc.c").read_text()
    rpc = rpc[rpc.index("typedef struct {"):]
    prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "arch/x86_64/cpu/smp_rpc_state.h"
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;
typedef int32_t s32;
typedef unsigned long irqflags_t;
#define SMP_MAX_CPUS 64
#define SMP_RPC_BYTES 128
#define SMP_VEC_CALL_FUNC 250
typedef struct { u32 lapic_id; u64 ipis_sent; } cpu_info_t;
static cpu_info_t g_cpu_infos[SMP_MAX_CPUS];
static u32 g_cpu_count = 4, current_cpu;
static u64 online = 15;
static bool irq_enabled, g_x2apic;
static volatile u32 mock_mmio[256], *g_lapic_mmio;
static unsigned reads, writes, pauses, guards, restores, msr_writes;
static unsigned busy_polls, gs_reads, callback_calls;
static u32 write_reg[2], write_value[2], msr_reg;
static u64 msr_value;
static bool receiver_triggered, rpc_mode;
enum { RECEIVE_NONE, RECEIVE_DONE, RECEIVE_RUNNING };
static unsigned receive_mode;
static bool smp_cpu_online(u32 cpu) { return cpu < 64 && (online & (1ULL << cpu)); }
static u32 smp_current_cpu_id(void) {
    if (rpc_mode) assert(!irq_enabled);
    gs_reads++; return current_cpu;
}
static cpu_info_t *smp_get_cpu(void) { gs_reads++; return &g_cpu_infos[current_cpu]; }
static u64 lapic_icr_guard_enter(void) {
    guards++; u64 flags = irq_enabled ? 0x202 : 2; irq_enabled = false; return flags;
}
static void lapic_icr_guard_exit(u64 flags) {
    assert(!irq_enabled); restores++; irq_enabled = !!(flags & 0x200);
}
static irqflags_t spinlock_irq_save(void) {
    irqflags_t flags = irq_enabled ? 0x202 : 2; irq_enabled = false; return flags;
}
static void spinlock_irq_restore(irqflags_t flags) { irq_enabled = !!(flags & 0x200); }
static bool ktime_try_get_ns(u64 *now) { *now = 100; return true; }
static long cmdline_get_long(const char *key, long fallback) { (void)key; return fallback; }
static u32 lapic_read(u32 reg);
static void lapic_write(u32 reg, u32 value);
static void cpu_pause(void) { assert(!irq_enabled); pauses++; }
static void wrmsr(u32 reg, u64 value) { msr_writes++; msr_reg = reg; msr_value = value; }
'''
    fixture = r'''
/* A remote receiver can race even a failed IPI send through a previous IPI.
 * Invoke the real receiver path for DONE; hold its real slot ownership for
 * RUNNING and complete it only after the caller has returned. */
static u32 lapic_read(u32 reg) {
    assert(!irq_enabled && reg == LAPIC_ICR_LO);
    reads++;
    if (!receiver_triggered && receive_mode) {
        receiver_triggered = true;
        if (receive_mode == RECEIVE_DONE) {
            u32 sender = current_cpu; current_cpu = 1;
            smp_rpc_drain(); current_cpu = sender;
        } else {
            u64 tag = g_rpc_slots[1].state & ~7ULL;
            assert(rpc_enter(&g_rpc_slots[1].state, tag));
        }
    }
    return reads <= busy_polls ? LAPIC_ICR_BUSY : 0;
}
static void lapic_write(u32 reg, u32 value) {
    assert(!irq_enabled && writes < 2);
    write_reg[writes] = reg; write_value[writes++] = value;
}
static void callback(void *data) { callback_calls++; (*(u32 *)data)++; }
static void reset(bool interrupts) {
    reads = writes = pauses = guards = restores = msr_writes = gs_reads = callback_calls = 0;
    busy_polls = 0; receive_mode = RECEIVE_NONE; receiver_triggered = false;
    rpc_mode = false;
    g_lapic_mmio = mock_mmio; g_x2apic = false; irq_enabled = interrupts;
    current_cpu = 0; online = 15; memset(g_cpu_infos, 0, sizeof(g_cpu_infos));
    for (u32 cpu = 0; cpu < 4; cpu++) g_cpu_infos[cpu].lapic_id = cpu + 16;
}
int main(void) {
    for (unsigned incoming = 0; incoming < 2; incoming++) {
        reset(incoming); g_lapic_mmio = NULL;
        assert(!lapic_send_ipi_bounded(5, 250));
        assert(irq_enabled == !!incoming && !guards && !reads && !writes);
        reset(incoming);
        assert(!lapic_send_ipi_bounded(256, 250));
        assert(irq_enabled == !!incoming && !guards && !reads);
        reset(incoming);
        assert(lapic_send_ipi_bounded(5, 250));
        assert(reads == 1 && !pauses && writes == 2 && guards == 1 && restores == 1);
        assert(write_reg[0] == LAPIC_ICR_HI && write_value[0] == (5U << 24));
        assert(write_reg[1] == LAPIC_ICR_LO && write_value[1] == (LAPIC_ICR_ASSERT | 250));
        assert(irq_enabled == !!incoming);
        reset(incoming); busy_polls = 3;
        assert(lapic_send_ipi_bounded(5, 250));
        assert(reads == 4 && pauses == 3 && writes == 2 && irq_enabled == !!incoming);
        reset(incoming); busy_polls = 4095;
        assert(lapic_send_ipi_bounded(5, 250));
        assert(reads == 4096 && pauses == 4095 && writes == 2 && irq_enabled == !!incoming);
        reset(incoming); busy_polls = 4096;
        assert(!lapic_send_ipi_bounded(5, 250));
        assert(reads == 4096 && pauses == 4096 && !writes && restores == 1);
        assert(irq_enabled == !!incoming);
        reset(incoming); g_x2apic = true;
        assert(lapic_send_ipi_bounded(0x12345678, 250));
        assert(msr_writes == 1 && msr_reg == MSR_X2APIC_ICR);
        assert(msr_value == ((0x12345678ULL << 32) | LAPIC_ICR_ASSERT | 250));
        assert(!guards && !reads && !writes && irq_enabled == !!incoming);
    }
    puts("PASS actual bounded LAPIC path: busy limit, boundary success, IRQ restore, x2APIC encoding");
    reset(true); online = 0;
    assert(!smp_send_ipi_bounded(1, 250) && !gs_reads && !guards);
    reset(true);
    assert(!smp_send_ipi_bounded(4, 250) && !gs_reads && !guards);
    online &= ~(1ULL << 2);
    assert(!smp_send_ipi_bounded(2, 250) && !gs_reads && !guards);
    assert(!smp_send_ipi_bounded(0, 250) && !guards);
    reset(true); busy_polls = 4096;
    assert(!smp_send_ipi_bounded(1, 250) && !g_cpu_infos[0].ipis_sent && irq_enabled);
    reset(true);
    assert(smp_send_ipi_bounded(1, 250) && g_cpu_infos[0].ipis_sent == 1);
    assert(write_value[0] == (17U << 24));
    puts("PASS actual SMP path: pre-GS, invalid/offline/self rejection and send accounting");
    for (unsigned incoming = 0; incoming < 2; incoming++) {
        reset(incoming); rpc_mode = true; u32 local_value = 12;
        assert(smp_rpc_call(0, callback, &local_value, sizeof(local_value), 1000) == 0);
        assert(local_value == 13 && callback_calls == 1 && g_rpc_slots[0].state == 0);
        assert(!guards && !reads && !writes && irq_enabled == !!incoming);
        reset(incoming); rpc_mode = true; busy_polls = 4096; u32 value = 77;
        assert(smp_rpc_call(1, callback, &value, sizeof(value), 1000) == -ETIMEDOUT);
        assert(value == 77 && !callback_calls && g_rpc_slots[1].state == 0);
        assert(reads == 4096 && !writes && irq_enabled == !!incoming);
        reset(incoming); rpc_mode = true; busy_polls = 4096; receive_mode = RECEIVE_DONE;
        assert(smp_rpc_call(1, callback, &value, sizeof(value), 1000) == 0);
        assert(value == 78 && callback_calls == 1 && g_rpc_slots[1].state == 0);
        assert(irq_enabled == !!incoming);
        reset(incoming); rpc_mode = true; busy_polls = 4096; receive_mode = RECEIVE_RUNNING;
        assert(smp_rpc_call(1, callback, &value, sizeof(value), 1000) == -ETIMEDOUT);
        u64 tag = g_rpc_slots[1].state & ~7ULL;
        assert(g_rpc_slots[1].state == (tag | RPC_ABANDONED));
        assert(!rpc_claim(&g_rpc_slots[1].state, tag + 8) && value == 78);
        g_rpc_slots[1].func(g_rpc_slots[1].payload);
        rpc_complete(&g_rpc_slots[1].state, tag);
        assert(g_rpc_slots[1].state == 0 && value == 78 && callback_calls == 1);
        assert(irq_enabled == !!incoming);
    }
    puts("PASS actual RPC path: failed dispatch cancels pending, DONE wins, RUNNING retains only by-value storage");
    return 0;
}
'''
    code = prelude + constants + "\n"
    code += function("arch/x86_64/cpu/lapic.c", "bool lapic_send_ipi_bounded(") + "\n"
    code += function("arch/x86_64/cpu/smp.c", "bool smp_send_ipi_bounded(") + "\n"
    code += rpc + fixture
    with tempfile.TemporaryDirectory(prefix="azami-rpc-dispatch-", dir="/tmp") as folder:
        source = Path(folder) / "test.c"
        binary = Path(folder) / "test"
        source.write_text(code)
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT), str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    main()
