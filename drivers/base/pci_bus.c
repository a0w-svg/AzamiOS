/* ============================================================================
 * AzamiOS — PCI Bus Type Implementation
 * File: drivers/base/pci_bus.c
 *
 * Adopts every PCI function the HAL enumerated into the driver model, names
 * it in Linux's domain:bus:slot.func form, and exposes the configuration
 * space snapshot through sysfs attributes:
 *
 *   /sys/bus/pci/devices/0000:00:02.0/{vendor,device,class,irq,resource,…}
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "pci_bus.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/lib/string.h"

/* ── Matching ────────────────────────────────────────────────────────────── */

static bool pci_id_matches(const pci_device_id_t *id, const pci_device_info_t *info)
{
    if (id->vendor != PCI_ANY_ID && id->vendor != info->vendor_id) return false;
    if (id->device != PCI_ANY_ID && id->device != info->device_id) return false;
    if (id->subvendor != PCI_ANY_ID && id->subvendor != info->subsys_vendor) return false;
    if (id->subdevice != PCI_ANY_ID && id->subdevice != info->subsys_id) return false;

    if (id->dev_class_mask) {
        u32 devclass = ((u32)info->class_code << 16) |
                       ((u32)info->subclass   << 8)  |
                        (u32)info->prog_if;
        if ((devclass & id->dev_class_mask) != (id->dev_class & id->dev_class_mask))
            return false;
    }
    return true;
}

/* Terminator convention: an all-zero entry, as in Linux's { 0 }. */
static bool pci_id_is_terminator(const pci_device_id_t *id)
{
    return id->vendor == 0 && id->device == 0 && id->dev_class_mask == 0;
}

static const pci_device_id_t *pci_match_id(const pci_driver_t *pdrv,
                                           const pci_device_info_t *info)
{
    if (!pdrv->id_table) return NULL;
    for (const pci_device_id_t *id = pdrv->id_table; !pci_id_is_terminator(id); id++) {
        if (pci_id_matches(id, info)) return id;
    }
    return NULL;
}

static bool pci_bus_match(dm_device_t *dev, dm_driver_t *drv)
{
    pci_device_info_t *info = to_pci_info(dev);
    if (!info) return false;
    const pci_driver_t *pdrv = container_of(drv, pci_driver_t, drv);
    return pci_match_id(pdrv, info) != NULL;
}

static int pci_bus_probe(dm_device_t *dev)
{
    pci_driver_t *pdrv = container_of(dev->driver, pci_driver_t, drv);
    pci_device_info_t *info = to_pci_info(dev);
    const pci_device_id_t *id = pci_match_id(pdrv, info);
    if (!id || !pdrv->probe) return -ENODEV;
    return pdrv->probe(dev, id);
}

static void pci_bus_remove(dm_device_t *dev)
{
    pci_driver_t *pdrv = container_of(dev->driver, pci_driver_t, drv);
    if (pdrv->remove) pdrv->remove(dev);
}

/* ── sysfs attributes ────────────────────────────────────────────────────── */

static int pci_bus_uevent(dm_device_t *dev, char *buf, size_t len)
{
    pci_device_info_t *info = to_pci_info(dev);
    if (!info) return 0;
    return scnprintf(buf, len,
                    "PCI_ID=%04X:%04X\nPCI_SUBSYS_ID=%04X:%04X\nPCI_SLOT_NAME=%s\n",
                    info->vendor_id, info->device_id,
                    info->subsys_vendor, info->subsys_id, dev->name);
}

static int pci_bus_attr_show(dm_device_t *dev, const char *attr, char *buf, size_t len)
{
    pci_device_info_t *info = to_pci_info(dev);
    if (!info) return -1;

    if (strcmp(attr, "vendor") == 0)   return scnprintf(buf, len, "0x%04x\n", info->vendor_id);
    if (strcmp(attr, "device") == 0)   return scnprintf(buf, len, "0x%04x\n", info->device_id);
    if (strcmp(attr, "revision") == 0) return scnprintf(buf, len, "0x%02x\n", info->revision);
    if (strcmp(attr, "irq") == 0)      return scnprintf(buf, len, "%u\n", info->interrupt_line);
    if (strcmp(attr, "subsystem_vendor") == 0)
        return scnprintf(buf, len, "0x%04x\n", info->subsys_vendor);
    if (strcmp(attr, "subsystem_device") == 0)
        return scnprintf(buf, len, "0x%04x\n", info->subsys_id);
    if (strcmp(attr, "class") == 0) {
        return scnprintf(buf, len, "0x%02x%02x%02x\n",
                        info->class_code, info->subclass, info->prog_if);
    }
    if (strcmp(attr, "resource") == 0) {
        int n = 0;
        for (u32 i = 0; i < 6 && (size_t)n < len; i++) {
            u32 bar = info->bar[i];
            u64 base = (bar & 1) ? (u64)(bar & ~0x3U) : (u64)(bar & ~0xFU);
            n += scnprintf(buf + n, len - (size_t)n, "0x%016llx 0x%016llx 0x%08x\n",
                          (unsigned long long)base, (unsigned long long)base,
                          (bar & 1) ? 0x100u : 0x200u);
        }
        return n;
    }
    return -1;
}

static const char *const pci_dev_attrs[] = {
    "vendor", "device", "revision", "class", "irq",
    "subsystem_vendor", "subsystem_device", "resource", NULL
};

dm_bus_t dm_pci_bus = {
    .name      = "pci",
    .match     = pci_bus_match,
    .probe     = pci_bus_probe,
    .uevent    = pci_bus_uevent,
    .attr_show = pci_bus_attr_show,
    .dev_attrs = pci_dev_attrs,
};

int pci_driver_register(pci_driver_t *pdrv)
{
    if (!pdrv || !pdrv->id_table) return -EINVAL;
    pdrv->drv.bus       = &dm_pci_bus;
    pdrv->drv.id_table  = pdrv->id_table;
    pdrv->drv.remove    = pci_bus_remove;
    return dm_driver_register(&pdrv->drv);
}

dm_device_t *pci_dm_device_for(device_t *hal_dev)
{
    if (!hal_dev) return NULL;
    for (dm_device_t *d = dm_pci_bus.devices; d; d = d->bus_next) {
        if (d->hal == hal_dev) return d;
    }
    return NULL;
}

/* ── Adoption of the HAL's enumeration ───────────────────────────────────── */

void pci_bus_init(void)
{
    dm_bus_register(&dm_pci_bus);

    device_t *pci_root = device_find("PCI0");
    if (!pci_root) {
        pr_debug("[PCI-BUS] HAL has no PCI0 root — driver model PCI bus is empty\n");
        return;
    }

    u32 adopted = 0;
    for (device_t *hal = pci_root->children; hal; hal = hal->sibling) {
        pci_device_info_t *info = pci_get_device_info(hal);
        if (!info) continue;

        char name[DM_NAME_MAX];
        scnprintf(name, sizeof(name), "0000:%02x:%02x.%x",
                 info->bus, info->slot, info->func);

        dm_device_t *dev = dm_device_alloc(name, &dm_pci_bus);
        if (!dev) continue;

        dev->hal      = hal;
        dev->bus_data = info;
        scnprintf(dev->modalias, sizeof(dev->modalias),
                 "pci:v%08Xd%08Xbc%02Xsc%02Xi%02X",
                 info->vendor_id, info->device_id,
                 info->class_code, info->subclass, info->prog_if);

        if (dm_device_register(dev) == 0) adopted++;
        else kfree(dev);
    }

    pr_debug("[PCI-BUS] %u PCI functions adopted into the driver model\n", adopted);
}
