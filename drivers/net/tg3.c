#include "../../hal/pci.h"
#include "../base/pci_bus.h"
#include "kernel/panic.h"
#include "hal/device.h"

static int tg3_pci_probe(dm_device_t *dev, const pci_device_id_t *id)
{
    (void)id;
    pci_enable_bus_mastering(dev->hal);
    return 0; // successfully probed
}

static const pci_device_id_t g_tg3_pci_ids[] = {
    { PCI_DEVICE(0x14E4, 0x165D) },
    { {0} }
};

static pci_driver_t g_tg3_pci_driver = {
    .drv = {
        .name = "tg3",
    },
    .id_table = g_tg3_pci_ids,
    .probe    = tg3_pci_probe,
    .remove   = NULL
};

void tg3_init(void)
{
    pci_driver_register(&g_tg3_pci_driver);
}
