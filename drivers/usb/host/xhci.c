/* ============================================================================
 * AzamiOS — eXtensible Host Controller Interface (xHCI, USB 3.x) driver
 * File: drivers/usb/host/xhci.c
 *
 * Every PC made since about 2012 routes all of its USB ports — and with them
 * the keyboard and mouse on most desktops, and on laptops whose firmware no
 * longer emulates PS/2 once it hands over — through an xHCI controller. This
 * driver brings one up and gives its devices to the USB core (usb.c), which
 * binds class drivers (usbhid.c).
 *
 * Bring-up, per controller (xHCI 1.2 §4.2):
 *   - take ownership from the BIOS (USB Legacy Support capability), which
 *     otherwise keeps servicing the controller from SMM to emulate PS/2;
 *   - on the Intel PCHs that share ports between EHCI and xHCI, switch every
 *     port over to xHCI (what Linux's usb_enable_intel_xhci_ports() does);
 *   - halt, reset, and program the device context array, scratchpad
 *     buffers, command ring and one event ring;
 *   - run, and hand port handling to a kernel thread.
 *
 * The thread does everything that follows: it watches the event ring, resets
 * and enumerates newly connected ports (so none of that delays boot), tears
 * down devices that go away, recovers halted endpoints, and runs the class
 * drivers' periodic work. Owning all of a controller's state from one thread
 * is what lets the driver go without locks.
 *
 * Interrupts are not used: this kernel has no MSI support yet and PCI INTx
 * lines are shared, so the event ring is polled — every 2 ms while a device
 * is attached (USB HID devices report every 1-10 ms anyway), every 20 ms
 * otherwise. The sleeps between polls are high-resolution
 * (sched_sleep_until_ns), so this costs a few hundred cheap wake-ups a
 * second, not a busy CPU.
 *
 * Not implemented: hubs (devices plug into root ports only), isochronous and
 * bulk transfers (so no USB audio or mass storage yet), and USB 3 streams.
 * ============================================================================ */

#define DEBUG 1
#include "../../../include/azami/debug.h"
#include "xhci.h"
#include "../core/usb.h"
#include "../../base/pci_bus.h"
#include "../../../hal/pci.h"
#include "../../../kernel/mm/pmm.h"
#include "../../../kernel/mm/kmalloc.h"
#include "../../../kernel/lib/string.h"
#include "../../../kernel/sched/sched.h"
#include "../../../kernel/time/timekeeping.h"
#include "../../../arch/x86_64/mm/vmm.h"

#define XHCI_MAX_CONTROLLERS  4
#define XHCI_RING_TRBS        (PAGE_SIZE / sizeof(xhci_trb_t))   /* 256 */
#define XHCI_SLOTS_LIMIT      64      /* device slots enabled per controller */

#define XHCI_CMD_TIMEOUT_MS   5000
#define XHCI_CTRL_TIMEOUT_MS  2000

#define barrier() __asm__ volatile("" ::: "memory")

typedef struct xhci_ring {
    xhci_trb_t  *trbs;
    phys_addr_t  phys;
    u32          enq;
    u32          cycle;
} xhci_ring_t;

typedef struct xhci_ep {
    xhci_ring_t   ring;
    bool          active;
    bool          needs_reset;
    u16           mps;
    u8           *buf;
    phys_addr_t   buf_phys;
    usb_intr_cb_t cb;
    void         *cb_ctx;
} xhci_ep_t;

struct xhci_hc;

typedef struct xhci_slot {
    struct xhci_hc *hc;
    u8            id;
    u8            port;
    u8            xspeed;
    u8           *in_ctx;
    phys_addr_t   in_ctx_phys;
    u8           *out_ctx;
    phys_addr_t   out_ctx_phys;
    xhci_ep_t     ep[32];               /* by device context index; 1 = EP0 */
    u8           *ctrl_buf;             /* bounce page for control data     */
    phys_addr_t   ctrl_buf_phys;
    u64           ctrl_data_trb;        /* data-stage TRB of the transfer in flight */
    volatile bool ctrl_done;
    u8            ctrl_cc;
    u32           ctrl_residual;
    usb_device_t *udev;
} xhci_slot_t;

typedef struct xhci_hc {
    int               index;
    char              name[12];
    dm_device_t      *dm;
    pci_device_info_t *pci;

    volatile u8      *cap, *op, *rt, *db;
    u32               max_slots, max_ports;
    u32               ctx_size;
    bool              ac64;
    bool              dead;

    u64              *dcbaa;
    phys_addr_t       dcbaa_phys;
    xhci_ring_t       cmd;
    xhci_trb_t       *evt;
    phys_addr_t       evt_phys;
    u32               evt_deq;
    u32               evt_cycle;
    u64              *erst;
    phys_addr_t       erst_phys;

    volatile bool     cmd_done;
    u64               cmd_trb;
    u8                cmd_cc;
    u8                cmd_slot;

    xhci_slot_t      *slots[256];
    xhci_slot_t      *port_slot[256];
    u8                port_major[256];  /* USB major revision behind each port */
    volatile bool     port_event[256];

    usb_bus_t         bus;
} xhci_hc_t;

static xhci_hc_t *g_hcs[XHCI_MAX_CONTROLLERS];
static int        g_hc_count;

/* ── Register and memory helpers ──────────────────────────────────────────── */

static inline u32  rd32(volatile u8 *b, u32 o)        { return *(volatile u32 *)(b + o); }
static inline void wr32(volatile u8 *b, u32 o, u32 v) { *(volatile u32 *)(b + o) = v; }
/* 64-bit registers written as two dwords, low first: valid whether or not
 * the controller accepts 64-bit MMIO (xHCI 5.1). */
