/* ============================================================================
 * AzamiOS — HPET (High Precision Event Timer) driver
 * File: drivers/misc/hpet.c
 *
 * The HPET exposes a free-running up-counter of known frequency via MMIO. We
 * use it purely as a passive time source (no comparators / interrupts) to give
 * clock_gettime(CLOCK_MONOTONIC) real sub-tick resolution.
 *
 * ACPI "HPET" table (after the 36-byte SDT header):
 *   +36  u32  event_timer_block_id
 *   +40  Generic Address Structure (12 bytes):
 *        +40 u8  address_space_id  (0 = system memory MMIO)
 *        +44 u64 base address
 *   +52  u8   hpet_number
 *   +53  u16  minimum_clock_tick
 *
 * MMIO registers (offsets from the mapped base):
 *   0x000  General Capabilities/ID  (bits 63:32 = COUNTER_CLK_PERIOD, in fs)
 *   0x010  General Configuration    (bit 0 = ENABLE_CNF)
 *   0x0F0  Main Counter Value (64-bit)
 * ============================================================================ */

#include "hpet.h"
#include "../../include/azami/defs.h"
#include "../../drivers/acpi/acpi.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../drivers/char/console.h"

#define HPET_REG_CAP        0x000
#define HPET_REG_CONFIG     0x010
#define HPET_REG_COUNTER    0x0F0

#define HPET_CFG_ENABLE     (1ULL << 0)
#define HPET_CAP_64BIT      (1ULL << 13)

static volatile u8 *g_hpet_base;
static u64  g_period_fs;      /* counter period in femtoseconds (10^-15 s) */
static u64  g_start_count;    /* counter value captured at hpet_init()     */
static bool g_hpet_ok;

static inline u64 hpet_rd(u32 off)
{
    return *(volatile u64 *)(g_hpet_base + off);
}
static inline void hpet_wr(u32 off, u64 val)
{
    *(volatile u64 *)(g_hpet_base + off) = val;
}

void hpet_init(void)
{
    if (g_hpet_ok) return;

    const u8 *tbl = (const u8 *)acpi_find_table("HPET");
    if (!tbl) {
        pr_debug("[HPET] No ACPI HPET table; falling back to PIT tick clock\n");
        return;
    }

    if (tbl[40] != 0) {   /* address_space_id must be system-memory MMIO */
        pr_debug("[HPET] Unsupported address space %u\n", tbl[40]);
        return;
    }

    u64 phys;
    __builtin_memcpy(&phys, tbl + 44, sizeof(phys));
    if (!phys) return;

    g_hpet_base = (volatile u8 *)vmm_map_io((phys_addr_t)phys, 0x400);
    if (!g_hpet_base) {
        pr_debug("[HPET] MMIO map failed for base 0x%llx\n", (unsigned long long)phys);
        return;
    }

    u64 cap = hpet_rd(HPET_REG_CAP);
    g_period_fs = cap >> 32;                  /* COUNTER_CLK_PERIOD */
    if (g_period_fs == 0 || g_period_fs > 100000000ULL /* >100 ns is bogus */) {
        pr_debug("[HPET] Implausible counter period %llu fs\n",
                 (unsigned long long)g_period_fs);
        return;
    }

    /* Enable the main counter (leave interrupts/comparators untouched). */
    hpet_wr(HPET_REG_CONFIG, hpet_rd(HPET_REG_CONFIG) | HPET_CFG_ENABLE);
    g_start_count = hpet_rd(HPET_REG_COUNTER);
    g_hpet_ok = true;

    u64 freq_hz = 1000000000000000ULL / g_period_fs;
    kprintf("[HPET] base=0x%llx  %llu.%03llu MHz  period=%llu fs  (%s counter)\n",
            (unsigned long long)phys,
            (unsigned long long)(freq_hz / 1000000),
            (unsigned long long)((freq_hz % 1000000) / 1000),
            (unsigned long long)g_period_fs,
            (cap & HPET_CAP_64BIT) ? "64-bit" : "32-bit");
}

bool hpet_available(void)
{
    return g_hpet_ok;
}

u64 hpet_now_ns(void)
{
    if (!g_hpet_ok) return 0;
    u64 delta = hpet_rd(HPET_REG_COUNTER) - g_start_count;   /* wraps correctly */

    /* ns = delta * period_fs / 1e6, computed without a 128-bit divide (no
     * libgcc in a freestanding kernel). Splitting keeps every product < 2^64
     * for centuries of uptime. */
    u64 whole = delta / 1000000ULL;
    u64 frac  = delta % 1000000ULL;
    return whole * g_period_fs + (frac * g_period_fs) / 1000000ULL;
}

u64 hpet_resolution_ns(void)
{
    if (!g_hpet_ok) return 0;
    u64 ns = g_period_fs / 1000000ULL;
    return ns ? ns : 1;
}
