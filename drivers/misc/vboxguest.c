#include "vboxguest.h"
#include "../../hal/pci.h"
#include "../base/pci_bus.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../arch/x86_64/cpu/idt.h"
#include "../../hal/irq.h"
#include "../../drivers/input/input.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/types.h"

extern int kprintf(const char *fmt, ...);

static struct {
    u16 ioport;
    u8 irq;
    bool enabled;
} g_vbox;

/* Send a VMMDev request via the BAR0 I/O port. */
static void vboxguest_send_request(void *req)
{
    if (!g_vbox.ioport) return;
    phys_addr_t phys = VIRT_TO_PHYS(req);
    outl(g_vbox.ioport, (u32)phys);
}

static void vboxguest_irq_handler(pt_regs_t *r, void *ctx)
{
    (void)r; (void)ctx;

    /* Acknowledge events */
    vmmdev_req_ack_events_t *ack_req = kmalloc(sizeof(*ack_req));
    if (!ack_req) return;
    __builtin_memset(ack_req, 0, sizeof(*ack_req));
    ack_req->header.size = sizeof(*ack_req);
    ack_req->header.version = VMMDEV_VERSION;
    ack_req->header.request_type = VMMDEVREQ_ACKNOWLEDGE_EVENTS;
    ack_req->events = ~0U;
    
    vboxguest_send_request(ack_req);
    u32 events = ack_req->events;
    kfree(ack_req);

    if (events & VMMDEV_EVENT_MOUSE_POSITION_CHANGED) {
        vmmdev_req_mouse_status_t *mouse_req = kmalloc(sizeof(*mouse_req));
        if (!mouse_req) return;
        __builtin_memset(mouse_req, 0, sizeof(*mouse_req));
        mouse_req->header.size = sizeof(*mouse_req);
        mouse_req->header.version = VMMDEV_VERSION;
        mouse_req->header.request_type = VMMDEVREQ_GET_MOUSE_STATUS;
        
        vboxguest_send_request(mouse_req);
        
        if (mouse_req->header.rc == 0) {
            input_event_t evt = {
                .type = INPUT_EVENT_MOUSE_ABS,
                .mouse_dx = (s16)mouse_req->x,
                .mouse_dy = (s16)mouse_req->y,
                .mouse_buttons = 0,
                .timestamp = 0
            };
            input_inject(&evt);
        }
        kfree(mouse_req);
    }
}

static int vboxguest_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;
    pci_device_info_t *pci = to_pci_info(dm);
    if (!pci) return -ENODEV;
    
    device_t *node = dm->hal;

    kprintf("[VBOX] VirtualBox Guest Service found at PCI %02x:%02x.%x\n",
             pci->bus, pci->slot, pci->func);

    pci_enable_bus_mastering(node);

    g_vbox.ioport = (u16)(pci_get_bar(node, 0) & ~3);
    g_vbox.irq = pci->interrupt_line;

    if (!g_vbox.ioport || !g_vbox.irq) {
        kprintf("[VBOX] Missing I/O port or IRQ\n");
        return -ENODEV;
    }

    /* Report Guest Info */
    vmmdev_req_report_guest_info_t *info_req = kmalloc(sizeof(*info_req));
    if (info_req) {
        __builtin_memset(info_req, 0, sizeof(*info_req));
        info_req->header.size = sizeof(*info_req);
        info_req->header.version = VMMDEV_VERSION;
        info_req->header.request_type = VMMDEVREQ_REPORT_GUEST_INFO;
        info_req->os_type = 0x100; /* Linux */
        vboxguest_send_request(info_req);
        kfree(info_req);
    }

    /* Set Capabilities (Enable Absolute Pointer) */
    vmmdev_req_guest_caps_t *caps_req = kmalloc(sizeof(*caps_req));
    if (caps_req) {
        __builtin_memset(caps_req, 0, sizeof(*caps_req));
        caps_req->header.size = sizeof(*caps_req);
        caps_req->header.version = VMMDEV_VERSION;
        caps_req->header.request_type = VMMDEVREQ_SET_GUEST_CAPABILITIES;
        caps_req->capabilities = VMMDEV_GUEST_SUPPORTS_ABSOLUTE_POINTER;
        vboxguest_send_request(caps_req);
        kfree(caps_req);
    }

    /* Enable absolute mouse reporting */
    vmmdev_req_mouse_status_t *mouse_req = kmalloc(sizeof(*mouse_req));
    if (mouse_req) {
        __builtin_memset(mouse_req, 0, sizeof(*mouse_req));
        mouse_req->header.size = sizeof(*mouse_req);
        mouse_req->header.version = VMMDEV_VERSION;
        mouse_req->header.request_type = VMMDEVREQ_SET_MOUSE_STATUS;
        mouse_req->features = VMMDEV_MOUSE_GUEST_CAN_ABSOLUTE;
        vboxguest_send_request(mouse_req);
        kfree(mouse_req);
    }

    /* Register IRQ */
    idt_register_irq(g_vbox.irq + 32, vboxguest_irq_handler, NULL);
    hal_irq_enable(g_vbox.irq, g_vbox.irq + 32);

    g_vbox.enabled = true;
    kprintf("[VBOX] Initialized on I/O 0x%x, IRQ %u. Absolute pointer enabled.\n",
            g_vbox.ioport, g_vbox.irq);

    return 0;
}

static void vboxguest_remove(dm_device_t *dm)
{
    (void)dm;
    if (g_vbox.enabled) {
        hal_irq_disable(g_vbox.irq);
        idt_register_irq(g_vbox.irq + 32, NULL, NULL);
        g_vbox.enabled = false;
    }
}

static const pci_device_id_t vboxguest_ids[] = {
    { PCI_DEVICE(0x80EE, 0xCAFE) },
    { 0 }
};

static pci_driver_t vboxguest_driver = {
    .drv      = { .name = "vboxguest" },
    .id_table = vboxguest_ids,
    .probe    = vboxguest_probe,
    .remove   = vboxguest_remove,
};

void vboxguest_init(void)
{
    pci_driver_register(&vboxguest_driver);
}
