/* ============================================================================
 * AzamiOS — hypervisor detection and paravirtual clock information
 * File: arch/x86_64/cpu/hypervisor.c
 *
 * See hypervisor.h. References: Linux arch/x86/kernel/cpu/hypervisor.c,
 * kvmclock.c, pvclock.c and vmware.c; the KVM paravirt ABI in
 * Documentation/virt/kvm/x86/{cpuid,msr}.rst.
 * ============================================================================ */

#include "hypervisor.h"
#include "cpu.h"
#include "msr.h"
#include "../../../include/azami/defs.h"
#include "../../../drivers/char/console.h"
#include "../../../kernel/mm/pmm.h"
#include "../../../kernel/lib/string.h"
#include "../mm/vmm.h"

/* ── KVM paravirtual interface ────────────────────────────────────────────── */
#define KVM_CPUID_FEATURES              0x00000001   /* offset from the base  */
#define KVM_FEATURE_CLOCKSOURCE         (1U << 0)    /* MSR 0x11              */
#define KVM_FEATURE_CLOCKSOURCE2        (1U << 3)    /* MSR 0x4b564d01        */
#define KVM_FEATURE_CLOCKSOURCE_STABLE  (1U << 24)   /* flags bit is honoured */

#define MSR_KVM_SYSTEM_TIME             0x00000012U
#define MSR_KVM_SYSTEM_TIME_NEW         0x4b564d01U

#define PVCLOCK_TSC_STABLE_BIT          (1U << 0)

/* The per-vCPU time record the host keeps current (pvclock ABI). */
struct pvclock_vcpu_time_info {
    u32 version;
    u32 pad0;
    u64 tsc_timestamp;
    u64 system_time;
    u32 tsc_to_system_mul;
    s8  tsc_shift;
    u8  flags;
    u8  pad[2];
} __attribute__((packed));

BUILD_ASSERT(sizeof(struct pvclock_vcpu_time_info) == 32, "pvclock ABI is 32 bytes");

static hv_type_t g_hv_type;
static u32       g_hv_base;          /* CPUID base leaf of the signature found */
static bool      g_hv_tsc_reliable;
static u32       g_hv_tsc_khz;
static bool      g_hv_detected;

static const struct { const char sig[13]; hv_type_t type; const char *name; } g_signatures[] = {
    { "KVMKVMKVM\0\0\0",  HV_KVM,    "KVM"        },
    { "VMwareVMware",     HV_VMWARE, "VMware"     },
    { "Microsoft Hv",     HV_HYPERV, "Hyper-V"    },
    { "XenVMMXenVMM",     HV_XEN,    "Xen"        },
    { "VBoxVBoxVBox",     HV_VBOX,   "VirtualBox" },
    { "TCGTCGTCGTCG",     HV_TCG,    "QEMU TCG"   },
};

static const char *type_name(hv_type_t t)
{
    if (t == HV_NONE) return "none";
    for (size_t i = 0; i < sizeof(g_signatures) / sizeof(g_signatures[0]); i++)
        if (g_signatures[i].type == t) return g_signatures[i].name;
    return "unknown";
}

/* Hypervisors that offer several interfaces stack them 0x100 apart: QEMU/KVM
 * with Hyper-V enlightenments enabled puts Hyper-V at 0x40000000 and KVM at
 * 0x40000100. Prefer KVM wherever it appears, since its clock interface is
 * the one this file understands; otherwise take the first signature. */
static void scan_signatures(void)
{
    hv_type_t first = HV_NONE;
    u32 first_base = 0;

    for (u32 base = 0x40000000U; base < 0x40010000U; base += 0x100) {
        u32 eax, ebx, ecx, edx;
        cpuid(base, 0, &eax, &ebx, &ecx, &edx);
        char sig[13];
        memcpy(sig + 0, &ebx, 4);
        memcpy(sig + 4, &ecx, 4);
        memcpy(sig + 8, &edx, 4);
        sig[12] = '\0';

        for (size_t i = 0; i < sizeof(g_signatures) / sizeof(g_signatures[0]); i++) {
            if (memcmp(sig, g_signatures[i].sig, 12) != 0) continue;
            if (g_signatures[i].type == HV_KVM) {
                g_hv_type = HV_KVM;
                g_hv_base = base;
                return;
            }
            if (first == HV_NONE) { first = g_signatures[i].type; first_base = base; }
        }
    }
    g_hv_type = first != HV_NONE ? first : HV_OTHER;
    g_hv_base = first_base;
}

/* pvclock's scale is ns = (tsc_delta << shift) * mul >> 32, so the TSC rate
 * is its inverse: (10^6 << 32) / mul kHz, shifted back the other way. */
