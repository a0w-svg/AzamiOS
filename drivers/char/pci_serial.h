/* ============================================================================
 * AzamiOS — PCI Multi-Port Serial (16550A) Driver
 * File: drivers/char/pci_serial.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

void pci_serial_init(void);
u32  pci_serial_get_port_count(void);
