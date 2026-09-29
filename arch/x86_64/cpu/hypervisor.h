/* ============================================================================
 * AzamiOS — hypervisor detection and paravirtual clock information
 * File: arch/x86_64/cpu/hypervisor.h
 *
 * Which hypervisor (if any) the kernel runs under, found the way Linux's
 * init_hypervisor_platform() finds it: the CPUID "hypervisor present" bit,
 * then a vendor signature in the 0x40000000 leaf range. What the kernel uses
 * it for today is the clock:
 *
 *   KVM      kvmclock (pvclock) says whether the host keeps the TSC stable
 *            and synchronised across vCPUs (PVCLOCK_TSC_STABLE_BIT), and
 *            encodes the exact guest TSC frequency.
 *   VMware   leaf 0x40000010 publishes the TSC frequency, and VMware
 *            guarantees a constant, synchronised TSC (Linux marks it
 *            TSC_RELIABLE unconditionally there).
 *
 * KVM hides CPUID's invariant-TSC bit by default because it blocks live
 * migration, so without this a KVM guest could not tell a perfectly good TSC
 * from a broken one and fell back to the HPET — an MMIO register QEMU
 * emulates in userspace, which made every clock_gettime() a ~6 µs VM exit
 * instead of a ~20 ns RDTSC.
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"

typedef enum {
    HV_NONE = 0,
    HV_KVM,
    HV_VMWARE,
    HV_HYPERV,
    HV_XEN,
    HV_VBOX,
    HV_TCG,        /* QEMU without acceleration */
    HV_OTHER,      /* hypervisor bit set, unknown or no signature */
} hv_type_t;

/** hypervisor_detect() — identify the hypervisor and read its clock info.
 * BSP only, once, after cpu_detect() and pmm_init(). Idempotent. */
void hypervisor_detect(void);

hv_type_t   hypervisor_type(void);
const char *hypervisor_name(void);

/** hypervisor_tsc_reliable() — the hypervisor guarantees a constant-rate
 * TSC that is synchronised across vCPUs, so it is safe as the clocksource
 * even without CPUID's invariant-TSC bit. */
bool hypervisor_tsc_reliable(void);

/** hypervisor_tsc_khz() — TSC frequency as reported by the hypervisor, in
 * kHz; 0 when it did not say. Exact, unlike a calibration against the HPET. */
u32 hypervisor_tsc_khz(void);
