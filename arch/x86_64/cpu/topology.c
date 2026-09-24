/* ============================================================================
 * AzamiOS — CPU Topology Enumeration Implementation
 * File: arch/x86_64/cpu/topology.c
 * ============================================================================ */

#include "topology.h"
#include "cpu.h"
#include "msr.h"
#include "lapic.h"
#include "smp.h"
#include "../../../drivers/char/console.h"
#include "../../../include/azami/defs.h"

static cpu_topology_t g_topo[SMP_MAX_CPUS];
static u32  g_nr_packages = 1;
static u32  g_nr_cores    = 1;
static bool g_smt_active  = false;

/* CPUID leaf 0xB / 0x1F level types (ECX[15:8]). */
#define TOPO_LEVEL_INVALID 0
#define TOPO_LEVEL_SMT     1
#define TOPO_LEVEL_CORE    2
#define TOPO_LEVEL_MODULE  3
#define TOPO_LEVEL_TILE    4
#define TOPO_LEVEL_DIE     5

/*
 * Decode via the extended topology leaves.
 *
 * Leaves 0x1F and 0xB share a format: subleaf n describes one level of the
 * hierarchy, EAX[4:0] gives the number of APIC-ID bits to shift right to move
 * *past* that level, and ECX[15:8] names the level. Walking the subleaves
 * until the level type reads INVALID yields the shift amounts that separate
 * thread from core from die from package.
 *
 * 0x1F is the superset — it adds DIE (and on some parts MODULE/TILE) — and is
 * defined to be enumerated in preference to 0xB where both exist.
 */
static bool topo_decode_extended(u32 leaf, u32 apic_id, cpu_topology_t *t)
{
    u32 eax, ebx, ecx, edx;
    u32 smt_shift = 0, core_shift = 0, die_shift = 0;
    bool saw_smt = false, saw_core = false, saw_die = false;
    u32 threads_at_smt = 1;

    for (u32 sub = 0; sub < 16; sub++) {
        cpuid(leaf, sub, &eax, &ebx, &ecx, &edx);

        u32 level_type  = (ecx >> 8) & 0xFF;
        u32 shift_width = eax & 0x1F;

        if (level_type == TOPO_LEVEL_INVALID) break;

        switch (level_type) {
        case TOPO_LEVEL_SMT:
            smt_shift = shift_width;
            threads_at_smt = ebx & 0xFFFF;
            saw_smt = true;
            break;
        case TOPO_LEVEL_CORE:
            core_shift = shift_width;
            saw_core = true;
            break;
        case TOPO_LEVEL_MODULE:
        case TOPO_LEVEL_TILE:
            /* Between core and die. Nothing in this kernel schedules on a
             * module boundary yet, but the shift still has to be carried
             * forward or the die/package fields below come out wrong. */
            if (shift_width > core_shift) core_shift = shift_width;
            break;
        case TOPO_LEVEL_DIE:
            die_shift = shift_width;
            saw_die = true;
            break;
        default:
            break;
        }

        /* EDX carries the full x2APIC ID of the CPU executing CPUID. On a
         * machine in x2APIC mode this is more authoritative than anything
         * derived from leaf 1's 8-bit field. */
        if (sub == 0 && edx != 0) apic_id = edx;
    }

    if (!saw_smt && !saw_core) return false;

    if (!saw_core) core_shift = smt_shift;
    if (!saw_die)  die_shift  = core_shift;
    if (core_shift < smt_shift) core_shift = smt_shift;
    if (die_shift  < core_shift) die_shift = core_shift;

    t->apic_id    = apic_id;
    t->smt_id     = smt_shift ? (apic_id & ((1U << smt_shift) - 1)) : 0;
    t->core_id    = (apic_id >> smt_shift) &
                    ((core_shift > smt_shift) ? ((1U << (core_shift - smt_shift)) - 1) : 0);
    t->die_id     = (apic_id >> core_shift) &
                    ((die_shift > core_shift) ? ((1U << (die_shift - core_shift)) - 1) : 0);
    t->package_id = apic_id >> die_shift;
    t->llc_id     = t->package_id;   /* refined by topology_finalize() */
    t->valid      = true;

    if (threads_at_smt > 1) g_smt_active = true;
    return true;
}

/*
 * Legacy derivation for parts without leaf 0xB/0x1F — which includes QEMU's
 * default CPU models, so this is a path that actually runs rather than a
 * museum piece.
 *
 * Leaf 1 EBX[23:16] gives the maximum number of logical processors the package
 * can address; leaf 4 subleaf 0 EAX[31:26] (Intel) or leaf 0x80000008 ECX[7:0]
 * (AMD) gives the number of cores. The ratio is the SMT width, and the APIC ID
 * splits on the bit widths those counts imply.
 */