static inline void wr64(volatile u8 *b, u32 o, u64 v)
{
    wr32(b, o, (u32)v);
    wr32(b, o + 4, (u32)(v >> 32));
}

static inline u32  portsc(xhci_hc_t *hc, u32 port)          { return rd32(hc->op, XHCI_OP_PORTSC(port)); }
static inline void portsc_write(xhci_hc_t *hc, u32 port, u32 v) { wr32(hc->op, XHCI_OP_PORTSC(port), v); }

/* One zeroed page the controller can address: anywhere, or below 4 GiB for
 * a controller without 64-bit addressing (HCCPARAMS1.AC64 = 0). */
static void *dma_page(xhci_hc_t *hc, phys_addr_t *phys)
{
    phys_addr_t p = hc->ac64 ? pmm_alloc_page() : pmm_alloc_32(0);
    if (!p) return NULL;
    void *v = PHYS_TO_VIRT(p);
    memset(v, 0, PAGE_SIZE);
    *phys = p;
    return v;
}

static void dma_free(phys_addr_t p)
{
    if (p) pmm_free_page(p);
}

static void xhci_sleep_ms(u32 ms)
{
    u64 deadline = ktime_get_ns() + (u64)ms * 1000000ULL;
    while (ktime_get_ns() < deadline) sched_sleep_until_ns(deadline);
}

/* Poll a register until (value & mask) == want, sleeping 1 ms between reads. */
static bool wait_reg(volatile u8 *base, u32 off, u32 mask, u32 want, u32 timeout_ms)
{
    u64 deadline = ktime_get_ns() + (u64)timeout_ms * 1000000ULL;
    for (;;) {
        u32 v = rd32(base, off);
        if (v == 0xFFFFFFFFU) return false;             /* device gone */
        if ((v & mask) == want) return true;
        if (ktime_get_ns() >= deadline) return false;
        xhci_sleep_ms(1);
    }
}

/* ── Rings ────────────────────────────────────────────────────────────────── */

static int ring_init(xhci_hc_t *hc, xhci_ring_t *r)
{
    r->trbs = (xhci_trb_t *)dma_page(hc, &r->phys);
    if (!r->trbs) return -ENOMEM;
    r->enq   = 0;
    r->cycle = 1;
    /* The last slot is a Link TRB back to the start with Toggle Cycle, which
     * is how the producer's cycle bit flips on every lap. Its own cycle bit
     * is written when the producer reaches it (ring_push). */
    xhci_trb_t *link = &r->trbs[XHCI_RING_TRBS - 1];
    link->param   = r->phys;
    link->status  = 0;
    link->control = TRB_TYPE(TRB_LINK) | TRB_TC;
    return 0;
}

static void ring_free(xhci_ring_t *r)
{
    dma_free(r->phys);
    r->trbs = NULL;
    r->phys = 0;
}

/* Queue one TRB; returns its physical address (what events point back to).
 * The cycle bit is written last, after the rest of the TRB is visible — it
 * is what hands the TRB to the controller. */
static u64 ring_push(xhci_ring_t *r, u64 param, u32 status, u32 control)
{
    xhci_trb_t *t = &r->trbs[r->enq];
    u64 phys = r->phys + (u64)r->enq * sizeof(xhci_trb_t);
    t->param  = param;
    t->status = status;
    barrier();
    *(volatile u32 *)&t->control = (control & ~TRB_CYCLE) | r->cycle;

    if (++r->enq == XHCI_RING_TRBS - 1) {
        /* A TD that continues across the link must have the link chained. */
        xhci_trb_t *link = &r->trbs[r->enq];
        barrier();
        *(volatile u32 *)&link->control = TRB_TYPE(TRB_LINK) | TRB_TC | (control & TRB_CH) | r->cycle;
        r->cycle ^= 1;
        r->enq = 0;
    }
    return phys;
}

static inline u64 ring_enqueue_ptr(const xhci_ring_t *r)
{
    return (r->phys + (u64)r->enq * sizeof(xhci_trb_t)) | r->cycle;
}

static inline void ring_doorbell(xhci_hc_t *hc, u32 slot, u32 target)
{
    barrier();
    wr32(hc->db, slot * 4, target);
}

/* ── Contexts ─────────────────────────────────────────────────────────────── */

static inline u32 *in_ctrl(xhci_slot_t *s)          { return (u32 *)s->in_ctx; }
static inline u32 *in_slot(xhci_slot_t *s)          { return (u32 *)(s->in_ctx + s->hc->ctx_size); }
static inline u32 *in_ep(xhci_slot_t *s, u32 dci)   { return (u32 *)(s->in_ctx + s->hc->ctx_size * (dci + 1)); }
static inline u32 *out_slot(xhci_slot_t *s)         { return (u32 *)s->out_ctx; }
static inline u32 *out_ep(xhci_slot_t *s, u32 dci)  { return (u32 *)(s->out_ctx + s->hc->ctx_size * dci); }

static void ep_ctx_set_dequeue(u32 *ep, u64 deq)
{
    ep[2] = (u32)deq;
    ep[3] = (u32)(deq >> 32);
}

/* ── Event ring ───────────────────────────────────────────────────────────── */

