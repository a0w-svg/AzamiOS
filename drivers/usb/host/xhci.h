/* ============================================================================
 * AzamiOS — eXtensible Host Controller Interface (xHCI, USB 3.x) driver
 * File: drivers/usb/host/xhci.h
 *
 * Register layout and data structures from the xHCI 1.2 specification
 * (section numbers refer to it).
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"

/* ── Capability registers (5.3) ───────────────────────────────────────────── */
#define XHCI_CAP_CAPLENGTH    0x00   /* u8  */
#define XHCI_CAP_HCIVERSION   0x02   /* u16 */
#define XHCI_CAP_HCSPARAMS1   0x04
#define XHCI_CAP_HCSPARAMS2   0x08
#define XHCI_CAP_HCSPARAMS3   0x0C
#define XHCI_CAP_HCCPARAMS1   0x10
#define XHCI_CAP_DBOFF        0x14
#define XHCI_CAP_RTSOFF       0x18

#define HCS1_MAX_SLOTS(p)     ((p) & 0xFF)
#define HCS1_MAX_INTRS(p)     (((p) >> 8) & 0x7FF)
#define HCS1_MAX_PORTS(p)     (((p) >> 24) & 0xFF)
#define HCS2_MAX_SCRATCH(p)   ((((p) >> 21) & 0x1F) << 5 | (((p) >> 27) & 0x1F))
#define HCC1_AC64             (1U << 0)
#define HCC1_CSZ              (1U << 2)
#define HCC1_PPC              (1U << 3)
#define HCC1_XECP(p)          (((p) >> 16) & 0xFFFF)

/* ── Operational registers (5.4) ──────────────────────────────────────────── */
#define XHCI_OP_USBCMD        0x00
#define XHCI_OP_USBSTS        0x04
#define XHCI_OP_PAGESIZE      0x08
#define XHCI_OP_DNCTRL        0x14
#define XHCI_OP_CRCR          0x18
#define XHCI_OP_DCBAAP        0x30
#define XHCI_OP_CONFIG        0x38
#define XHCI_OP_PORTSC(n)     (0x400 + 0x10 * ((n) - 1))   /* n is 1-based */

#define USBCMD_RS             (1U << 0)
#define USBCMD_HCRST          (1U << 1)
#define USBCMD_INTE           (1U << 2)
#define USBCMD_HSEE           (1U << 3)

#define USBSTS_HCH            (1U << 0)
#define USBSTS_HSE            (1U << 2)
#define USBSTS_EINT           (1U << 3)
#define USBSTS_PCD            (1U << 4)
#define USBSTS_CNR            (1U << 11)
#define USBSTS_HCE            (1U << 12)

#define CRCR_RCS              (1ULL << 0)

#define PORTSC_CCS            (1U << 0)
#define PORTSC_PED            (1U << 1)
#define PORTSC_OCA            (1U << 3)
#define PORTSC_PR             (1U << 4)
#define PORTSC_PLS_SHIFT      5
#define PORTSC_PLS_MASK       (0xFU << PORTSC_PLS_SHIFT)
#define PORTSC_PP             (1U << 9)
#define PORTSC_SPEED(p)       (((p) >> 10) & 0xF)
#define PORTSC_PIC_MASK       (3U << 14)
#define PORTSC_LWS            (1U << 16)
#define PORTSC_CSC            (1U << 17)
#define PORTSC_PEC            (1U << 18)
#define PORTSC_WRC            (1U << 19)
#define PORTSC_OCC            (1U << 20)
#define PORTSC_PRC            (1U << 21)
#define PORTSC_PLC            (1U << 22)
#define PORTSC_CEC            (1U << 23)
#define PORTSC_WAKE_MASK      (7U << 25)
#define PORTSC_WPR            (1U << 31)
/* Every write-1-to-clear change bit. */
#define PORTSC_CHANGE_MASK    (PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | PORTSC_OCC | \
                               PORTSC_PRC | PORTSC_PLC | PORTSC_CEC)
/* The bits a read-modify-write must carry back unchanged. Everything else is
 * either read-only, write-1-to-clear (PED included: writing it back as 1
 * disables the port) or an action trigger. */
#define PORTSC_PRESERVE       (PORTSC_PP | PORTSC_PIC_MASK | PORTSC_WAKE_MASK)

/* PORTSC speed IDs (default Protocol Speed ID mapping, 7.2.2.1.1) */
#define XHCI_SPEED_FULL       1
#define XHCI_SPEED_LOW        2
#define XHCI_SPEED_HIGH       3
#define XHCI_SPEED_SUPER      4
#define XHCI_SPEED_SUPER_PLUS 5

/* ── Runtime registers (5.5) ──────────────────────────────────────────────── */
#define XHCI_RT_IR0           0x20            /* interrupter 0 register set */
#define XHCI_IR_IMAN          0x00
#define XHCI_IR_IMOD          0x04
#define XHCI_IR_ERSTSZ        0x08
#define XHCI_IR_ERSTBA        0x10
#define XHCI_IR_ERDP          0x18

