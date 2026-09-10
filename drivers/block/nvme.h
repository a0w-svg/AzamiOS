/* ============================================================================
 * AzamiOS — NVM Express (NVMe) 1.x Block Driver
 * File: drivers/block/nvme.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

/**
 * block_nvme_init() — Probe PCI for NVMe controllers (class 0x01, subclass
 * 0x08, prog-if 0x02), bring each one up and register a block device per
 * namespace as "nvmeN".
 */
void block_nvme_init(void);