static void handle_transfer_event(xhci_hc_t *hc, const xhci_trb_t *e)
{
    u32 slot_id = TRB_GET_SLOT(e->control), dci = TRB_GET_EP(e->control);
    u32 cc = TRB_GET_CC(e->status), residual = TRB_GET_LEN(e->status);
    xhci_slot_t *s = hc->slots[slot_id];
    if (!s || dci == 0 || dci > 31) return;

    if (dci == 1) {
        if (s->ctrl_data_trb && e->param == s->ctrl_data_trb) {
            /* Data stage: only reported when short (ISP) or failed. A
             * failure halts the endpoint, so no status-stage event follows. */
            s->ctrl_residual = residual;
            if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
                s->ctrl_cc   = (u8)cc;
                s->ctrl_done = true;
            }
        } else if (!s->ctrl_done) {
            s->ctrl_cc   = (u8)(cc == CC_SHORT_PACKET ? CC_SUCCESS : cc);
            s->ctrl_done = true;
        }
        return;
    }

    xhci_ep_t *ep = &s->ep[dci];
    if (!ep->active) return;
    if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
        u32 got = residual <= ep->mps ? ep->mps - residual : 0;
        if (ep->cb) ep->cb(s->udev, ep->cb_ctx, ep->buf, got);
        if (ep->active) {
            ring_push(&ep->ring, ep->buf_phys, ep->mps,
                      TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
            ring_doorbell(hc, s->id, dci);
        }
    } else {
        /* The endpoint halted; recovery issues commands, which cannot be
         * done from inside event processing — the thread does it. */
        ep->needs_reset = true;
    }
}

static void handle_event(xhci_hc_t *hc, const xhci_trb_t *e)
{
    switch (TRB_GET_TYPE(e->control)) {
    case TRB_EV_TRANSFER:
        handle_transfer_event(hc, e);
        break;
    case TRB_EV_COMMAND:
        if (e->param == hc->cmd_trb) {
            hc->cmd_cc   = (u8)TRB_GET_CC(e->status);
            hc->cmd_slot = (u8)TRB_GET_SLOT(e->control);
            hc->cmd_done = true;
        }
        break;
    case TRB_EV_PORT_STATUS: {
        u32 port = (u32)(e->param >> 24) & 0xFF;
        if (port >= 1 && port <= hc->max_ports) hc->port_event[port] = true;
        break;
    }
    case TRB_EV_HOST:
        pr_debug("[XHCI] %s: host controller event, code %u\n",
                 hc->name, TRB_GET_CC(e->status));
        break;
    default:
        break;
    }
}

static void xhci_poll_events(xhci_hc_t *hc)
{
    bool consumed = false;
    for (;;) {
        xhci_trb_t *e = &hc->evt[hc->evt_deq];
        u32 control = *(volatile u32 *)&e->control;
        if ((control & TRB_CYCLE) != hc->evt_cycle) break;
        barrier();
        handle_event(hc, e);
        if (++hc->evt_deq == XHCI_RING_TRBS) {
            hc->evt_deq = 0;
            hc->evt_cycle ^= 1;
        }
        consumed = true;
    }
    if (consumed) {
        wr64(hc->rt, XHCI_RT_IR0 + XHCI_IR_ERDP,
             (hc->evt_phys + (u64)hc->evt_deq * sizeof(xhci_trb_t)) | ERDP_EHB);
    }

    u32 sts = rd32(hc->op, XHCI_OP_USBSTS);
    if (!hc->dead && (sts == 0xFFFFFFFFU || (sts & (USBSTS_HSE | USBSTS_HCE)))) {
        hc->dead = true;
        pr_debug("[XHCI] %s: controller stopped (USBSTS=0x%08x); detaching\n", hc->name, sts);
    }
}

/* Process events until *flag is set (by an event handler) or time runs out. */
static int xhci_wait(xhci_hc_t *hc, volatile bool *flag, u32 timeout_ms)
{
    u64 deadline = ktime_get_ns() + (u64)timeout_ms * 1000000ULL;
    for (;;) {
        xhci_poll_events(hc);
        if (*flag) return 0;
        if (hc->dead) return -EIO;
        u64 now = ktime_get_ns();
        if (now >= deadline) return -ETIMEDOUT;
        sched_sleep_until_ns(now + 100000ULL);            /* 100 µs */
    }
}

/* ── Commands ─────────────────────────────────────────────────────────────── */

static int xhci_command(xhci_hc_t *hc, u64 param, u32 control, u8 *slot_out)
{
    hc->cmd_done = false;
    hc->cmd_trb  = ring_push(&hc->cmd, param, 0, control);
    ring_doorbell(hc, 0, 0);

    int rc = xhci_wait(hc, &hc->cmd_done, XHCI_CMD_TIMEOUT_MS);
    if (rc < 0) {
        pr_debug("[XHCI] %s: command type %u timed out\n", hc->name, TRB_GET_TYPE(control));
        return rc;
    }
    if (slot_out) *slot_out = hc->cmd_slot;
    if (hc->cmd_cc != CC_SUCCESS) {
        pr_debug("[XHCI] %s: command type %u failed, completion code %u\n",
                 hc->name, TRB_GET_TYPE(control), hc->cmd_cc);
        return -EIO;
    }
    return 0;
}

/* Bring a halted (or, with @stop, a running) endpoint back to a usable state
 * with its dequeue pointer past everything queued so far. */
static int xhci_ep_recover(xhci_slot_t *s, u32 dci, bool stop)
{
    xhci_hc_t *hc = s->hc;
    if (stop) xhci_command(hc, 0, TRB_TYPE(TRB_STOP_EP) | TRB_SLOT(s->id) | TRB_EP(dci), NULL);
    else      xhci_command(hc, 0, TRB_TYPE(TRB_RESET_EP) | TRB_SLOT(s->id) | TRB_EP(dci), NULL);
    return xhci_command(hc, ring_enqueue_ptr(&s->ep[dci].ring),
                        TRB_TYPE(TRB_SET_TR_DEQUEUE) | TRB_SLOT(s->id) | TRB_EP(dci), NULL);
}