static u32 topo_next_pow2_shift(u32 n)
{
    u32 shift = 0;
    while ((1U << shift) < n) shift++;
    return shift;
}

static bool topo_decode_legacy(u32 apic_id, cpu_topology_t *t)
{
    u32 eax, ebx, ecx, edx;

    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    u32 logical_per_pkg = (ebx >> 16) & 0xFF;
    if (logical_per_pkg == 0) logical_per_pkg = 1;

    u32 cores_per_pkg = 1;
    if (g_cpu_info.max_leaf >= 4) {
        cpuid(4, 0, &eax, &ebx, &ecx, &edx);
        cores_per_pkg = ((eax >> 26) & 0x3F) + 1;
    } else if (g_cpu_info.max_ext_leaf >= 0x80000008) {
        cpuid(0x80000008, 0, &eax, &ebx, &ecx, &edx);
        cores_per_pkg = (ecx & 0xFF) + 1;
    }
    if (cores_per_pkg == 0) cores_per_pkg = 1;
    if (cores_per_pkg > logical_per_pkg) cores_per_pkg = logical_per_pkg;

    u32 threads_per_core = logical_per_pkg / cores_per_pkg;
    if (threads_per_core == 0) threads_per_core = 1;

    u32 smt_shift  = topo_next_pow2_shift(threads_per_core);
    u32 core_shift = smt_shift + topo_next_pow2_shift(cores_per_pkg);

    t->apic_id    = apic_id;
    t->smt_id     = smt_shift ? (apic_id & ((1U << smt_shift) - 1)) : 0;
    t->core_id    = (apic_id >> smt_shift) &
                    ((core_shift > smt_shift) ? ((1U << (core_shift - smt_shift)) - 1) : 0);
    t->die_id     = 0;
    t->package_id = apic_id >> core_shift;
    t->llc_id     = t->package_id;
    t->valid      = true;

    if (threads_per_core > 1) g_smt_active = true;
    return true;
}

/*
 * AMD families 17h and later carry a dedicated leaf that states the
 * core/thread split directly instead of leaving it to be derived. Where it is
 * present it is authoritative: the derived form gets CCX/CCD boundaries wrong
 * on multi-die parts, which is exactly the boundary that matters for deciding
 * whether a migration keeps the L3 warm.
 */
static bool topo_decode_amd_1e(u32 apic_id, cpu_topology_t *t)
{
    if (g_cpu_info.max_ext_leaf < 0x8000001E) return false;

    u32 eax, ebx, ecx, edx;
    cpuid(0x8000001E, 0, &eax, &ebx, &ecx, &edx);

    u32 ext_apic_id      = eax;
    u32 core_id          = ebx & 0xFF;
    u32 threads_per_core = ((ebx >> 8) & 0xFF) + 1;
    u32 node_id          = ecx & 0xFF;

    if (threads_per_core == 0) threads_per_core = 1;

    t->apic_id    = ext_apic_id ? ext_apic_id : apic_id;
    t->smt_id     = (threads_per_core > 1) ? (t->apic_id & (threads_per_core - 1)) : 0;
    t->core_id    = core_id;
    t->die_id     = node_id;
    t->package_id = node_id;
    t->llc_id     = node_id;
    t->valid      = true;

    if (threads_per_core > 1) g_smt_active = true;
    return true;
}

void topology_detect_self(u32 cpu_id)
{
    if (cpu_id >= SMP_MAX_CPUS) return;

    cpu_topology_t *t = &g_topo[cpu_id];
    __builtin_memset(t, 0, sizeof(*t));

    u32 eax, ebx, ecx, edx;
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    u32 apic_id = (ebx >> 24) & 0xFF;

    /* In x2APIC mode the 8-bit leaf-1 field truncates; the APIC's own ID
     * register is the full value. */
    if (lapic_x2apic_active()) apic_id = lapic_id();

    bool ok = false;
    if (g_cpu_info.max_leaf >= 0x1F) ok = topo_decode_extended(0x1F, apic_id, t);
    if (!ok && g_cpu_info.max_leaf >= 0xB) ok = topo_decode_extended(0xB, apic_id, t);
    if (!ok && cpu_is_amd())              ok = topo_decode_amd_1e(apic_id, t);
    if (!ok) ok = topo_decode_legacy(apic_id, t);

    if (!ok) {
        /* Nothing usable: treat the CPU as a package of its own. Every
         * consumer then simply sees no siblings, which is conservative in
         * every direction (no SMT mitigation elision, no SMT-aware packing). */
        t->apic_id    = apic_id;
        t->package_id = cpu_id;
        t->core_id    = 0;
        t->smt_id     = 0;
        t->llc_id     = cpu_id;
        t->valid      = true;
    }
}