#define IMAN_IP               (1U << 0)
#define IMAN_IE               (1U << 1)
#define ERDP_EHB              (1ULL << 3)

/* ── Extended capabilities (7) ────────────────────────────────────────────── */
#define XECP_ID(v)            ((v) & 0xFF)
#define XECP_NEXT(v)          (((v) >> 8) & 0xFF)
#define XECP_LEGACY           1
#define XECP_PROTOCOL         2

#define LEGACY_BIOS_OWNED     (1U << 16)
#define LEGACY_OS_OWNED       (1U << 24)
#define LEGCTL_SMI_ENABLES    ((1U << 0) | (1U << 4) | (7U << 13))
#define LEGCTL_SMI_EVENTS     (7U << 29)      /* RW1C */

/* ── TRBs (6.4) ───────────────────────────────────────────────────────────── */
typedef struct xhci_trb {
    u64 param;
    u32 status;
    u32 control;
} __attribute__((packed, aligned(16))) xhci_trb_t;

#define TRB_CYCLE             (1U << 0)
#define TRB_ENT               (1U << 1)       /* evaluate next TRB     */
#define TRB_TC                (1U << 1)       /* link: toggle cycle    */
#define TRB_ISP               (1U << 2)       /* interrupt on short    */
#define TRB_CH                (1U << 4)       /* chain                 */
#define TRB_IOC               (1U << 5)       /* interrupt on complete */
#define TRB_IDT               (1U << 6)       /* immediate data        */
#define TRB_BSR               (1U << 9)       /* address: block SET_ADDRESS */
#define TRB_DIR_IN            (1U << 16)
#define TRB_TYPE(t)           ((u32)(t) << 10)
#define TRB_GET_TYPE(c)       (((c) >> 10) & 0x3F)
#define TRB_SLOT(s)           ((u32)(s) << 24)
#define TRB_GET_SLOT(c)       (((c) >> 24) & 0xFF)
#define TRB_EP(e)             ((u32)(e) << 16)
#define TRB_GET_EP(c)         (((c) >> 16) & 0x1F)
#define TRB_TRT(t)            ((u32)(t) << 16) /* setup: transfer type */
#define TRB_GET_CC(s)         (((s) >> 24) & 0xFF)
#define TRB_GET_LEN(s)        ((s) & 0xFFFFFF)

/* TRB types */
#define TRB_NORMAL            1
#define TRB_SETUP             2
#define TRB_DATA              3
#define TRB_STATUS            4
#define TRB_LINK              6
#define TRB_NOOP              8
#define TRB_ENABLE_SLOT       9
#define TRB_DISABLE_SLOT      10
#define TRB_ADDRESS_DEVICE    11
#define TRB_CONFIGURE_EP      12
#define TRB_EVALUATE_CONTEXT  13
#define TRB_RESET_EP          14
#define TRB_STOP_EP           15
#define TRB_SET_TR_DEQUEUE    16
#define TRB_EV_TRANSFER       32
#define TRB_EV_COMMAND        33
#define TRB_EV_PORT_STATUS    34
#define TRB_EV_HOST           37

/* Setup TRB transfer types */
#define TRT_NO_DATA           0
#define TRT_OUT_DATA          2
#define TRT_IN_DATA           3

/* Completion codes (6.4.5) */
#define CC_SUCCESS            1
#define CC_DATA_BUFFER        2
#define CC_BABBLE             3
#define CC_USB_TRANSACTION    4
#define CC_TRB                5
#define CC_STALL              6
#define CC_SHORT_PACKET       13

/* ── Contexts (6.2) ───────────────────────────────────────────────────────── */
/* Slot context */
#define SLOT_CTX_SPEED(s)     ((u32)(s) << 20)
#define SLOT_CTX_ENTRIES(n)   ((u32)(n) << 27)
#define SLOT_CTX_ENTRIES_MASK (0x1FU << 27)
#define SLOT_CTX_PORT(p)      ((u32)(p) << 16)

/* Endpoint context */
#define EP_CTX_INTERVAL(i)    ((u32)(i) << 16)
#define EP_CTX_CERR(c)        ((u32)(c) << 1)
#define EP_CTX_TYPE(t)        ((u32)(t) << 3)
#define EP_CTX_BURST(b)       ((u32)(b) << 8)
#define EP_CTX_MPS(m)         ((u32)(m) << 16)
#define EP_CTX_AVG_TRB(l)     ((u32)(l) & 0xFFFF)
#define EP_CTX_ESIT_LO(p)     (((u32)(p) & 0xFFFF) << 16)

#define EP_TYPE_CONTROL       4
#define EP_TYPE_INT_IN        7

/** xhci_init() — register the xHCI PCI driver (class 0C0330). */
int xhci_init(void);
