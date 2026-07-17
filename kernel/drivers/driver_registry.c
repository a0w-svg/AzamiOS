#include "include/driver.h"
#include "../klibc/include/stdio.h"
#include "../klibc/include/string.h"

#define MAX_DRIVERS 32

static driver_desc_t *g_drivers[MAX_DRIVERS];
static uint32_t       g_driver_count = 0;

void driver_register(driver_desc_t *drv) {
    if (!drv || g_driver_count >= MAX_DRIVERS) return;
    g_drivers[g_driver_count++] = drv;
}

void driver_probe_all(void) {
    kprintf("drv: probing %u registered driver(s) against %u PCI device(s)\n",
            (unsigned)g_driver_count, (unsigned)g_pci_count);

    for (uint32_t d = 0; d < g_driver_count; d++) {
        driver_desc_t *drv = g_drivers[d];
        if (!drv || !drv->ops.probe || !drv->ops.init) continue;

        for (uint32_t p = 0; p < g_pci_count; p++) {
            pci_device_t *pdev = &g_pci_table[p];
            if (!pdev->valid) continue;
            if (drv->ops.probe(pdev) == 0) {
                kprintf("drv: [%s] claimed PCI %u:%u.%u (vendor=%04x dev=%04x)\n",
                        drv->name,
                        (unsigned)pdev->bus, (unsigned)pdev->slot, (unsigned)pdev->func,
                        (unsigned)pdev->vendor_id, (unsigned)pdev->device_id);
                drv->ops.init(pdev);
                pdev->valid = false; /* claim: prevent double-bind */
                break;
            }
        }
    }
}