/* ── USB core operations ──────────────────────────────────────────────────── */

static int xhci_op_control(usb_device_t *dev, const usb_setup_packet_t *setup, void *data)
{
    xhci_slot_t *s = (xhci_slot_t *)dev->hcd_priv;
    if (!s) return -ENODEV;
    xhci_hc_t *hc = s->hc;
    if (hc->dead) return -EIO;

    u16 len = setup->wLength;
    if (len > PAGE_SIZE) return -EINVAL;
    bool in = (setup->bmRequestType & USB_DIR_IN) != 0;
    if (!in && len) memcpy(s->ctrl_buf, data, len);

    xhci_ring_t *r = &s->ep[1].ring;
    u64 setup_word;
    memcpy(&setup_word, setup, sizeof(setup_word));
    u32 trt = len == 0 ? TRT_NO_DATA : in ? TRT_IN_DATA : TRT_OUT_DATA;

    s->ctrl_done     = false;
    s->ctrl_cc       = 0;
    s->ctrl_residual = 0;
    s->ctrl_data_trb = 0;

    ring_push(r, setup_word, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | TRB_TRT(trt));
    if (len)
        s->ctrl_data_trb = ring_push(r, s->ctrl_buf_phys, len,
                                     TRB_TYPE(TRB_DATA) | TRB_ISP | (in ? TRB_DIR_IN : 0));
    /* The status stage runs opposite to the data (IN when there is none). */
    ring_push(r, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | ((len && in) ? 0 : TRB_DIR_IN));
    ring_doorbell(hc, s->id, 1);

    int rc = xhci_wait(hc, &s->ctrl_done, XHCI_CTRL_TIMEOUT_MS);
    if (rc < 0) {
        xhci_ep_recover(s, 1, true);
        return rc;
    }
    if (s->ctrl_cc != CC_SUCCESS) {
        xhci_ep_recover(s, 1, false);
        return s->ctrl_cc == CC_STALL ? -EPIPE : -EIO;
    }
    u32 actual = s->ctrl_residual <= len ? len - s->ctrl_residual : 0;
    if (in && actual) memcpy(data, s->ctrl_buf, actual);
    return (int)actual;
}

static int xhci_op_set_ep0_mps(usb_device_t *dev, u16 mps)
{
    xhci_slot_t *s = (xhci_slot_t *)dev->hcd_priv;
    if (!s) return -ENODEV;
    if ((out_ep(s, 1)[1] >> 16) == mps) return 0;

    memset(s->in_ctx, 0, PAGE_SIZE);
    in_ctrl(s)[1] = 1U << 1;                         /* evaluate EP0 */
    memcpy(in_ep(s, 1), out_ep(s, 1), s->hc->ctx_size);
    in_ep(s, 1)[1] = (in_ep(s, 1)[1] & 0xFFFF) | EP_CTX_MPS(mps);
    return xhci_command(s->hc, s->in_ctx_phys,
                        TRB_TYPE(TRB_EVALUATE_CONTEXT) | TRB_SLOT(s->id), NULL);
}

/* Endpoint context Interval: the period is 2^Interval x 125 µs. Full/low
 * speed descriptors give bInterval in 1 ms frames; high speed and faster
 * already give an exponent (bInterval - 1). */
static u32 xhci_interval(u8 xspeed, u8 b_interval)
{
    u32 iv;
    if (xspeed == XHCI_SPEED_HIGH || xspeed >= XHCI_SPEED_SUPER) {
        iv = b_interval ? (u32)b_interval - 1 : 0;
        if (iv > 15) iv = 15;
    } else {
        u32 microframes = (b_interval ? b_interval : 1) * 8U;
        iv = 0;
        while ((2U << iv) <= microframes) iv++;
        if (iv < 3)  iv = 3;
        if (iv > 10) iv = 10;
    }
    return iv;
}

