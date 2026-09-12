/* ============================================================================
 * AzamiOS — CPU Digital Thermal Sensor (coretemp) Driver
 * File: drivers/hwmon/coretemp.c
 *
 * Reads hardware Digital Thermal Sensor (DTS) registers from Intel/AMD CPUs via
 * MSR_IA32_THERM_STATUS and MSR_IA32_TEMPERATURE_TARGET.
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
static u32  g_coretemp_tjmax = 100;

static inline void x86_cpuid(u32 leaf, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(0));
}

bool coretemp_is_supported(void)
{
    return g_coretemp_supported;
}

s32 coretemp_get_temp_celsius(u32 cpu)
{
    (void)cpu;
    if (!g_coretemp_supported) return 38; /* Safe fallback / VM baseline */

    u64 status = rdmsr(MSR_IA32_THERM_STATUS);
    if (!(status & (1ULL << 31))) {
        /* Reading not valid, return fallback */
        return 42;
    }

    u32 delta = (status >> 16) & 0x7F;
    s32 temp = (s32)g_coretemp_tjmax - (s32)delta;
    if (temp < 0) temp = 0;
    if (temp > 125) temp = 125;
    return temp;
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
    int n = scnprintf(tmp_buf, sizeof(tmp_buf), "CPU 0: %d C (TjMax: %u C)%s\n",
                      temp, g_coretemp_tjmax,
                      g_coretemp_supported ? "" : " [emulated]");

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
    /* 1. Probe CPUID leaf 6 for DTS support */
    u32 max_leaf = 0, b = 0, c = 0, d = 0;
    x86_cpuid(0, &max_leaf, &b, &c, &d);

    if (max_leaf >= 6) {
        u32 eax = 0;
        x86_cpuid(6, &eax, &b, &c, &d);
        if (eax & (1 << 0)) {
            g_coretemp_supported = true;
        }
    }

    /* 2. Probe TjMax */
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

    /* 3. Register devfs entry */
    devfs_register_device("cpu_temp", &g_cpu_temp_fops, NULL);

    /* 4. Register hwmon class in unified driver model */
    dm_class_register(&g_hwmon_class);

    dm_device_t *hw_dev = dm_device_alloc("hwmon0", NULL);
    if (hw_dev) {
        dm_device_register(hw_dev);
        dm_device_add_class(hw_dev, &g_hwmon_class, 0);
    }

    s32 current_temp = coretemp_get_temp_celsius(0);
    pr_debug("[CORETEMP] CPU digital thermal sensor %s (TjMax=%u C, Core0=%d C, /dev/cpu_temp)\n",
             g_coretemp_supported ? "online" : "emulated",
             g_coretemp_tjmax, current_temp);
}
