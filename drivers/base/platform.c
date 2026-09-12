/* ============================================================================
 * AzamiOS — Platform Bus Implementation
 * File: drivers/base/platform.c
 *
 * Matching on the platform bus is by name: a device registered as
 * "serial8250.0" is claimed by the driver named "serial8250".  The trailing
 * ".<id>" is an instance suffix and never part of the match, so one driver
 * picks up every instance the board declares.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "platform.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/boot/limine_req.h"

/* ── Bus operations ──────────────────────────────────────────────────────── */

static bool platform_match(dm_device_t *dev, dm_driver_t *drv)
{
    platform_device_t *pdev = to_platform_device(dev);
    if (!pdev || !pdev->name || !drv->name) return false;
    return strcmp(pdev->name, drv->name) == 0;
}

static int platform_bus_probe(dm_device_t *dev)
{
    platform_driver_t *pdrv = container_of(dev->driver, platform_driver_t, drv);
    platform_device_t *pdev = to_platform_device(dev);
    if (!pdrv->probe) return 0;
    return pdrv->probe(pdev);
}

static void platform_bus_remove(dm_device_t *dev)
{
    platform_driver_t *pdrv = container_of(dev->driver, platform_driver_t, drv);
    if (pdrv->remove) pdrv->remove(to_platform_device(dev));
}

static int platform_uevent(dm_device_t *dev, char *buf, size_t len)
{
    platform_device_t *pdev = to_platform_device(dev);
    if (!pdev) return 0;
    return scnprintf(buf, len, "OF_NAME=%s\nPLATFORM_ID=%d\n", pdev->name, pdev->id);
}

static int platform_attr_show(dm_device_t *dev, const char *attr, char *buf, size_t len)
{
    platform_device_t *pdev = to_platform_device(dev);
    if (!pdev) return -1;

    if (strcmp(attr, "name") == 0) {
        return scnprintf(buf, len, "%s\n", pdev->name);
    }
    if (strcmp(attr, "id") == 0) {
        return scnprintf(buf, len, "%d\n", pdev->id);
    }
    if (strcmp(attr, "resource") == 0) {
        int n = 0;
        for (u32 i = 0; i < pdev->nres && (size_t)n < len; i++) {
            const platform_resource_t *r = &pdev->res[i];
            const char *kind = (r->type == PLATFORM_RES_MEM) ? "mem"
                             : (r->type == PLATFORM_RES_IO)  ? "io" : "irq";
            n += scnprintf(buf + n, len - (size_t)n, "0x%016llx 0x%016llx %s\n",
                          (unsigned long long)r->start,
                          (unsigned long long)(r->start + (r->size ? r->size - 1 : 0)),
                          kind);
        }
        return n;
    }
    return -1;
}

static const char *const platform_dev_attrs[] = { "name", "id", "resource", NULL };

dm_bus_t dm_platform_bus = {
    .name      = "platform",
    .match     = platform_match,
    .probe     = platform_bus_probe,
    .uevent    = platform_uevent,
    .attr_show = platform_attr_show,
    .dev_attrs = platform_dev_attrs,
};

/* ── Registration ────────────────────────────────────────────────────────── */

int platform_device_register(platform_device_t *pdev)
{
    if (!pdev || !pdev->name) return -EINVAL;

    char devname[DM_NAME_MAX];
    if (pdev->id >= 0) {
        scnprintf(devname, sizeof(devname), "%s.%d", pdev->name, pdev->id);
    } else {
        scnprintf(devname, sizeof(devname), "%s", pdev->name);
    }

    dm_device_t *dev = dm_device_alloc(devname, &dm_platform_bus);
    if (!dev) return -ENOMEM;

    dev->bus_data = pdev;
    scnprintf(dev->modalias, sizeof(dev->modalias), "platform:%s", pdev->name);
    pdev->dev = dev;

    int ret = dm_device_register(dev);
    if (ret != 0) {
        pdev->dev = NULL;
        kfree(dev);
    }
    return ret;
}

int platform_driver_register(platform_driver_t *pdrv)
{
    if (!pdrv) return -EINVAL;
    pdrv->drv.bus    = &dm_platform_bus;
    pdrv->drv.remove = platform_bus_remove;
    return dm_driver_register(&pdrv->drv);
}

const platform_resource_t *platform_get_resource(platform_device_t *pdev,
                                                 u32 type, u32 index)
{
    if (!pdev || !pdev->res) return NULL;
    u32 seen = 0;
    for (u32 i = 0; i < pdev->nres; i++) {
        if (pdev->res[i].type != type) continue;
        if (seen++ == index) return &pdev->res[i];
    }
    return NULL;
}

int platform_get_irq(platform_device_t *pdev, u32 index)
{
    const platform_resource_t *r = platform_get_resource(pdev, PLATFORM_RES_IRQ, index);
    return r ? (int)r->start : -1;
}

/* ── Board devices ───────────────────────────────────────────────────────── */
/*
 * The fixed PC/AT hardware AzamiOS already drives, declared so it shows up in
 * the driver model and in /sys/bus/platform/devices next to everything else.
 * Resources mirror what the existing drivers use.
 */