static int xhci_op_intr_in(usb_device_t *dev, const usb_endpoint_descriptor_t *epd,
                           usb_intr_cb_t cb, void *ctx)
{
    xhci_slot_t *s = (xhci_slot_t *)dev->hcd_priv;
    if (!s) return -ENODEV;
    xhci_hc_t *hc = s->hc;

    u32 dci = (u32)(epd->bEndpointAddress & USB_ENDPOINT_NUM_MASK) * 2 + 1;
    if (dci < 3 || dci > 31) return -EINVAL;
    xhci_ep_t *ep = &s->ep[dci];
    if (ep->active) return -EBUSY;

    u16 wmps  = epd->wMaxPacketSize;
    u16 mps   = wmps & 0x7FF;
    u32 burst = s->xspeed == XHCI_SPEED_HIGH ? (wmps >> 11) & 3 : 0;
    if (!mps) return -EINVAL;

    if (ring_init(hc, &ep->ring) < 0) return -ENOMEM;
    ep->buf = (u8 *)dma_page(hc, &ep->buf_phys);
    if (!ep->buf) { ring_free(&ep->ring); return -ENOMEM; }

    memset(s->in_ctx, 0, PAGE_SIZE);
    in_ctrl(s)[1] = (1U << 0) | (1U << dci);
    memcpy(in_slot(s), out_slot(s), hc->ctx_size);
    u32 entries = (in_slot(s)[0] & SLOT_CTX_ENTRIES_MASK) >> 27;
    if (dci > entries)
        in_slot(s)[0] = (in_slot(s)[0] & ~SLOT_CTX_ENTRIES_MASK) | SLOT_CTX_ENTRIES(dci);

    u32 *e = in_ep(s, dci);
    u32 esit = (u32)mps * (burst + 1);
    e[0] = EP_CTX_INTERVAL(xhci_interval(s->xspeed, epd->bInterval));
    e[1] = EP_CTX_CERR(3) | EP_CTX_TYPE(EP_TYPE_INT_IN) | EP_CTX_BURST(burst) | EP_CTX_MPS(mps);
    ep_ctx_set_dequeue(e, ep->ring.phys | 1);
    e[4] = EP_CTX_AVG_TRB(mps) | EP_CTX_ESIT_LO(esit);

    int rc = xhci_command(hc, s->in_ctx_phys, TRB_TYPE(TRB_CONFIGURE_EP) | TRB_SLOT(s->id), NULL);
    if (rc < 0) {
        dma_free(ep->buf_phys);
        ep->buf = NULL;
        ring_free(&ep->ring);
        return rc;
    }

    ep->mps    = mps;
    ep->cb     = cb;
    ep->cb_ctx = ctx;
    ep->active = true;
    ring_push(&ep->ring, ep->buf_phys, mps, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    ring_doorbell(hc, s->id, dci);
    return 0;
}

static const usb_hcd_ops_t g_xhci_ops = {
    .control     = xhci_op_control,
    .set_ep0_mps = xhci_op_set_ep0_mps,
    .intr_in     = xhci_op_intr_in,
};

/* ── Devices ──────────────────────────────────────────────────────────────── */

static u8 xhci_to_usb_speed(u8 xspeed)
{
    switch (xspeed) {
    case XHCI_SPEED_LOW:  return USB_SPEED_LOW;
    case XHCI_SPEED_FULL: return USB_SPEED_FULL;
    case XHCI_SPEED_HIGH: return USB_SPEED_HIGH;
    default:              return USB_SPEED_SUPER;
    }
}

/* EP0's packet size until the device descriptor says otherwise. */
static u16 xhci_default_mps0(u8 xspeed)
{
    switch (xspeed) {
    case XHCI_SPEED_HIGH:  return 64;
    case XHCI_SPEED_SUPER:
    case XHCI_SPEED_SUPER_PLUS: return 512;
    default:               return 8;
    }
}

static void xhci_slot_free(xhci_hc_t *hc, xhci_slot_t *s)
{
    for (u32 d = 1; d < 32; d++) {
        s->ep[d].active = false;
        if (s->ep[d].ring.trbs) ring_free(&s->ep[d].ring);
        if (s->ep[d].buf) dma_free(s->ep[d].buf_phys);
    }
    if (hc->slots[s->id] == s) {
        xhci_command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(s->id), NULL);
        hc->dcbaa[s->id] = 0;
        hc->slots[s->id] = NULL;
    }
    dma_free(s->in_ctx_phys);
    dma_free(s->out_ctx_phys);
    dma_free(s->ctrl_buf_phys);
    kfree(s);
}

/* Reset @port and report whether it came out enabled. USB 2 ports need an
 * explicit reset to enable; USB 3 ports enable themselves once link training
 * finishes, and only get a (warm) reset if that does not happen. */
static bool xhci_port_reset(xhci_hc_t *hc, u32 port)
{
    u32 sc = portsc(hc, port);
    if (hc->port_major[port] == 3) {
        if (wait_reg(hc->op, XHCI_OP_PORTSC(port), PORTSC_PED, PORTSC_PED, 300)) return true;
        portsc_write(hc, port, (sc & PORTSC_PRESERVE) | PORTSC_WPR);
    } else {
        portsc_write(hc, port, (sc & PORTSC_PRESERVE) | PORTSC_PR);
    }
    wait_reg(hc->op, XHCI_OP_PORTSC(port), PORTSC_PRC, PORTSC_PRC, 500);
    sc = portsc(hc, port);
    portsc_write(hc, port, (sc & PORTSC_PRESERVE) | (sc & PORTSC_CHANGE_MASK));
    return (sc & PORTSC_PED) != 0;
}

static void xhci_attach(xhci_hc_t *hc, u32 port)
{
    if (!xhci_port_reset(hc, port)) {
        pr_debug("[XHCI] %s port %u: did not enable after reset\n", hc->name, port);
        return;
    }
    xhci_sleep_ms(10);                               /* TRSTRCY, USB 2.0 7.1.7.5 */
    u32 sc = portsc(hc, port);
    if (!(sc & PORTSC_CCS)) return;
    u8 xspeed = (u8)PORTSC_SPEED(sc);

    u8 slot_id = 0;
    if (xhci_command(hc, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot_id) < 0 ||
        slot_id == 0 || slot_id > hc->max_slots) {
        pr_debug("[XHCI] %s port %u: no device slot available\n", hc->name, port);
        return;
    }

    xhci_slot_t *s = (xhci_slot_t *)kzalloc(sizeof(xhci_slot_t));
    if (!s) {
        xhci_command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(slot_id), NULL);
        return;
    }
    s->hc = hc;
    s->id = slot_id;
    s->port = (u8)port;
    s->xspeed = xspeed;
    hc->slots[slot_id] = s;
    s->in_ctx   = (u8 *)dma_page(hc, &s->in_ctx_phys);
    s->out_ctx  = (u8 *)dma_page(hc, &s->out_ctx_phys);
    s->ctrl_buf = (u8 *)dma_page(hc, &s->ctrl_buf_phys);
    if (!s->in_ctx || !s->out_ctx || !s->ctrl_buf || ring_init(hc, &s->ep[1].ring) < 0) {
        xhci_slot_free(hc, s);
        return;
    }
    hc->dcbaa[slot_id] = s->out_ctx_phys;

    /* Address Device: slot context plus the default control endpoint. The
     * controller picks the USB address and sends SET_ADDRESS itself. */
    in_ctrl(s)[1] = (1U << 0) | (1U << 1);
    in_slot(s)[0] = SLOT_CTX_SPEED(xspeed) | SLOT_CTX_ENTRIES(1);
    in_slot(s)[1] = SLOT_CTX_PORT(port);
    u32 *ep0 = in_ep(s, 1);
    ep0[1] = EP_CTX_CERR(3) | EP_CTX_TYPE(EP_TYPE_CONTROL) | EP_CTX_MPS(xhci_default_mps0(xspeed));
    ep_ctx_set_dequeue(ep0, s->ep[1].ring.phys | 1);
    ep0[4] = EP_CTX_AVG_TRB(8);
    if (xhci_command(hc, s->in_ctx_phys, TRB_TYPE(TRB_ADDRESS_DEVICE) | TRB_SLOT(slot_id), NULL) < 0) {
        pr_debug("[XHCI] %s port %u: Address Device failed\n", hc->name, port);
        xhci_slot_free(hc, s);
        return;
    }

    usb_device_t *udev = usb_device_create(&hc->bus, (u8)port, xhci_to_usb_speed(xspeed));
    if (!udev) {
        xhci_slot_free(hc, s);
        return;
    }
    udev->hcd_priv = s;
    udev->address  = (u8)(out_slot(s)[3] & 0xFF);
    s->udev = udev;
    hc->port_slot[port] = s;

    if (usb_new_device(udev) < 0) {
        pr_debug("[XHCI] %s port %u: enumeration failed\n", hc->name, port);
        hc->port_slot[port] = NULL;
        usb_disconnect(udev);
        s->udev = NULL;
        xhci_slot_free(hc, s);
    }
}

