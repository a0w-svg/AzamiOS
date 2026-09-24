/* ============================================================================
 * AzamiOS — CPU Topology Enumeration
 * File: arch/x86_64/cpu/topology.h
 *
 * Who is a sibling of whom, and how closely.
 *
 * A logical CPU's APIC ID is not an opaque number: it is a packed set of
 * bit-fields naming the SMT thread, the core, the die and the package the CPU
 * lives in. Decoding it is what turns "four CPUs" into "two cores with two
 * hyperthreads each", and that distinction drives real decisions:
 *
 *   - The scheduler should fill distinct physical cores before it puts a
 *     second thread on a core that already has one, because SMT siblings
 *     share execution resources and two threads on one core are worth much
 *     less than two threads on two cores. sched_topo_pick_idle_cpu() encodes
 *     exactly that preference.
 *   - Load balancing should prefer to move a thread between CPUs that share
 *     an L2/L3 before moving it across packages, because the cache it has
 *     warmed comes with it in the first case and not in the second.
 *   - The MDS/L1TF class of mitigations only need their expensive flush when
 *     the core actually has a sibling thread that could observe the leaked
 *     state (see arch/x86_64/cpu/mitigations.c).
 *   - /proc/cpuinfo and /sys/devices/system/cpu/cpuN/topology/ report it, and
 *     userspace (lscpu, taskset, OpenMP runtimes, hwloc) reads it there.
 *
 * Enumeration follows the modern leaves first — 0x1F (V2 extended topology,
 * which adds die and module levels) then 0xB (extended topology) — and falls
 * back to the legacy leaf 1 / leaf 4 / AMD leaf 0x8000001E derivation on parts
 * that do not implement them. The fallback is not academic: it is the path
 * QEMU's default CPU models take.
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"

/** struct cpu_topology — decoded identity of one logical CPU. */
typedef struct cpu_topology {
    u32 apic_id;          /* full APIC ID this was decoded from            */
    u32 smt_id;           /* thread index within its core                  */
    u32 core_id;          /* core index within its package                 */
    u32 die_id;           /* die index within its package (0 if no leaf 1F)*/
    u32 package_id;       /* physical package (socket)                     */
    u32 llc_id;           /* last-level-cache domain, package-scoped       */
    u64 smt_mask;         /* logical CPUs sharing this physical core       */
    u64 core_mask;        /* logical CPUs sharing this LLC / package       */
    bool valid;           /* decoding succeeded for this CPU               */
} cpu_topology_t;

/**
 * topology_detect_self(cpu_id) — decode the calling CPU's topology and record
 * it under logical id @cpu_id. Each CPU calls this for itself, because CPUID
 * leaf 0xB/0x1F only ever describes the CPU executing it.
 */
void topology_detect_self(u32 cpu_id);

/**
 * topology_finalize(ncpus) — once every CPU has registered, build the sibling
 * masks and renumber LLC domains. Called on the BSP after all APs are online.
 */
void topology_finalize(u32 ncpus);

/** topology_of(cpu_id) — decoded topology for a logical CPU, or NULL. */
const cpu_topology_t *topology_of(u32 cpu_id);

/** topology_smt_siblings(cpu_id) — mask of logical CPUs on the same physical
 *  core, including @cpu_id itself. 0 if topology is unknown. */
u64 topology_smt_siblings(u32 cpu_id);

/** topology_core_siblings(cpu_id) — mask of logical CPUs sharing the same
 *  last-level cache (in practice: the same package). */
u64 topology_core_siblings(u32 cpu_id);

/** topology_same_core(a, b) — true when two logical CPUs are SMT siblings. */
bool topology_same_core(u32 a, u32 b);

/** topology_same_llc(a, b) — true when two logical CPUs share a last-level
 *  cache, i.e. migrating between them keeps the working set warm. */
bool topology_same_llc(u32 a, u32 b);

/** topology_nr_packages() / topology_nr_cores() — totals across the machine,
 *  counted from what was actually enumerated rather than from CPUID's
 *  per-package maxima (which report the silicon's capacity, not how much of
 *  it is present and enabled). */
u32 topology_nr_packages(void);
u32 topology_nr_cores(void);

/** topology_smt_active() — true when at least one physical core has more than
 *  one logical CPU online. What the MDS/L1TF mitigations gate on. */
bool topology_smt_active(void);

/** topology_dump() — one line per CPU on the console. */
void topology_dump(u32 ncpus);
