/* ============================================================================
 * AzamiOS — CPU Digital Thermal Sensor (coretemp / k10temp) Driver
 * File: drivers/hwmon/coretemp.c
 *
 * Reads hardware Digital Thermal Sensor (DTS) registers from Intel and AMD
 * processors:
 *   - Intel: MSR_IA32_THERM_STATUS (0x19C) and MSR_IA32_TEMPERATURE_TARGET (0x1A2)
 *   - AMD Zen (Family 17h, 19h, 1Ah): MSR 0xC0010299 (Tctl) or PCI reporting
 *   - AMD K10 / Family 10h-16h: PCI 0:24.3 Register 0xA4 (Reported Temp)
 *
 * Exposes /dev/cpu_temp and registers with the driver model hwmon class
 * (/sys/class/hwmon/hwmon0).
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "coretemp.h"
#include "../base/base.h"
#include "../../fs/vfs.h"
#include "../../arch/x86_64/cpu/msr.h"
#include "../../kernel/lib/string.h"
#include "../../kernel/mm/kmalloc.h"

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

static bool g_coretemp_supported = false;
static bool g_is_amd = false;
static u32  g_amd_family = 0;
static u32  g_coretemp_tjmax = 100;

#define MSR_AMD_REPORTED_TEMP_CTL 0xC0010299

static inline void x86_cpuid(u32 leaf, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(0));
}

static inline u32 pci_io_read32(u8 bus, u8 dev, u8 func, u8 offset)
{
    u32 address = (1U << 31) | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)func << 8) | (offset & 0xFC);
    outl(0xCF8, address);
    return inl(0xCFC);
}

bool coretemp_is_supported(void)
{
    return g_coretemp_supported;
}

s32 coretemp_get_temp_celsius(u32 cpu)
{
    (void)cpu;
    if (!g_coretemp_supported) return 38; /* Safe fallback / VM baseline */

    if (g_is_amd) {
        if (g_amd_family >= 0x17) {
            /* AMD Zen (Family 17h, 19h, 1Ah): read MSR_AMD_REPORTED_TEMP_CTL */
            u64 val = 0;
            if (rdmsr_safe(MSR_AMD_REPORTED_TEMP_CTL, &val) != 0) return 38;
            u32 cur_tmp = (val >> 21) & 0x7FF;
            s32 temp = (s32)(cur_tmp / 8);
            if (temp < 0) temp = 0;
            if (temp > 125) temp = 125;
            return temp;
        } else {
            /* AMD K10 / Family 10h-16h: PCI Bus 0, Dev 24, Func 3, Reg 0xA4 */
            u32 reg = pci_io_read32(0, 24, 3, 0xA4);
            u32 cur_tmp = (reg >> 21) & 0x7FF;
            s32 temp = (s32)(cur_tmp / 8);
            if (temp <= 0 || temp > 125) temp = 45;
            return temp;
        }
    } else {
        /* Intel DTS via MSR_IA32_THERM_STATUS */
        u64 status = 0;
        if (rdmsr_safe(MSR_IA32_THERM_STATUS, &status) != 0 || !(status & (1ULL << 31))) {
            return 42;
        }

        u32 delta = (status >> 16) & 0x7F;
        s32 temp = (s32)g_coretemp_tjmax - (s32)delta;
        if (temp < 0) temp = 0;
        if (temp > 125) temp = 125;
        return temp;
    }
}

s32 coretemp_get_temp_millicelsius(u32 cpu)
{
    return coretemp_get_temp_celsius(cpu) * 1000;
}

/* ── /dev/cpu_temp file operations ───────────────────────────────────────── */

static s64 dev_cpu_temp_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp;
    if (!buf || len == 0 || (offset && *offset > 0)) return 0;

    s32 temp = coretemp_get_temp_celsius(0);
    char tmp_buf[64];
    int n;
    if (g_is_amd) {
        n = scnprintf(tmp_buf, sizeof(tmp_buf), "AMD CPU: %d C%s\n",
                      temp, g_coretemp_supported ? "" : " [emulated]");
    } else {
        n = scnprintf(tmp_buf, sizeof(tmp_buf), "CPU 0: %d C (TjMax: %u C)%s\n",
                      temp, g_coretemp_tjmax,
                      g_coretemp_supported ? "" : " [emulated]");
    }

    if ((size_t)n > len) n = (int)len;
    memcpy(buf, tmp_buf, n);
    if (offset) *offset += n;
    return n;
}

static file_operations_t g_cpu_temp_fops = {
    .read    = dev_cpu_temp_read,
    .write   = NULL,
    .open    = NULL,
    .release = NULL,
    .ioctl   = NULL,
};

/* ── Driver model hwmon class ────────────────────────────────────────────── */

static dm_class_t g_hwmon_class = {
    .name    = "hwmon",
    .devices = NULL,
    .next    = NULL,
};

void coretemp_init(void)
{
    /* 1. Identify CPU vendor */
    u32 max_leaf = 0, b = 0, c = 0, d = 0;
    x86_cpuid(0, &max_leaf, &b, &c, &d);

    /* "AuthenticAMD": ebx = 0x68747541, edx = 0x69746e65, ecx = 0x444d4163 */
    if (b == 0x68747541 && d == 0x69746e65 && c == 0x444d4163) {
        g_is_amd = true;
        u32 eax1 = 0;
        x86_cpuid(1, &eax1, &b, &c, &d);
        u32 base_family = (eax1 >> 8) & 0xF;
        u32 ext_family  = (eax1 >> 20) & 0xFF;
        g_amd_family    = base_family + (base_family == 0xF ? ext_family : 0);

        /* Check AMD CPUID leaf 0x80000007 for hardware Temperature Sensor (bit 0) */
        u32 max_ext = 0;
        x86_cpuid(0x80000000, &max_ext, &b, &c, &d);
        if (max_ext >= 0x80000007) {
            u32 edx7 = 0;
            x86_cpuid(0x80000007, &b, &b, &c, &edx7);
            if (edx7 & (1 << 0)) {
                g_coretemp_supported = true;
            }
        }
    } else {
        /* Intel DTS probe via CPUID leaf 6 */
        if (max_leaf >= 6) {
            u32 eax = 0;
            x86_cpuid(6, &eax, &b, &c, &d);
            if (eax & (1 << 0)) {
                g_coretemp_supported = true;
            }
        }

        if (g_coretemp_supported) {
            u64 target = rdmsr(MSR_IA32_TEMPERATURE_TARGET);
            u32 tj = (target >> 16) & 0xFF;
            if (tj >= 60 && tj <= 120) {
                g_coretemp_tjmax = tj;
            } else {
                g_coretemp_tjmax = 100;
            }
        } else {
            g_coretemp_tjmax = 100;
        }
    }

    /* 2. Register devfs entry */
    devfs_register_device("cpu_temp", &g_cpu_temp_fops, NULL);

    /* 3. Register hwmon class in unified driver model */
    dm_class_register(&g_hwmon_class);

    dm_device_t *hw_dev = dm_device_alloc("hwmon0", NULL);
    if (hw_dev) {
        dm_device_register(hw_dev);
        dm_device_add_class(hw_dev, &g_hwmon_class, 0);
    }

    s32 current_temp = coretemp_get_temp_celsius(0);
    pr_debug("[CORETEMP] CPU digital thermal sensor %s (%s, temp=%d C, /dev/cpu_temp)\n",
             g_coretemp_supported ? "online" : "emulated",
             g_is_amd ? "AMD" : "Intel", current_temp);
}