static void xhci_detach(xhci_hc_t *hc, u32 port)
{
    xhci_slot_t *s = hc->port_slot[port];
    if (!s) return;
    hc->port_slot[port] = NULL;
    for (u32 d = 1; d < 32; d++) s->ep[d].active = false;
    if (s->udev) usb_disconnect(s->udev);
    s->udev = NULL;
    xhci_slot_free(hc, s);
}

static void xhci_port_check(xhci_hc_t *hc, u32 port)
{
    u32 sc = portsc(hc, port);
    if (sc == 0xFFFFFFFFU) return;
    if (sc & PORTSC_CHANGE_MASK)
        portsc_write(hc, port, (sc & PORTSC_PRESERVE) | (sc & PORTSC_CHANGE_MASK));

    bool connected = (sc & PORTSC_CCS) != 0;
    /* A connect-status change on an occupied port is an unplug followed by a
     * plug that happened between two looks: start over with the new device. */
    if (hc->port_slot[port] && (!connected || (sc & PORTSC_CSC)))
        xhci_detach(hc, port);
    if (connected && !hc->port_slot[port])
        xhci_attach(hc, port);
}

static void xhci_recover_endpoints(xhci_hc_t *hc)
{
    for (u32 id = 1; id <= hc->max_slots; id++) {
        xhci_slot_t *s = hc->slots[id];
        if (!s) continue;
        for (u32 d = 2; d < 32; d++) {
            xhci_ep_t *ep = &s->ep[d];
            if (!ep->active || !ep->needs_reset) continue;
            ep->needs_reset = false;
            if (xhci_ep_recover(s, d, false) == 0) {
                ring_push(&ep->ring, ep->buf_phys, ep->mps, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
                ring_doorbell(hc, s->id, d);
            }
        }
    }
}

static void xhci_thread(void *arg)
{
    xhci_hc_t *hc = (xhci_hc_t *)arg;

    /* Devices plugged in before the controller was reset may not raise a
     * port-status event at all; look at every port once. */
    for (u32 p = 1; p <= hc->max_ports; p++) hc->port_event[p] = true;

    for (;;) {
        xhci_poll_events(hc);
        if (hc->dead) {
            for (u32 p = 1; p <= hc->max_ports; p++) xhci_detach(hc, p);
            for (;;) sched_sleep(100);
        }
        for (u32 p = 1; p <= hc->max_ports; p++) {
            if (!hc->port_event[p]) continue;
            hc->port_event[p] = false;
            xhci_port_check(hc, p);
        }
        xhci_recover_endpoints(hc);

        u64 now = ktime_get_ns();
        usb_bus_poll(&hc->bus, now);
        sched_sleep_until_ns(now + (hc->bus.devices ? 2000000ULL : 20000000ULL));
    }
}

/* ── Controller bring-up ──────────────────────────────────────────────────── */

/* Ask the firmware to let go (xHCI 4.22.1), then make sure no SMI fires for
 * this controller again, whether or not the firmware cooperated. */
static void xhci_bios_handoff(xhci_hc_t *hc, u32 off)
{
    u32 v = rd32(hc->cap, off);
    if (v & LEGACY_BIOS_OWNED) {
        wr32(hc->cap, off, v | LEGACY_OS_OWNED);
        if (!wait_reg(hc->cap, off, LEGACY_BIOS_OWNED, 0, 1000)) {
            pr_debug("[XHCI] %s: firmware did not release the controller; taking it\n", hc->name);
            wr32(hc->cap, off, (rd32(hc->cap, off) & ~LEGACY_BIOS_OWNED) | LEGACY_OS_OWNED);
        }
    }
    u32 ctl = rd32(hc->cap, off + 4);
    wr32(hc->cap, off + 4, (ctl & ~LEGCTL_SMI_ENABLES) | LEGCTL_SMI_EVENTS);
}

static void xhci_scan_capabilities(xhci_hc_t *hc)
{
    u32 hcc1 = rd32(hc->cap, XHCI_CAP_HCCPARAMS1);
    u32 off = HCC1_XECP(hcc1) << 2;
    for (u32 guard = 0; off && guard < 64; guard++) {
        u32 v = rd32(hc->cap, off);
        if (XECP_ID(v) == XECP_LEGACY) {
            xhci_bios_handoff(hc, off);
        } else if (XECP_ID(v) == XECP_PROTOCOL) {
            u8 major = (u8)(v >> 24);
            u32 ports = rd32(hc->cap, off + 8);
            u32 first = ports & 0xFF, count = (ports >> 8) & 0xFF;
            for (u32 p = first; p < first + count && p <= hc->max_ports; p++)
                if (p) hc->port_major[p] = major;
        }
        u32 next = XECP_NEXT(v);
        off = next ? off + (next << 2) : 0;
    }
}

/* Intel 7/8/9-series PCHs power up with their ports on the EHCI controller;
 * setting the xHCI routing registers moves every port the platform allows
 * (the mask registers) over to xHCI. Linux does the same for these parts. */
static void xhci_intel_route_ports(xhci_hc_t *hc)
{
    static const u16 switchable[] = { 0x1E31, 0x8C31, 0x9C31, 0x8CB1, 0x9CB1 };
    pci_device_info_t *p = hc->pci;
    if (p->vendor_id != 0x8086) return;
    bool match = false;
    for (size_t i = 0; i < sizeof(switchable) / sizeof(switchable[0]); i++)
        if (p->device_id == switchable[i]) match = true;
    if (!match) return;

    u32 usb3_mask = pci_config_read32(p->bus, p->slot, p->func, 0xDC);   /* USB3PRM */
    pci_config_write32(p->bus, p->slot, p->func, 0xD8, usb3_mask);        /* USB3_PSSEN */
    u32 usb2_mask = pci_config_read32(p->bus, p->slot, p->func, 0xD4);   /* XUSB2PRM */
    pci_config_write32(p->bus, p->slot, p->func, 0xD0, usb2_mask);        /* XUSB2PR */
    pr_debug("[XHCI] %s: Intel PCH ports routed to xHCI (USB3 0x%x, USB2 0x%x)\n",
             hc->name, usb3_mask, usb2_mask);
}

static int xhci_setup_scratchpad(xhci_hc_t *hc)
{
    u32 n = HCS2_MAX_SCRATCH(rd32(hc->cap, XHCI_CAP_HCSPARAMS2));
    if (n == 0) return 0;
    if (n > PAGE_SIZE / sizeof(u64)) return -ENOMEM;
    phys_addr_t arr_phys;
    u64 *arr = (u64 *)dma_page(hc, &arr_phys);
    if (!arr) return -ENOMEM;
    for (u32 i = 0; i < n; i++) {
        phys_addr_t pg;
        if (!dma_page(hc, &pg)) return -ENOMEM;
        arr[i] = pg;
    }
    hc->dcbaa[0] = arr_phys;
    return 0;
}

static int xhci_hw_init(xhci_hc_t *hc)
{
    u32 hcs1 = rd32(hc->cap, XHCI_CAP_HCSPARAMS1);
    u32 hcc1 = rd32(hc->cap, XHCI_CAP_HCCPARAMS1);
    hc->max_ports = HCS1_MAX_PORTS(hcs1);
    hc->max_slots = HCS1_MAX_SLOTS(hcs1);
    if (hc->max_slots > XHCI_SLOTS_LIMIT) hc->max_slots = XHCI_SLOTS_LIMIT;
    hc->ctx_size  = (hcc1 & HCC1_CSZ) ? 64 : 32;
    hc->ac64      = (hcc1 & HCC1_AC64) != 0;

    xhci_intel_route_ports(hc);
    xhci_scan_capabilities(hc);

    /* Halt, then reset (4.22.1). */
    wr32(hc->op, XHCI_OP_USBCMD, rd32(hc->op, XHCI_OP_USBCMD) & ~USBCMD_RS);
    if (!wait_reg(hc->op, XHCI_OP_USBSTS, USBSTS_HCH, USBSTS_HCH, 100)) return -EIO;
    wr32(hc->op, XHCI_OP_USBCMD, USBCMD_HCRST);
    if (!wait_reg(hc->op, XHCI_OP_USBCMD, USBCMD_HCRST, 0, 1000)) return -EIO;
    if (!wait_reg(hc->op, XHCI_OP_USBSTS, USBSTS_CNR, 0, 1000)) return -EIO;

    wr32(hc->op, XHCI_OP_CONFIG, (rd32(hc->op, XHCI_OP_CONFIG) & ~0xFFU) | hc->max_slots);

    hc->dcbaa = (u64 *)dma_page(hc, &hc->dcbaa_phys);
    if (!hc->dcbaa) return -ENOMEM;
    if (xhci_setup_scratchpad(hc) < 0) return -ENOMEM;
    wr64(hc->op, XHCI_OP_DCBAAP, hc->dcbaa_phys);

    if (ring_init(hc, &hc->cmd) < 0) return -ENOMEM;
    wr64(hc->op, XHCI_OP_CRCR, hc->cmd.phys | CRCR_RCS);

    /* One event ring segment, polled; the interrupter stays disabled. */
    hc->evt  = (xhci_trb_t *)dma_page(hc, &hc->evt_phys);
    hc->erst = (u64 *)dma_page(hc, &hc->erst_phys);
    if (!hc->evt || !hc->erst) return -ENOMEM;
    hc->erst[0] = hc->evt_phys;
    hc->erst[1] = XHCI_RING_TRBS;                   /* size, then reserved dword */
    hc->evt_deq   = 0;
    hc->evt_cycle = 1;
    volatile u8 *ir = hc->rt + XHCI_RT_IR0;
    wr32(ir, XHCI_IR_ERSTSZ, 1);
    wr64(ir, XHCI_IR_ERDP, hc->evt_phys);
    wr64(ir, XHCI_IR_ERSTBA, hc->erst_phys);       /* last: enables the ring */
    wr32(ir, XHCI_IR_IMAN, IMAN_IP);                /* clear pending, IE = 0 */

    wr32(hc->op, XHCI_OP_USBCMD, USBCMD_RS);
    if (!wait_reg(hc->op, XHCI_OP_USBSTS, USBSTS_HCH, 0, 100)) return -EIO;

    /* With port power control, ports come up unpowered. */
    if (hcc1 & HCC1_PPC) {
        for (u32 p = 1; p <= hc->max_ports; p++) {
            u32 sc = portsc(hc, p);
            if (!(sc & PORTSC_PP)) portsc_write(hc, p, (sc & PORTSC_PRESERVE) | PORTSC_PP);
        }
        xhci_sleep_ms(20);
    }
    for (u32 p = 1; p <= hc->max_ports; p++)
        if (!hc->port_major[p]) hc->port_major[p] = 2;
    return 0;
}

static int xhci_pci_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;
    pci_device_info_t *pci = to_pci_info(dm);
    if (!pci) return -ENODEV;
    if (g_hc_count >= XHCI_MAX_CONTROLLERS) return -ENOSPC;

    pci_enable_bus_mastering(dm->hal);
    phys_addr_t bar = pci_get_bar(dm->hal, 0);
    if (!bar) return -ENXIO;

    xhci_hc_t *hc = (xhci_hc_t *)kzalloc(sizeof(xhci_hc_t));
    if (!hc) return -ENOMEM;
    hc->index = g_hc_count;
    hc->dm    = dm;
    hc->pci   = pci;
    snprintf(hc->name, sizeof(hc->name), "usb%d", hc->index + 1);

    /* Map the capability page, learn where everything else lives, then map
     * the whole register window those offsets span. */
    phys_addr_t base_pg = ALIGN_DOWN(bar, PAGE_SIZE);
    virt_addr_t va = (virt_addr_t)PHYS_TO_VIRT(base_pg);
    vmm_map(0, va, base_pg, VMM_MMIO);
    hc->cap = (volatile u8 *)(va + (bar - base_pg));

    /* CAPLENGTH and HCIVERSION share the first dword; read it whole, since
     * not every implementation (QEMU's included) answers narrower reads. */
    u32 cap0   = rd32(hc->cap, XHCI_CAP_CAPLENGTH);
    u32 caplen = cap0 & 0xFF;
    u32 hcs1   = rd32(hc->cap, XHCI_CAP_HCSPARAMS1);
    u32 dboff  = rd32(hc->cap, XHCI_CAP_DBOFF) & ~0x3U;
    u32 rtsoff = rd32(hc->cap, XHCI_CAP_RTSOFF) & ~0x1FU;
    u32 span = caplen + 0x400 + 0x10 * HCS1_MAX_PORTS(hcs1);
    if (dboff + 4 * 256 > span) span = dboff + 4 * 256;
    if (rtsoff + XHCI_RT_IR0 + 0x20 > span) span = rtsoff + XHCI_RT_IR0 + 0x20;
    u32 xecp = HCC1_XECP(rd32(hc->cap, XHCI_CAP_HCCPARAMS1)) << 2;
    if (xecp && xecp + 0x1000 > span) span = xecp + 0x1000;   /* extended caps */
    if (span > 0x100000) span = 0x100000;
    for (u32 off = PAGE_SIZE; off < span + (bar - base_pg); off += PAGE_SIZE)
        vmm_map(0, va + off, base_pg + off, VMM_MMIO);

    hc->op = hc->cap + caplen;
    hc->rt = hc->cap + rtsoff;
    hc->db = hc->cap + dboff;

    u16 version = (u16)(cap0 >> 16);
    int rc = xhci_hw_init(hc);
    if (rc < 0) {
        pr_debug("[XHCI] %02x:%02x.%x: initialisation failed (%d)\n",
                 pci->bus, pci->slot, pci->func, rc);
        /* Leaves its DMA pages allocated: after a failed reset the
         * controller may still hold their addresses. */
        return rc;
    }

    hc->bus.name    = hc->name;
    hc->bus.bus_num = hc->index + 1;
    hc->bus.hcd     = hc;
    hc->bus.dev     = dm;
    hc->bus.ops     = &g_xhci_ops;
    g_hcs[g_hc_count++] = hc;
    dm_set_drvdata(dm, hc);

    pr_debug("[XHCI] %s: xHCI %x.%02x at %02x:%02x.%x (%04x:%04x), %u ports, %u slots, %u-byte contexts%s\n",
             hc->name, version >> 8, version & 0xFF, pci->bus, pci->slot, pci->func,
             pci->vendor_id, pci->device_id, hc->max_ports, hc->max_slots, hc->ctx_size,
             hc->ac64 ? "" : ", 32-bit DMA");

    thread_create(sched_kernel_process(), (uintptr_t)xhci_thread, (uintptr_t)hc, true);
    return 0;
}

static const pci_device_id_t g_xhci_pci_ids[] = {
    { PCI_DEVICE_CLASS(0x0C0330, 0xFFFFFF) },   /* serial bus / USB / xHCI */
    { 0 }
};

static pci_driver_t g_xhci_pci_driver = {
    .drv      = { .name = "xhci_hcd" },
    .id_table = g_xhci_pci_ids,
    .probe    = xhci_pci_probe,
    .remove   = NULL,
};

int xhci_init(void)
{
    usb_core_init();
    return pci_driver_register(&g_xhci_pci_driver);
}
