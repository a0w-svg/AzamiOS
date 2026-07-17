#ifndef DRIVER_H
#define DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "pci.h"

typedef enum {
    DRV_BLOCK   = 0,
    DRV_NET     = 1,
    DRV_DISPLAY = 2,
    DRV_CHAR    = 3,
    DRV_INPUT   = 4,
} driver_class_t;

typedef struct driver_ops {
    int  (*probe)(pci_device_t *pci);
    int  (*init)(pci_device_t *pci);
    int  (*read)(void *dev, uint64_t offset, uint32_t len, void *buf);
    int  (*write)(void *dev, uint64_t offset, uint32_t len, const void *buf);
    int  (*ioctl)(void *dev, uint32_t cmd, void *arg);
    void (*remove)(void *dev);
} driver_ops_t;

typedef struct driver_desc {
    const char    *name;
    driver_class_t dclass;
    driver_ops_t   ops;
} driver_desc_t;

void driver_register(driver_desc_t *drv);
void driver_probe_all(void);

#endif