static const platform_resource_t uart0_res[] = {
    { PLATFORM_RES_IO,  0x3F8, 8, "com1" },
    { PLATFORM_RES_IRQ, 4,     1, NULL   },
};
static const platform_resource_t uart1_res[] = {
    { PLATFORM_RES_IO,  0x2F8, 8, "com2" },
    { PLATFORM_RES_IRQ, 3,     1, NULL   },
};
static const platform_resource_t rtc_res[] = {
    { PLATFORM_RES_IO,  0x70, 2, "cmos" },
    { PLATFORM_RES_IRQ, 8,    1, NULL   },
};
static const platform_resource_t i8042_res[] = {
    { PLATFORM_RES_IO,  0x60, 1, "data"   },
    { PLATFORM_RES_IO,  0x64, 1, "status" },
    { PLATFORM_RES_IRQ, 1,    1, "kbd"    },
    { PLATFORM_RES_IRQ, 12,   1, "aux"    },
};
static const platform_resource_t pcspkr_res[] = {
    { PLATFORM_RES_IO,  0x61, 1, "gate"  },
    { PLATFORM_RES_IO,  0x42, 1, "pit2"  },
};
static const platform_resource_t lpt_res[] = {
    { PLATFORM_RES_IO,  0x378, 3, "lpt1" },
    { PLATFORM_RES_IRQ, 7,     1, NULL   },
};
static const platform_resource_t fw_cfg_res[] = {
    { PLATFORM_RES_IO,  0x510, 2, "fw_cfg" },
};
static const platform_resource_t debugcon_res[] = {
    { PLATFORM_RES_IO,  0xE9,  1, "debugcon" },
};
static const platform_resource_t pvpanic_res[] = {
    { PLATFORM_RES_IO,  0x505, 1, "pvpanic" },
};
static const platform_resource_t mpu401_res[] = {
    { PLATFORM_RES_IO,  0x330, 2, "mpu401" },
};
static const platform_resource_t pm_timer_res[] = {
    { PLATFORM_RES_IO,  0x408, 4, "pm_timer" },
};

static platform_device_t g_board_devices[] = {
    { .name = "serial8250",  .id = 0, .res = uart0_res,    .nres = ARRAY_SIZE(uart0_res)    },
    { .name = "serial8250",  .id = 1, .res = uart1_res,    .nres = ARRAY_SIZE(uart1_res)    },
    { .name = "rtc_cmos",    .id = 0, .res = rtc_res,      .nres = ARRAY_SIZE(rtc_res)      },
    { .name = "i8042",       .id = 0, .res = i8042_res,    .nres = ARRAY_SIZE(i8042_res)    },
    { .name = "pcspkr",      .id = 0, .res = pcspkr_res,   .nres = ARRAY_SIZE(pcspkr_res)   },
    { .name = "parport_pc",  .id = 0, .res = lpt_res,      .nres = ARRAY_SIZE(lpt_res)      },
    { .name = "qemu_fw_cfg", .id = 0, .res = fw_cfg_res,   .nres = ARRAY_SIZE(fw_cfg_res)   },
    { .name = "debugcon",    .id = 0, .res = debugcon_res, .nres = ARRAY_SIZE(debugcon_res) },
    { .name = "pvpanic",     .id = 0, .res = pvpanic_res,  .nres = ARRAY_SIZE(pvpanic_res)  },
    { .name = "mpu401",      .id = 0, .res = mpu401_res,   .nres = ARRAY_SIZE(mpu401_res)   },
    { .name = "pm_timer",    .id = 0, .res = pm_timer_res, .nres = ARRAY_SIZE(pm_timer_res) },
};

/* The bootloader framebuffer, described the way Linux's simple-framebuffer
 * device tree node describes it, so simpledrm can bind to it. */
static platform_resource_t g_simplefb_res[1];
static platform_device_t   g_simplefb_dev = {
    .name = "simple-framebuffer", .id = 0,
    .res  = g_simplefb_res, .nres = 0,
};

void platform_bus_init(void)
{
    dm_bus_register(&dm_platform_bus);

    for (u32 i = 0; i < ARRAY_SIZE(g_board_devices); i++) {
        platform_device_register(&g_board_devices[i]);
    }

    struct limine_framebuffer *lfb = az_boot_framebuffer();
    if (lfb && lfb->address) {
        g_simplefb_res[0].type  = PLATFORM_RES_MEM;
        g_simplefb_res[0].start = (u64)((uintptr_t)lfb->address - HHDM_BASE);
        g_simplefb_res[0].size  = (u64)lfb->pitch * lfb->height;
        g_simplefb_res[0].name  = "vram";
        g_simplefb_dev.nres     = 1;
        g_simplefb_dev.pdata    = lfb;
        platform_device_register(&g_simplefb_dev);
    }

    pr_debug("[PLATFORM] %u platform devices declared\n",
             (unsigned)(ARRAY_SIZE(g_board_devices) + (g_simplefb_dev.nres ? 1 : 0)));
}
