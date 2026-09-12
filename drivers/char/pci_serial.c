/* ============================================================================
 * AzamiOS — PCI Multi-Port Serial (16550A) Driver
 * File: drivers/char/pci_serial.c
 *
 * Drives PCI serial adapter cards (Red Hat QEMU PCI Serial, NetMos NM9835,
 * Oxford Semiconductor OX16PCI954, and generic 16550A PCI communication controllers).
 * Automatically initializes 16550A FIFOs, 115200 8N1 baud, and registers
 * /dev/ttyS4, /dev/ttyS5, etc.
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "pci_serial.h"
#include "../base/pci_bus.h"
#include "../../fs/vfs.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/cpu/spinlock.h"

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

#define UART_RBR 0
#define UART_THR 0
#define UART_IER 1
#define UART_IIR 2
#define UART_FCR 2
#define UART_LCR 3
#define UART_MCR 4
#define UART_LSR 5
#define UART_MSR 6
#define UART_SCR 7

#define UART_DLL 0
#define UART_DLH 1

#define UART_LSR_DR   0x01
#define UART_LSR_THRE 0x20

#define MAX_PCI_SERIAL_PORTS 8

typedef struct pci_serial_port {
    u16  io_base;
    u8   irq;
    char dev_name[16];
    spinlock_t lock;
    file_operations_t fops;
} pci_serial_port_t;

static pci_serial_port_t g_serial_ports[MAX_PCI_SERIAL_PORTS];
static u32 g_serial_port_count = 0;

static void uart_init_hardware(u16 base)
{
    outb(base + UART_IER, 0x00);                /* Disable interrupts */
    outb(base + UART_LCR, 0x80);                /* Enable DLAB */
    outb(base + UART_DLL, 0x01);                /* 115200 baud divisor low */
    outb(base + UART_DLH, 0x00);                /* divisor high */
    outb(base + UART_LCR, 0x03);                /* 8 bits, no parity, 1 stop bit (8N1) */
    outb(base + UART_FCR, 0xC7);                /* Enable FIFO, clear TX/RX, 14-byte threshold */
    outb(base + UART_MCR, 0x0B);                /* DTR, RTS, OUT2 */
}

static s64 pci_serial_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    (void)offset;
    pci_serial_port_t *port = (filp && filp->f_inode) ? (pci_serial_port_t *)filp->f_inode->i_private : NULL;
    if (!port || !port->io_base || !buf || len == 0) return 0;

    u8 *p = (u8 *)buf;
    size_t count = 0;

    irqflags_t flags = spinlock_lock_irqsave(&port->lock);
    while (count < len) {
        if (inb(port->io_base + UART_LSR) & UART_LSR_DR) {
            p[count++] = inb(port->io_base + UART_RBR);
        } else {
            break; /* No more data ready */
        }
    }
    spinlock_unlock_irqrestore(&port->lock, flags);

    return (s64)count;
}

static s64 pci_serial_write(struct file *filp, const void *buf, size_t len, u64 *offset)
{
    (void)offset;
    pci_serial_port_t *port = (filp && filp->f_inode) ? (pci_serial_port_t *)filp->f_inode->i_private : NULL;
    if (!port || !port->io_base || !buf || len == 0) return 0;

    const u8 *p = (const u8 *)buf;
    irqflags_t flags = spinlock_lock_irqsave(&port->lock);

    for (size_t i = 0; i < len; i++) {
        for (int retry = 0; retry < 10000; retry++) {
            if (inb(port->io_base + UART_LSR) & UART_LSR_THRE) {
                break;
            }
            cpu_pause();
        }
        outb(port->io_base + UART_THR, p[i]);
    }

    spinlock_unlock_irqrestore(&port->lock, flags);
    return (s64)len;
}

static int pci_serial_probe(dm_device_t *dev, const pci_device_id_t *id)
{
    (void)id;
    pci_device_info_t *info = to_pci_info(dev);
    if (!info) return -ENODEV;

    u32 ports_added = 0;

    for (int bar_idx = 0; bar_idx < 6; bar_idx++) {
        if (g_serial_port_count >= MAX_PCI_SERIAL_PORTS) break;
        u32 raw_bar = info->bar[bar_idx];
        if (!(raw_bar & 1)) continue; /* Must be I/O space */

        u16 io_base = (u16)(raw_bar & ~0x3);
        if (io_base == 0) continue;

        pci_serial_port_t *port = &g_serial_ports[g_serial_port_count];
        port->io_base = io_base;
        port->irq = info->interrupt_line;
        spinlock_init(&port->lock);
        uart_init_hardware(io_base);

        scnprintf(port->dev_name, sizeof(port->dev_name), "ttyS%u", 4 + g_serial_port_count);

        port->fops.read    = pci_serial_read;
        port->fops.write   = pci_serial_write;
        port->fops.open    = NULL;
        port->fops.release = NULL;
        port->fops.ioctl   = NULL;

        devfs_register_device(port->dev_name, &port->fops, port);

        pr_debug("[PCI_SERIAL] Port /dev/%s ready at I/O 0x%04X, IRQ %u (PCI %02x:%02x.%u)\n",
                 port->dev_name, port->io_base, port->irq,
                 info->bus, info->slot, info->func);

        g_serial_port_count++;
        ports_added++;
    }

    return (ports_added > 0) ? 0 : -ENODEV;
}

static const pci_device_id_t pci_serial_ids[] = {
    { PCI_DEVICE(0x1B36, 0x0002) },   /* QEMU PCI 16550A Serial (single) */
    { PCI_DEVICE(0x1B36, 0x0003) },   /* QEMU PCI 16550A Serial (dual)   */
    { PCI_DEVICE(0x1B36, 0x0004) },   /* QEMU PCI 16550A Serial (quad)   */
    { PCI_DEVICE(0x9710, 0x9835) },   /* NetMos NM9835 Dual Serial       */
    { PCI_DEVICE(0x1415, 0x9501) },   /* Oxford OX16PCI954 Serial        */
    { PCI_DEVICE_CLASS(0x070002, 0xFFFFFF) }, /* Generic 16550 Serial    */
    { 0 }
};

static pci_driver_t g_pci_serial_driver = {
    .drv      = { .name = "pci_serial" },
    .id_table = pci_serial_ids,
    .probe    = pci_serial_probe,
    .remove   = NULL,
};

void pci_serial_init(void)
{
    pci_driver_register(&g_pci_serial_driver);
}

u32 pci_serial_get_port_count(void)
{
    return g_serial_port_count;
}