static u32 pvclock_tsc_khz(const volatile struct pvclock_vcpu_time_info *pv)
{
    u32 mul = pv->tsc_to_system_mul;
    s8  shift = pv->tsc_shift;
    if (mul == 0) return 0;
    u64 khz = (1000000ULL << 32) / mul;
    if (shift < 0) khz <<= -shift;
    else           khz >>= shift;
    return (u32)khz;
}

static void kvm_clock_probe(void)
{
    u32 max_leaf, ebx, ecx, edx, features;
    cpuid(g_hv_base, 0, &max_leaf, &ebx, &ecx, &edx);
    cpuid(g_hv_base + KVM_CPUID_FEATURES, 0, &features, &ebx, &ecx, &edx);

    u32 msr;
    if (features & KVM_FEATURE_CLOCKSOURCE2)     msr = MSR_KVM_SYSTEM_TIME_NEW;
    else if (features & KVM_FEATURE_CLOCKSOURCE) msr = MSR_KVM_SYSTEM_TIME;
    else return;

    /* One record for the BSP is all this needs: the flags are a property of
     * the host's TSC, the same on every vCPU. The page stays registered and
     * allocated for the life of the system (the host writes to it on every
     * vCPU entry), so it is never returned to the allocator. */
    phys_addr_t page = pmm_alloc_page_zeroed();
    if (!page) return;
    volatile struct pvclock_vcpu_time_info *pv =
        (volatile struct pvclock_vcpu_time_info *)PHYS_TO_VIRT(page);
    wrmsr(msr, (u64)page | 1);

    /* The host fills the record synchronously on the MSR write's VM exit;
     * read it under its version protocol (odd = update in progress). */
    u32 ver, mul_khz = 0;
    u8 flags = 0;
    for (int tries = 0; tries < 1000; tries++) {
        ver = pv->version;
        __asm__ volatile("" ::: "memory");
        if (ver & 1) continue;
        flags   = pv->flags;
        mul_khz = pvclock_tsc_khz(pv);
        __asm__ volatile("" ::: "memory");
        if (pv->version == ver && ver != 0) break;
    }

    /* Leaf base+0x10, when present, carries the TSC frequency directly (QEMU
     * fills it in when the TSC frequency is fixed). */
    if (max_leaf >= g_hv_base + 0x10) {
        u32 khz, x1, x2, x3;
        cpuid(g_hv_base + 0x10, 0, &khz, &x1, &x2, &x3);
        if (khz) mul_khz = khz;
    }
    g_hv_tsc_khz = mul_khz;

    if ((features & KVM_FEATURE_CLOCKSOURCE_STABLE) && (flags & PVCLOCK_TSC_STABLE_BIT))
        g_hv_tsc_reliable = true;

    kprintf("[HV] KVM clock: MSR 0x%x, TSC %u.%03u MHz, host TSC %s\n",
            msr, g_hv_tsc_khz / 1000, g_hv_tsc_khz % 1000,
            g_hv_tsc_reliable ? "stable across vCPUs" : "not guaranteed stable");
}

static void vmware_clock_probe(void)
{
    u32 max_leaf, ebx, ecx, edx;
    cpuid(g_hv_base, 0, &max_leaf, &ebx, &ecx, &edx);
    if (max_leaf >= g_hv_base + 0x10) {
        u32 khz;
        cpuid(g_hv_base + 0x10, 0, &khz, &ebx, &ecx, &edx);
        g_hv_tsc_khz = khz;
    }
    /* VMware virtualises a constant, synchronised TSC on every host it
     * supports; Linux sets X86_FEATURE_TSC_RELIABLE for it unconditionally. */
    g_hv_tsc_reliable = true;
}

void hypervisor_detect(void)
{
    if (g_hv_detected) return;
    g_hv_detected = true;

    if (!(g_cpu_info.features & CPU_FEAT_HYPERVISOR)) {
        g_hv_type = HV_NONE;
        return;
    }
    scan_signatures();

    switch (g_hv_type) {
    case HV_KVM:    kvm_clock_probe();    break;
    case HV_VMWARE: vmware_clock_probe(); break;
    default: break;
    }
    kprintf("[HV] Running under %s%s\n", type_name(g_hv_type),
            g_hv_tsc_reliable ? " (TSC reliable)" : "");
}

hv_type_t   hypervisor_type(void)         { return g_hv_type; }
const char *hypervisor_name(void)         { return type_name(g_hv_type); }
bool        hypervisor_tsc_reliable(void) { return g_hv_tsc_reliable; }
u32         hypervisor_tsc_khz(void)      { return g_hv_tsc_khz; }
