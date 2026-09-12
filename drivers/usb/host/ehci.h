/* ============================================================================
 * AzamiOS — Enhanced Host Controller Interface (EHCI) Driver Header
 * File: drivers/usb/host/ehci.h
 * ============================================================================ */
#pragma once

#include "../core/usb.h"
#include "../../../hal/pci.h"
#include "../../base/pci_bus.h"

/* ── Capability Registers (relative to MMIO base) ────────────────────────── */
#define EHCI_CAP_CAPLENGTH          0x00  /* 8-bit Capability Registers Length */
#define EHCI_CAP_HCIVERSION         0x02  /* 16-bit Interface Version Number */
#define EHCI_CAP_HCSPARAMS          0x04  /* 32-bit Structural Parameters */
#define EHCI_CAP_HCCPARAMS          0x08  /* 32-bit Capability Parameters */

/* ── Operational Registers (relative to MMIO base + CAPLENGTH) ───────────── */
#define EHCI_OP_USBCMD              0x00  /* 32-bit USB Command */
#define EHCI_OP_USBSTS              0x04  /* 32-bit USB Status */
#define EHCI_OP_USBINTR             0x08  /* 32-bit USB Interrupt Enable */
#define EHCI_OP_FRINDEX             0x0C  /* 32-bit Frame Index */
#define EHCI_OP_CTRLDSSEGMENT       0x10  /* 32-bit 4G Segment Selector */
#define EHCI_OP_PERIODICLISTBASE    0x14  /* 32-bit Frame List Base Address */
#define EHCI_OP_ASYNCLISTADDR       0x18  /* 32-bit Next Asynchronous Link Address */
#define EHCI_OP_CONFIGFLAG          0x40  /* 32-bit Configured Flag Register */
#define EHCI_OP_PORTSC(n)           (0x44 + (((n) - 1) * 4)) /* Port Status & Control */

/* ── USBCMD Register Bits ────────────────────────────────────────────────── */
#define EHCI_CMD_RUN                (1 << 0)  /* Run/Stop (1=Run) */
#define EHCI_CMD_HCRESET            (1 << 1)  /* Host Controller Reset */
#define EHCI_CMD_FLS_1024           (0 << 2)  /* Frame List Size: 1024 elements */
#define EHCI_CMD_PSE                (1 << 4)  /* Periodic Schedule Enable */
#define EHCI_CMD_ASE                (1 << 5)  /* Asynchronous Schedule Enable */
#define EHCI_CMD_IAAD               (1 << 6)  /* Interrupt on Async Advance Doorbell */

/* ── USBSTS Register Bits ────────────────────────────────────────────────── */
#define EHCI_STS_USBINT             (1 << 0)  /* USB Interrupt */
#define EHCI_STS_USBERRINT          (1 << 1)  /* USB Error Interrupt */
#define EHCI_STS_PORTCHANGE         (1 << 2)  /* Port Change Detect */
#define EHCI_STS_FLROLLOVER         (1 << 3)  /* Frame List Rollover */
#define EHCI_STS_HOSTSYSERR         (1 << 4)  /* Host System Error */
#define EHCI_STS_INTASYNC           (1 << 5)  /* Interrupt on Async Advance */
#define EHCI_STS_HCHALTED           (1 << 12) /* HC Halted */
#define EHCI_STS_RECLAMATION        (1 << 13) /* Reclamation status */
#define EHCI_STS_PSS                (1 << 14) /* Periodic Schedule Status */
#define EHCI_STS_ASS                (1 << 15) /* Asynchronous Schedule Status */

/* ── PORTSC Register Bits ────────────────────────────────────────────────── */
#define EHCI_PORT_CCS               (1 << 0)  /* Current Connect Status */
#define EHCI_PORT_CSC               (1 << 1)  /* Connect Status Change (W1C) */
#define EHCI_PORT_PE                (1 << 2)  /* Port Enable */
#define EHCI_PORT_PEC               (1 << 3)  /* Port Enable Change (W1C) */
#define EHCI_PORT_OCA               (1 << 4)  /* Over-current Active */
#define EHCI_PORT_OCC               (1 << 5)  /* Over-current Change (W1C) */
#define EHCI_PORT_FPR               (1 << 6)  /* Force Port Resume */
#define EHCI_PORT_SUSP              (1 << 7)  /* Suspend */
#define EHCI_PORT_RESET             (1 << 8)  /* Port Reset */
#define EHCI_PORT_LS_MASK           (3 << 10) /* Line Status */
#define EHCI_PORT_PP                (1 << 12) /* Port Power */
#define EHCI_PORT_OWNER             (1 << 13) /* Port Owner (0=EHCI, 1=Companion HC) */

/* ── EHCI Transfer Descriptor (qTD) (32 bytes) ───────────────────────────── */
typedef struct __attribute__((aligned(32))) ehci_qtd {
    u32 next_qtd;          /* Pointer to next qTD, bit 0 = terminate */
    u32 alt_next_qtd;      /* Alternate next qTD, bit 0 = terminate */
    u32 token;             /* Status, PID code, error counter, total bytes */
    u32 buffer[5];         /* Page buffer pointers (4KB pages) */
} ehci_qtd_t;

/* ── EHCI Queue Head (QH) (48 bytes padded to 64 bytes) ──────────────────── */
typedef struct __attribute__((aligned(64))) ehci_qh {
    u32 horizontal_link;   /* Next QH physical address | 0x2 (QH type) */
    u32 ep_char;           /* Endpoint characteristics (addr, ep, speed, max packet) */
    u32 ep_caps;           /* Endpoint capabilities (mult, port, hub) */
    u32 current_qtd;       /* Current executing qTD pointer */
    /* Overlay area (matches qTD) */
    u32 next_qtd;
    u32 alt_next_qtd;
    u32 token;
    u32 buffer[5];
    u32 reserved[4];       /* Padding to ensure clean alignment */
} ehci_qh_t;

/* ── EHCI Controller State ───────────────────────────────────────────────── */
typedef struct ehci_controller {
    virt_addr_t   mmio_base;
    virt_addr_t   op_regs;
    phys_addr_t   mmio_phys;
    u8            caplength;
    u16           hciversion;
    u8            num_ports;
    u8            irq;
    u32          *periodic_list_virt;
    phys_addr_t   periodic_list_phys;
    ehci_qh_t    *async_qh;
    phys_addr_t   async_qh_phys;
    usb_bus_t     bus;
    bool          running;
    u32           ports_detected;
} ehci_controller_t;

/* ── Character Device IOCTLs ─────────────────────────────────────────────── */
#define EHCI_IOC_RESET_PORT         0x5510
#define EHCI_IOC_GET_NUM_PORTS      0x5511

/** ehci_init() — Register the EHCI USB 2.0 PCI driver in the driver model. */
int ehci_init(void);
