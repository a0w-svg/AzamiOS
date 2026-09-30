/* ============================================================================
 * AzamiOS — ACPI Power Supply (Battery & AC Adapter) Driver
 * File: drivers/acpi/battery.c
 *
 * Implements the Linux-compatible /sys/class/power_supply subsystem, providing
 * battery status and AC adapter state for mobile PCs and laptops.
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "battery.h"
#include "acpi.h"
#include "../base/base.h"
#include "../../fs/vfs.h"
#include "../../kernel/lib/string.h"
#include "../../kernel/mm/kmalloc.h"

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

static dm_class_t g_power_supply_class = {
    .name    = "power_supply",
    .devices = NULL,
    .next    = NULL,
};

static battery_info_t    g_battery;
static ac_adapter_info_t g_ac_adapter;

/* ── Status Readers ───────────────────────────────────────────────────────── */

int battery_get_info(battery_info_t *info)
{
    if (!info) return -EINVAL;
    memcpy(info, &g_battery, sizeof(battery_info_t));
    return 0;
}

bool ac_adapter_is_online(void)
{
    return g_ac_adapter.online;
}

static const char *battery_status_str(battery_status_t st)
{
    switch (st) {
    case BATTERY_STATUS_CHARGING:     return "Charging";
    case BATTERY_STATUS_DISCHARGING:  return "Discharging";
    case BATTERY_STATUS_NOT_CHARGING: return "Not charging";
    case BATTERY_STATUS_FULL:         return "Full";
    case BATTERY_STATUS_UNKNOWN:
    default:                          return "Unknown";
    }
}

/* ── /dev/battery file operations ─────────────────────────────────────────── */

static s64 dev_battery_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp;
    if (!buf || len == 0 || (offset && *offset > 0)) return 0;

    char tmp[256];
    int n = scnprintf(tmp, sizeof(tmp),
                      "POWER_SUPPLY_NAME=BAT0\n"
                      "POWER_SUPPLY_TYPE=Battery\n"
                      "POWER_SUPPLY_STATUS=%s\n"
                      "POWER_SUPPLY_PRESENT=%d\n"
                      "POWER_SUPPLY_TECHNOLOGY=%s\n"
                      "POWER_SUPPLY_VOLTAGE_NOW=%u\n"
                      "POWER_SUPPLY_CURRENT_NOW=%u\n"
                      "POWER_SUPPLY_CAPACITY=%u\n"
                      "POWER_SUPPLY_MODEL_NAME=%s\n",
                      battery_status_str(g_battery.status),
                      g_battery.present ? 1 : 0,
                      g_battery.technology,
                      g_battery.voltage_now_uv,
                      g_battery.current_now_ua,
                      g_battery.capacity_percent,
                      g_battery.model);

    if ((size_t)n > len) n = (int)len;
    memcpy(buf, tmp, n);
    if (offset) *offset += n;
    return n;
}

static file_operations_t g_battery_fops = {
    .read    = dev_battery_read,
    .write   = NULL,
    .open    = NULL,
    .release = NULL,
    .ioctl   = NULL,
};

/* ── /dev/ac_adapter file operations ──────────────────────────────────────── */

static s64 dev_ac_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp;
    if (!buf || len == 0 || (offset && *offset > 0)) return 0;

    char tmp[128];
    int n = scnprintf(tmp, sizeof(tmp),
                      "POWER_SUPPLY_NAME=AC\n"
                      "POWER_SUPPLY_TYPE=Mains\n"
                      "POWER_SUPPLY_ONLINE=%d\n",
                      g_ac_adapter.online ? 1 : 0);

    if ((size_t)n > len) n = (int)len;
    memcpy(buf, tmp, n);
    if (offset) *offset += n;
    return n;
}

static file_operations_t g_ac_fops = {
    .read    = dev_ac_read,
    .write   = NULL,
    .open    = NULL,
    .release = NULL,
    .ioctl   = NULL,
};

/* ── Initialization ───────────────────────────────────────────────────────── */

void battery_init(void)
{
    /* Initialize default state */
    g_ac_adapter.online = true;

    memset(&g_battery, 0, sizeof(g_battery));
    g_battery.present             = true;
    g_battery.status              = BATTERY_STATUS_CHARGING;
    g_battery.capacity_percent    = 95;
    g_battery.voltage_now_uv      = 11400000; /* 11.4V */
    g_battery.current_now_ua      = 1200000;  /* 1.2A */
    g_battery.design_capacity_mah = 4400;
    strncpy(g_battery.technology, "Li-ion", sizeof(g_battery.technology) - 1);
    strncpy(g_battery.model, "Azami-SmartBat", sizeof(g_battery.model) - 1);

    /* 1. Register driver model class /sys/class/power_supply */
    dm_class_register(&g_power_supply_class);

    dm_device_t *ac_dev = dm_device_alloc("AC", NULL);
    if (ac_dev) {
        dm_device_register(ac_dev);
        dm_device_add_class(ac_dev, &g_power_supply_class, 0);
    }

    dm_device_t *bat_dev = dm_device_alloc("BAT0", NULL);
    if (bat_dev) {
        dm_device_register(bat_dev);
        dm_device_add_class(bat_dev, &g_power_supply_class, 1);
    }

    /* 2. Register devfs nodes */
    devfs_register_device("battery", &g_battery_fops, NULL);
    devfs_register_device("ac_adapter", &g_ac_fops, NULL);

    pr_debug("[POWER] ACPI Power Supply subsystem ready: AC=%s, BAT0=%u%% (%s) -> /sys/class/power_supply\n",
             g_ac_adapter.online ? "Online" : "Offline",
             g_battery.capacity_percent, battery_status_str(g_battery.status));
}
