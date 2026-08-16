/* ============================================================================
 * AzamiOS — Hardware Abstraction Layer (HAL) Master Init
 * File: hal/hal.c
 *
 * Orchestrates the initialisation of all HAL subsystems.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "hal.h"
#include "../drivers/char/console.h"


void hal_init(void)
{
    pr_debug("[HAL] Initializing Hardware Abstraction Layer...\n");

    /* Step 1: Device tree (creates root \Device\System) */
    device_init();

    /* Step 2: Driver registry */
    driver_init();

    /* Step 3: PCI bus enumeration (creates \Device\PCI0 and children) */
    pci_init();

    /* Step 4: Dump the full device tree for diagnostic purposes */
    device_dump_tree();

    pr_debug("[HAL] Hardware Abstraction Layer ready.\n");
}
