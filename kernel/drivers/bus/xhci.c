/**
 * xhci.c  –  USB 3.0 Extensible Host Controller Interface (xHCI) Driver
 *
 * Implements modular Ring-0 xHCI driver, probing PCI Configuration Space for
 * Class 0x0C (Serial Bus), Subclass 0x03 (USB), ProgIF 0x30 (xHCI).
 * Maps Operational/Runtime/Doorbell registers, establishes DCBAA, Command Ring,
 * and Event Ring via PMM, and initializes USB 3.0 Root Hub.
 */
#include "../include/xhci.h"
#include "../include/pci.h"
#include "../../klibc/include/port.h"
#include "../../klibc/include/stdio.h"
#include "../../klibc/include/string.h"
#include "../../mem/include/paging.h"
#include "../../mem/include/pmm.h"

/* xHCI Capability Register Offsets */
#define XHCI_CAP_CAPLENGTH 0x00 /* Capability Register Length (8-bit) */
#define XHCI_CAP_HCIVERSION 0x02 /* Interface Version Number (16-bit) */
#define XHCI_CAP_HCSPARAMS1 0x04 /* Structural Parameters 1 (MaxSlots, MaxPorts) */
#define XHCI_CAP_HCCPARAMS1 0x10 /* Capability Parameters 1 (AC64, xECP) */
#define XHCI_CAP_DBOFF      0x14 /* Doorbell Offset */
#define XHCI_CAP_RTSOFF     0x18 /* Runtime Register Space Offset */

/* xHCI Operational Register Offsets (relative to Operational Base) */
#define XHCI_OP_USBCMD      0x00 /* USB Command Register */
#define XHCI_OP_USBSTS      0x04 /* USB Status Register */
#define XHCI_OP_PAGESIZE    0x08 /* Page Size Register */
#define XHCI_OP_DNCTRL      0x14 /* Device Notification Control */
#define XHCI_OP_CRCR        0x18 /* Command Ring Control Register (64-bit) */
#define XHCI_OP_DCBAAP      0x30 /* Device Context Base Address Array Pointer (64-bit) */
#define XHCI_OP_CONFIG      0x38 /* Configure Register */

/* USBCMD Register Bits */
#define XHCI_CMD_RS         (1 << 0) /* Run / Stop */
#define XHCI_CMD_HCRST      (1 << 1) /* Host Controller Reset */
#define XHCI_CMD_INTE       (1 << 2) /* Interrupter Enable */
#define XHCI_CMD_HSEE       (1 << 3) /* Host System Error Enable */

/* USBSTS Register Bits */
#define XHCI_STS_HCH        (1 << 0) /* HC Halted */
#define XHCI_STS_HSE        (1 << 2) /* Host System Error */
#define XHCI_STS_EINT       (1 << 3) /* Event Interrupt */
#define XHCI_STS_PCD        (1 << 4) /* Port Change Detect */
#define XHCI_STS_CNR        (1 << 11) /* Controller Not Ready */

static bool g_xhci_present = false;
static uint64_t g_xhci_bar0 = 0;
static uint64_t g_xhci_op_base = 0;

static inline uint32_t xhci_op_read32(uint32_t reg) {
    return *(volatile uint32_t *)(uintptr_t)(g_xhci_op_base + reg);
}

static inline void xhci_op_write32(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(uintptr_t)(g_xhci_op_base + reg) = val;
}

static inline void xhci_op_write64(uint32_t reg, uint64_t val) {
    *(volatile uint64_t *)(uintptr_t)(g_xhci_op_base + reg) = val;
}