void topology_finalize(u32 ncpus)
{
    if (ncpus > SMP_MAX_CPUS) ncpus = SMP_MAX_CPUS;
    if (ncpus == 0) ncpus = 1;

    /* Sibling masks. O(n^2) over at most SMP_MAX_CPUS entries, run exactly
     * once at boot — the alternative (sorting into buckets) buys nothing at
     * this scale and costs clarity. */
    for (u32 i = 0; i < ncpus; i++) {
        if (!g_topo[i].valid) continue;
        u64 smt = 0, core = 0;
        for (u32 j = 0; j < ncpus; j++) {
            if (!g_topo[j].valid) continue;
            if (g_topo[i].package_id == g_topo[j].package_id &&
                g_topo[i].die_id     == g_topo[j].die_id) {
                core |= (1ULL << j);
                if (g_topo[i].core_id == g_topo[j].core_id)
                    smt |= (1ULL << j);
            }
        }
        g_topo[i].smt_mask  = smt;
        g_topo[i].core_mask = core;
    }

    /* Count distinct packages and distinct physical cores from what is
     * actually online. CPUID's cores_per_package reports what the silicon can
     * hold, which on a VM (or a part with cores fused off) is not what is
     * there. */
    u32 packages = 0, cores = 0;
    for (u32 i = 0; i < ncpus; i++) {
        if (!g_topo[i].valid) continue;

        bool new_pkg = true, new_core = true;
        for (u32 j = 0; j < i; j++) {
            if (!g_topo[j].valid) continue;
            if (g_topo[j].package_id == g_topo[i].package_id) {
                new_pkg = false;
                if (g_topo[j].die_id  == g_topo[i].die_id &&
                    g_topo[j].core_id == g_topo[i].core_id)
                    new_core = false;
            }
        }
        if (new_pkg)  packages++;
        if (new_core) cores++;
    }
    g_nr_packages = packages ? packages : 1;
    g_nr_cores    = cores ? cores : 1;

    /* SMT is "active" only if some core really has two online logical CPUs —
     * not merely if the part is capable of it. A machine booted with SMT
     * disabled in firmware still enumerates threads_per_core == 2 on some
     * parts, and paying for the MDS flush there would be pure loss. */
    g_smt_active = false;
    for (u32 i = 0; i < ncpus; i++) {
        if (g_topo[i].valid && g_topo[i].smt_mask &&
            (g_topo[i].smt_mask & ~(1ULL << i)) != 0) {
            g_smt_active = true;
            break;
        }
    }
}

const cpu_topology_t *topology_of(u32 cpu_id)
{
    if (cpu_id >= SMP_MAX_CPUS || !g_topo[cpu_id].valid) return NULL;
    return &g_topo[cpu_id];
}

u64 topology_smt_siblings(u32 cpu_id)
{
    if (cpu_id >= SMP_MAX_CPUS || !g_topo[cpu_id].valid) return 0;
    return g_topo[cpu_id].smt_mask;
}

u64 topology_core_siblings(u32 cpu_id)
{
    if (cpu_id >= SMP_MAX_CPUS || !g_topo[cpu_id].valid) return 0;
    return g_topo[cpu_id].core_mask;
}

bool topology_same_core(u32 a, u32 b)
{
    if (a >= SMP_MAX_CPUS || b >= SMP_MAX_CPUS) return false;
    if (!g_topo[a].valid || !g_topo[b].valid) return false;
    return g_topo[a].package_id == g_topo[b].package_id &&
           g_topo[a].die_id     == g_topo[b].die_id &&
           g_topo[a].core_id    == g_topo[b].core_id;
}

bool topology_same_llc(u32 a, u32 b)
{
    if (a >= SMP_MAX_CPUS || b >= SMP_MAX_CPUS) return false;
    if (!g_topo[a].valid || !g_topo[b].valid) return false;
    return g_topo[a].llc_id == g_topo[b].llc_id;
}

u32 topology_nr_packages(void) { return g_nr_packages; }
u32 topology_nr_cores(void)    { return g_nr_cores; }
bool topology_smt_active(void) { return g_smt_active; }

void topology_dump(u32 ncpus)
{
    if (ncpus > SMP_MAX_CPUS) ncpus = SMP_MAX_CPUS;

    kprintf("[TOPO] %u package(s), %u physical core(s), %u logical CPU(s)%s\n",
            g_nr_packages, g_nr_cores, ncpus,
            g_smt_active ? ", SMT active" : "");

    for (u32 i = 0; i < ncpus; i++) {
        if (!g_topo[i].valid) continue;
        kprintf("[TOPO]   cpu%u: apic=%u pkg=%u die=%u core=%u smt=%u "
                "siblings=0x%llx\n",
                i, g_topo[i].apic_id, g_topo[i].package_id, g_topo[i].die_id,
                g_topo[i].core_id, g_topo[i].smt_id,
                (unsigned long long)g_topo[i].smt_mask);
    }
}
