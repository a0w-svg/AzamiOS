/* ============================================================================
 * AzamiOS — Hardware Abstraction Layer (HAL) Umbrella Header
 * File: hal/hal.h
 *
 * Includes all HAL sub-headers and provides the master hal_init() function
 * that initialises the device tree, driver registry, and bus enumeration.
 * ============================================================================ */
#pragma once

#include "device.h"
#include "driver.h"
#include "irp.h"
#include "pci.h"

/**
 * hal_init() — Initialise the Hardware Abstraction Layer.
 *
 * Must be called after kmalloc_init() (needs heap) and after
 * az_object_manager_init() (needs object namespace).
 *
 * Initialisation sequence:
 *   1. device_init()  — create the root device tree
 *   2. driver_init()  — initialise the driver registry
 *   3. pci_init()     — enumerate the PCI bus and populate the device tree
 *   4. Dump device tree summary to console
 */
void hal_init(void);