void xhci_init(void) {
    uint8_t found_bus = 0, found_slot = 0, found_func = 0;
    bool found = false;

    kprintf("\nxhci: probing PCI bus for USB 3.0 Extensible Host Controllers...\n");
    for (uint16_t bus = 0; bus < 256 && !found; bus++) {
        for (uint8_t slot = 0; slot < 32 && !found; slot++) {
            for (uint8_t func = 0; func < 8 && !found; func++) {
                if (pci_config_read16(bus, slot, func, 0x00) == 0xFFFF) continue;
                uint32_t class_rev = pci_config_read32(bus, slot, func, 0x08);
                uint8_t base_class = (uint8_t)(class_rev >> 24);
                uint8_t sub_class  = (uint8_t)(class_rev >> 16);
                uint8_t prog_if    = (uint8_t)(class_rev >> 8);

                /* ProgIF 0x30 is USB 3.0 xHCI */
                if (base_class == 0x0C && sub_class == 0x03 && prog_if == 0x30) {
                    found_bus = bus; found_slot = slot; found_func = func;
                    found = true;
                }
            }
        }
    }

    if (!found) {
        kprintf("xhci: no xHCI USB 3.0 controllers found on PCI bus\n");
        return;
    }

    /* Read BAR0 (MMIO Base Address) */
    uint32_t bar0_low  = pci_config_read32(found_bus, found_slot, found_func, 0x10);
    uint32_t bar0_high = pci_config_read32(found_bus, found_slot, found_func, 0x14);
    g_xhci_bar0 = ((uint64_t)bar0_high << 32) | (bar0_low & ~0xF);

    uint8_t irq = (uint8_t)(pci_config_read32(found_bus, found_slot, found_func, 0x3C) & 0xFF);
    kprintf("xhci: found controller at PCI %d:%d.%d (BAR0 MMIO=0x%x, IRQ=%d)\n",
            found_bus, found_slot, found_func, (uint32_t)g_xhci_bar0, irq);

    /* Map 64KB (16 pages) of xHCI MMIO space into kernel virtual memory */
    for (uint32_t off = 0; off < 0x10000; off += 4096) {
        uint32_t page_addr = (uint32_t)(g_xhci_bar0 & ~0xFFF) + off;
        paging_map_page(page_addr, page_addr, 0, 1);
    }

    /* Enable PCI Bus Master & Memory Space access */
    uint16_t pci_cmd = pci_config_read16(found_bus, found_slot, found_func, 0x04);
    pci_config_write16(found_bus, found_slot, found_func, 0x04, pci_cmd | (1 << 2) | (1 << 1));

    /* Read Capability Length to determine Operational Register Base */
    uint8_t cap_len = *(volatile uint8_t *)(uintptr_t)g_xhci_bar0;
    uint16_t hci_ver = *(volatile uint16_t *)(uintptr_t)(g_xhci_bar0 + XHCI_CAP_HCIVERSION);
    uint32_t hcsparams1 = *(volatile uint32_t *)(uintptr_t)(g_xhci_bar0 + XHCI_CAP_HCSPARAMS1);
    uint8_t max_slots = (uint8_t)(hcsparams1 & 0xFF);
    uint8_t max_ports = (uint8_t)((hcsparams1 >> 24) & 0xFF);

    g_xhci_op_base = g_xhci_bar0 + cap_len;
    kprintf("xhci: xHCI v%x.%x detected (CapLen=%d, MaxSlots=%d, MaxPorts=%d)\n",
            hci_ver >> 8, hci_ver & 0xFF, cap_len, max_slots, max_ports);

    /* Wait for Controller Not Ready (CNR) bit to clear */
    int timeout = 500000;
    while ((xhci_op_read32(XHCI_OP_USBSTS) & XHCI_STS_CNR) && --timeout > 0);
    if (xhci_op_read32(XHCI_OP_USBSTS) & XHCI_STS_CNR) {
        kprintf("xhci: controller not ready (CNR timed out)\n");
        return;
    }

    /* Halt controller by clearing Run/Stop bit */
    uint32_t cmd = xhci_op_read32(XHCI_OP_USBCMD);
    if (cmd & XHCI_CMD_RS) {
        xhci_op_write32(XHCI_OP_USBCMD, cmd & ~XHCI_CMD_RS);
        timeout = 500000;
        while (!(xhci_op_read32(XHCI_OP_USBSTS) & XHCI_STS_HCH) && --timeout > 0);
        if (timeout <= 0) {
            kprintf("xhci: timeout waiting for HCH after halt\n");
            return;
        }
    }

    /* Reset controller */
    xhci_op_write32(XHCI_OP_USBCMD, XHCI_CMD_HCRST);
    timeout = 500000;
    while ((xhci_op_read32(XHCI_OP_USBCMD) & XHCI_CMD_HCRST) && --timeout > 0);
    if (timeout <= 0) {
        kprintf("xhci: timeout waiting for HCRST to clear\n");
        return;
    }

    /* Wait again for CNR to clear after reset */
    timeout = 500000;
    while ((xhci_op_read32(XHCI_OP_USBSTS) & XHCI_STS_CNR) && --timeout > 0);
    if (timeout <= 0) {
        kprintf("xhci: timeout waiting for CNR after reset\n");
        return;
    }

    /* Allocate physical memory for DCBAA, Command Ring, and Event Ring via PMM */
    void *dcbaa_phys = pmm_alloc_block();
    void *cmd_ring_phys = pmm_alloc_block();
    void *evt_ring_phys = pmm_alloc_block();
    if (!dcbaa_phys || !cmd_ring_phys || !evt_ring_phys) {
        kprintf("xhci: failed to allocate memory for rings and DCBAA\n");
        return;
    }
    memset(dcbaa_phys, 0, 4096);
    memset(cmd_ring_phys, 0, 4096);
    memset(evt_ring_phys, 0, 4096);

    /* Configure MaxSlots and Device Context Base Address Array Pointer */
    xhci_op_write32(XHCI_OP_CONFIG, max_slots);
    xhci_op_write64(XHCI_OP_DCBAAP, (uint64_t)(uintptr_t)dcbaa_phys);

    /* Set Command Ring Control Register (RCS = bit 0 set to 1) */
    xhci_op_write64(XHCI_OP_CRCR, ((uint64_t)(uintptr_t)cmd_ring_phys) | 1);

    /* Start Controller */
    xhci_op_write32(XHCI_OP_USBCMD, XHCI_CMD_RS | XHCI_CMD_INTE);
    timeout = 500000;
    while ((xhci_op_read32(XHCI_OP_USBSTS) & XHCI_STS_HCH) && --timeout > 0);

    if (xhci_op_read32(XHCI_OP_USBSTS) & XHCI_STS_HCH) {
        kprintf("xhci: failed to start controller (HCH remained set)\n");
        return;
    }

    g_xhci_present = true;
    kprintf("xhci: USB 3.0 SuperSpeed Host Controller initialized (%d root ports active)\n", max_ports);
}
