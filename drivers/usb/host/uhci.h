/* ============================================================================
 * AzamiOS — Universal Host Controller Interface (UHCI) Driver Header
 * File: drivers/usb/host/uhci.h
 * ============================================================================ */
#pragma once

#include "../core/usb.h"
#include "../../../hal/pci.h"
#include "../../base/pci_bus.h"

/* ── UHCI I/O Registers ──────────────────────────────────────────────────── */
#define UHCI_REG_USBCMD      0x00  /* 16-bit USB Command */
#define UHCI_REG_USBSTS      0x02  /* 16-bit USB Status */
#define UHCI_REG_USBINTR     0x04  /* 16-bit USB Interrupt Enable */
#define UHCI_REG_FRNUM       0x06  /* 16-bit Frame Number */
#define UHCI_REG_FLBASEADD   0x08  /* 32-bit Frame List Base Address */
#define UHCI_REG_SOFMOD      0x0C  /* 8-bit Start of Frame Modify */
#define UHCI_REG_PORTSC1     0x10  /* 16-bit Port 1 Status / Control */
#define UHCI_REG_PORTSC2     0x12  /* 16-bit Port 2 Status / Control */

/* ── USBCMD Register Bits ────────────────────────────────────────────────── */
#define UHCI_CMD_RS          (1 << 0)  /* Run / Stop (1=Run) */
#define UHCI_CMD_HCRESET     (1 << 1)  /* Host Controller Reset */
#define UHCI_CMD_GRESET      (1 << 2)  /* Global Reset */
#define UHCI_CMD_EGSM        (1 << 3)  /* Enter Global Suspend Mode */
#define UHCI_CMD_FGR         (1 << 4)  /* Force Global Resume */
#define UHCI_CMD_SWDBG       (1 << 5)  /* Software Debug */
#define UHCI_CMD_CF          (1 << 6)  /* Configure Flag */
#define UHCI_CMD_MAXP        (1 << 7)  /* Max Packet (0=32B, 1=64B) */

/* ── USBSTS Register Bits ────────────────────────────────────────────────── */
#define UHCI_STS_USBINT      (1 << 0)  /* USB Interrupt */
#define UHCI_STS_ERROR       (1 << 1)  /* USB Error Interrupt */
#define UHCI_STS_RD          (1 << 2)  /* Resume Detect */
#define UHCI_STS_HSE         (1 << 3)  /* Host System Error */
#define UHCI_STS_HCPE        (1 << 4)  /* Host Controller Process Error */
#define UHCI_STS_HCHALTED    (1 << 5)  /* Host Controller Halted */

/* ── PORTSC Register Bits ────────────────────────────────────────────────── */
#define UHCI_PORT_CCS        (1 << 0)  /* Current Connect Status */
#define UHCI_PORT_CSC        (1 << 1)  /* Connect Status Change (W1C) */
#define UHCI_PORT_PE         (1 << 2)  /* Port Enable */
#define UHCI_PORT_PEC        (1 << 3)  /* Port Enable Change (W1C) */
#define UHCI_PORT_LINE_DM    (1 << 4)  /* Line Status D- */
#define UHCI_PORT_LINE_DP    (1 << 5)  /* Line Status D+ */
#define UHCI_PORT_RD         (1 << 6)  /* Resume Detect */
#define UHCI_PORT_LSDA       (1 << 8)  /* Low Speed Device Attached */
#define UHCI_PORT_PR         (1 << 9)  /* Port Reset */
#define UHCI_PORT_SUSP       (1 << 12) /* Suspend */

/* ── UHCI Queue Head (16 bytes) ──────────────────────────────────────────── */
typedef struct __attribute__((aligned(16))) uhci_qh {
    u32 head_link;
    u32 element_link;
    u32 reserved[2];
} uhci_qh_t;

/* ── UHCI Transfer Descriptor (32 bytes) ─────────────────────────────────── */
typedef struct __attribute__((aligned(16))) uhci_td {
    u32 link;
    u32 ctrl;
    u32 token;
    u32 buffer;
    u32 reserved[4];
} uhci_td_t;

/* ── UHCI Controller State ───────────────────────────────────────────────── */
typedef struct uhci_controller {
    u16          io_base;
    u8           irq;
    u32         *frame_list_virt;
    phys_addr_t  frame_list_phys;
    uhci_qh_t   *default_qh;
    phys_addr_t  default_qh_phys;
    usb_bus_t    bus;
    bool         running;
    u32          ports_detected;
} uhci_controller_t;

/** uhci_init() — Register the UHCI PCI driver in the unified driver model. */
int uhci_init(void);
