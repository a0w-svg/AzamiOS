/* ============================================================================
 * AzamiOS — Speculative-Execution & Side-Channel Mitigation Policy (x86_64)
 * File: arch/x86_64/cpu/mitigations.h
 *
 * cpu.c enumerates what the part can do; this module decides what the kernel
 * actually turns on, and is the only place that writes IA32_SPEC_CTRL,
 * IA32_PRED_CMD, IA32_TSX_CTRL or the AMD equivalents.
 *
 * Two rules shape every decision here:
 *
 *   1. Never pay for a mitigation the CPU says is unnecessary.
 *      IA32_ARCH_CAPABILITIES exists precisely so an OS can tell a fixed part
 *      from an affected one. A kernel that unconditionally enables IBRS on
 *      hardware advertising RDCL_NO/MDS_NO is spending real cycles on nothing.
 *
 *   2. Never enable one it cannot afford to leave on.
 *      Everything programmed here is either set-once (enhanced IBRS, AutoIBRS,
 *      SSBD, TSX disable) or a single instruction on an existing boundary (the
 *      VERW on the user-return path, IBPB on an address-space switch). The
 *      original retpoline-era mitigations that need a per-transition MSR write
 *      are deliberately not implemented — half-applying them would cost the
 *      cycles without closing the hole.
 *
 * What this module does NOT do, and why:
 *   - No KPTI. The Meltdown mitigation is a page-table split, not an MSR; it
 *     belongs in the VMM and is reported honestly as "Vulnerable" until it
 *     exists. Saying "Mitigation: PTI" here would be a lie in a header file.
 *   - No retpoline. That is a compiler flag (-mindirect-branch=thunk-extern)
 *     plus a thunk, not a runtime decision.
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"

/* Which class of Spectre-v2 protection ended up active. */
#define SPECTRE_V2_NONE       0   /* nothing available on this part          */
#define SPECTRE_V2_EIBRS      1   /* enhanced IBRS: IA32_SPEC_CTRL.IBRS once */
#define SPECTRE_V2_AUTOIBRS   2   /* AMD AutoIBRS: EFER bit, no MSR traffic  */
#define SPECTRE_V2_IBPB       3   /* barrier on every address-space switch   */

/**
 * mitigations_init_bsp() — choose the policy and apply it to the boot CPU.
 * Call once, from cpu_enable_features_bsp(), after CPUID detection.
 */
void mitigations_init_bsp(void);

/**
 * mitigations_init_ap() — replay the BSP's policy on an application processor.
 *
 * IA32_SPEC_CTRL, the EFER AutoIBRS bit and IA32_TSX_CTRL are all
 * per-logical-processor. A core that missed this would run the same threads
 * with different speculation behaviour from every other core — the kind of
 * difference that shows up as an intermittent side channel and nothing else.
 */
void mitigations_init_ap(void);

/**
 * mitigations_switch_mm(prev_pml4, next_pml4) — barrier on an address-space
 * change.
 *
 * Issues IBPB when the policy calls for it, so indirect-branch predictor
 * entries trained by the outgoing process cannot steer the incoming one. Only
 * armed on parts with no enhanced IBRS: where eIBRS is active the hardware
 * already isolates predictions and the barrier would be pure cost. Cheap to
 * call unconditionally — it returns immediately when disarmed, and when the
 * two address spaces are the same.
 */
void mitigations_switch_mm(u64 prev_pml4, u64 next_pml4);

/**
 * mitigations_flush_l1d() — write back and invalidate L1D via IA32_FLUSH_CMD.
 * Exposed for completeness (it is what an L1TF-affected hypervisor would call
 * before entering a guest); the kernel itself has no path that needs it.
 */
void mitigations_flush_l1d(void);

/** Format one "name: status" line per vulnerability, Linux's wording. */
size_t mitigations_format(char *buf, size_t max);

/** Single-line summary for /proc/cpuinfo. */
size_t mitigations_format_short(char *buf, size_t max);

/* Set when the user-return paths must run VERW to overwrite the store, fill
 * and load-port buffers before ring 3 can sample them (MDS / TAA / MMIO stale
 * data / RFDS). Read directly by isr.asm and syscall_entry.asm — a call would
 * cost more than the instruction it guards. */
extern u8  g_verw_user_clear;
extern u16 g_verw_sel;

extern u8  g_spectre_v2_mode;   /* SPECTRE_V2_* */
extern u8  g_ibpb_on_switch;
extern u8  g_ssbd_enabled;
extern u8  g_tsx_disabled;
