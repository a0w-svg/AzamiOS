/**
 * kernel/drivers/char/secure_uart.c — Hardened Concurrent UART Character Driver
 *
 * Security properties:
 *   - All read/write paths enforce explicit max_len / len bounds (no overrun).
 *   - Ring buffer indices are uint32_t masked with (SIZE-1); no pointer arithmetic.
 *   - TX ring and RX ring each protected by their own spinlock to avoid
 *     head-of-line blocking between producer (ISR) and consumer (kernel thread).
 *   - ioctl buf_len validated before any dereference; unknown commands → -EINVAL.
 *   - Hardware port set at probe time; the COM port base is never a raw
 *     user-supplied pointer.
 *
 * Concurrency model:
 *   - RX path:  ISR → rx_buf (rx_lock) → wq_wake_one(rx_wq)
 *               Reader → wq_wait_event(rx_wq) → rx_buf (rx_lock)
 *   - TX path:  Writer → tx_buf (tx_lock) → hw drain loop (tx_lock)
 *               (For IRQ-driven TX: ISR drains tx_buf and calls wq_wake_one(tx_wq))
 */

#include "secure_uart.h"
#include "../include/device.h"
#include "../include/wait_queue.h"
#include "../../arch/include/spinlock.h"
#include "../../klibc/include/stdio.h"
#include "../../klibc/include/string.h"

/* ── Inline port I/O helpers (avoids dependency on a separate header) ── */
static inline void suart_outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t suart_inb(uint16_t port) {
    uint8_t val;
    asm volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* ── LSR register bit masks ─────────────────────────────────────────── */
#define LSR_DATA_READY   0x01u   /* bit 0: RX data available            */
#define LSR_TX_EMPTY     0x20u   /* bit 5: TX holding register empty    */
#define LSR_FRAMING_ERR  0x08u   /* bit 3: framing error                */
#define LSR_PARITY_ERR   0x04u   /* bit 2: parity error                 */

/* ── UART register offsets (relative to base_port) ─────────────────── */
#define UART_DATA        0u
#define UART_IER         1u      /* interrupt enable register           */
#define UART_FCR         2u      /* FIFO control                        */
#define UART_LCR         3u      /* line control                        */
#define UART_MCR         4u      /* modem control                       */
#define UART_LSR         5u      /* line status                         */
#define UART_DLL         0u      /* divisor latch low  (DLAB=1)        */
#define UART_DLH         1u      /* divisor latch high (DLAB=1)        */

/* ── Static private state (one instance for COM1) ───────────────────── */
static suart_state_t g_uart_state;

/* ── Ring buffer helpers (lockless arithmetic; lock held by caller) ─── */

/* Returns number of bytes available to read from rx ring */
static inline uint32_t rx_available(const suart_state_t *s) {
    return (s->rx_head - s->rx_tail) & (SUART_RX_BUF_SIZE - 1u);
}
/* Returns free space in tx ring */
static inline uint32_t tx_free(const suart_state_t *s) {
    return (SUART_TX_BUF_SIZE - 1u) -
           ((s->tx_head - s->tx_tail) & (SUART_TX_BUF_SIZE - 1u));
}

/* ── Hardware probe / remove ─────────────────────────────────────────── */

static int suart_probe(driver_t *drv, device_t *dev) {
    (void)drv;
    suart_state_t *s = (suart_state_t*)dev->private_data;
    if (!s) return -1;

    uint16_t port = s->base_port;

    /* Disable interrupts during init                                     */
    suart_outb(port + UART_IER, 0x00u);

    /* Set baud rate divisor = 3 → 38400 bps (DLAB=1)                   */
    suart_outb(port + UART_LCR, 0x80u);          /* DLAB on             */
    suart_outb(port + UART_DLL, 0x03u);
    suart_outb(port + UART_DLH, 0x00u);

    /* 8N1 (DLAB=0)                                                       */
    suart_outb(port + UART_LCR, 0x03u);
    /* Enable FIFO, 14-byte threshold                                     */
    suart_outb(port + UART_FCR, 0xC7u);
    /* RTS/DTR asserted                                                   */
    suart_outb(port + UART_MCR, 0x0Bu);

    /* Enable RX-data-available interrupt                                  */
    suart_outb(port + UART_IER, 0x01u);

    kprintf("suart: COM1 probe OK at 0x%x\n", (uint32_t)port);
    return 0;
}

static int suart_remove(driver_t *drv, device_t *dev) {
    (void)drv;
    suart_state_t *s = (suart_state_t*)dev->private_data;
    if (!s) return -1;
    /* Disable all UART interrupts before teardown                        */
    suart_outb(s->base_port + UART_IER, 0x00u);
    kprintf("suart: device removed\n");
    return 0;
}

/* ── cdev_ops: open / close ──────────────────────────────────────────── */

static int suart_open(device_t *dev, uint32_t flags) {
    (void)flags;
    suart_state_t *s = (suart_state_t*)dev->private_data;
    if (!s) return -1;
    /* Flush stale RX data on open */
    unsigned long irqf;
    spinlock_acquire_irqsave(&s->rx_lock, &irqf);
    s->rx_tail = s->rx_head;           /* discard old bytes in ring     */
    s->error_flags = 0;
    spinlock_release_irqrestore(&s->rx_lock, irqf);
    return 0;
}

static int suart_close(device_t *dev) {
    (void)dev;
    return 0;
}

/* ── cdev_ops: read ──────────────────────────────────────────────────── */
/*
 * Reads up to max_len bytes from the RX ring buffer into buf.
 * Blocks (via wq_wait_event) if the ring is empty.
 * Boundary check: never copies more than max_len bytes regardless of
 * what is in the ring.
 */
static ssize_t suart_read(device_t *dev, uint8_t *buf,
                           size_t max_len, uint64_t offset) {
    (void)offset;                       /* character device: ignore offset */
    if (!dev || !buf || max_len == 0) return -1; /* bounds guard          */

    suart_state_t *s = (suart_state_t*)dev->private_data;
    if (!s) return -1;

    size_t copied = 0;

    while (copied < max_len) {
        /* Block until at least one byte is available                     */
        unsigned long irqf;
        spinlock_acquire_irqsave(&s->rx_lock, &irqf);
        uint32_t avail = rx_available(s);
        spinlock_release_irqrestore(&s->rx_lock, irqf);

        if (avail == 0) {
            if (copied > 0) break;      /* return what we have already   */
            /* Wait for ISR to push data                                  */
            wq_wait_event(&s->rx_wq);
            continue;
        }

        /* Drain ring — each iteration copies one byte under lock         */
        spinlock_acquire_irqsave(&s->rx_lock, &irqf);
        while (copied < max_len && rx_available(s) > 0) {
            /* BOUNDS: index masked to ring size — no overrun possible    */
            buf[copied++] = s->rx_buf[s->rx_tail & (SUART_RX_BUF_SIZE - 1u)];
            s->rx_tail++;
        }
        spinlock_release_irqrestore(&s->rx_lock, irqf);
        break; /* return partial read rather than blocking again          */
    }

    return (ssize_t)copied;
}

/* ── cdev_ops: write ─────────────────────────────────────────────────── */
/*
 * Writes at most len bytes from buf to the TX ring / hardware.
 * Enforces explicit len bound — never reads past buf+len.
 */
static ssize_t suart_write(device_t *dev, const uint8_t *buf,
                            size_t len, uint64_t offset) {
    (void)offset;
    if (!dev || !buf || len == 0) return -1; /* bounds guard              */

    suart_state_t *s = (suart_state_t*)dev->private_data;
    if (!s) return -1;

    size_t written = 0;

    while (written < len) {
        unsigned long irqf;
        spinlock_acquire_irqsave(&s->tx_lock, &irqf);

        /* Drain TX ring to hardware while THRE is set                   */
        while (s->tx_head != s->tx_tail) {
            if (!(suart_inb(s->base_port + UART_LSR) & LSR_TX_EMPTY)) break;
            suart_outb(s->base_port + UART_DATA,
                       s->tx_buf[s->tx_tail & (SUART_TX_BUF_SIZE - 1u)]);
            s->tx_tail++;
        }

        /* Fill TX ring from caller buffer — bounded by tx_free and len  */
        uint32_t space = tx_free(s);
        while (written < len && space > 0) {
            /* BOUNDS: index masked; len is explicit caller argument      */
            s->tx_buf[s->tx_head & (SUART_TX_BUF_SIZE - 1u)] = buf[written++];
            s->tx_head++;
            space--;
        }

        /* Drain TX ring → hardware while THRE                           */
        while (s->tx_head != s->tx_tail) {
            if (!(suart_inb(s->base_port + UART_LSR) & LSR_TX_EMPTY)) break;
            suart_outb(s->base_port + UART_DATA,
                       s->tx_buf[s->tx_tail & (SUART_TX_BUF_SIZE - 1u)]);
            s->tx_tail++;
        }

        spinlock_release_irqrestore(&s->tx_lock, irqf);

        if (written < len) {
            /* TX ring full — yield and retry                             */
            wq_wait_event(&s->tx_wq);
        }
    }

    return (ssize_t)written;
}

/* ── cdev_ops: ioctl ─────────────────────────────────────────────────── */
/*
 * buf_len validated before any dereference.
 * Unknown commands return -1 immediately.
 */
static int suart_ioctl(device_t *dev, uint32_t cmd,
                        void *buf, size_t buf_len) {
    suart_state_t *s = (suart_state_t*)dev->private_data;
    if (!s) return -1;

    switch (cmd) {

    case SUART_IOCTL_SET_BAUD: {
        /* buf must point to a uint32_t divisor */
        if (!buf || buf_len < sizeof(uint32_t)) return -1; /* bounds     */
        uint32_t divisor = *(const uint32_t*)buf;
        /* Clamp divisor to valid range [1, 0xFFFF]                      */
        if (divisor == 0 || divisor > 0xFFFFu) return -1;
        suart_outb(s->base_port + UART_LCR, 0x80u);        /* DLAB on   */
        suart_outb(s->base_port + UART_DLL, (uint8_t)(divisor & 0xFFu));
        suart_outb(s->base_port + UART_DLH, (uint8_t)(divisor >> 8));
        suart_outb(s->base_port + UART_LCR, 0x03u);        /* DLAB off  */
        return 0;
    }

    case SUART_IOCTL_FLUSH_RX: {
        unsigned long irqf;
        spinlock_acquire_irqsave(&s->rx_lock, &irqf);
        s->rx_tail = s->rx_head;        /* discard all pending bytes     */
        spinlock_release_irqrestore(&s->rx_lock, irqf);
        return 0;
    }

    case SUART_IOCTL_FLUSH_TX: {
        unsigned long irqf;
        spinlock_acquire_irqsave(&s->tx_lock, &irqf);
        s->tx_tail = s->tx_head;        /* discard all pending bytes     */
        spinlock_release_irqrestore(&s->tx_lock, irqf);
        return 0;
    }

    case SUART_IOCTL_GET_ERRORS: {
        /* buf must point to a uint32_t output */
        if (!buf || buf_len < sizeof(uint32_t)) return -1; /* bounds     */
        *(uint32_t*)buf = s->error_flags;
        s->error_flags  = 0;            /* clear on read                 */
        return 0;
    }

    default:
        return -1; /* -EINVAL: unknown command                           */
    }
}

/* ── cdev_ops vtable (const — no modification after init) ───────────── */
static const cdev_ops_t suart_cdev_ops = {
    .read  = suart_read,
    .write = suart_write,
    .open  = suart_open,
    .close = suart_close,
    .ioctl = suart_ioctl,
};

/* ── Driver and device descriptors ──────────────────────────────────── */
driver_t secure_uart_driver = {
    .name     = "secure_uart",
    .cls      = DEV_CLASS_CHAR,
    .major    = 0,                      /* assigned by drv_register()   */
    .probe    = suart_probe,
    .remove   = suart_remove,
    .cdev_ops = &suart_cdev_ops,
    .next     = (void*)0,
};

device_t secure_uart_device = {
    .name         = "ttyS0",
    .devno        = 0,                  /* assigned by dev_add()        */
    .cls          = DEV_CLASS_CHAR,
    .drv          = (void*)0,
    .private_data = &g_uart_state,
    .initialized  = false,
    .next         = (void*)0,
};

/* ── IRQ handler — called from COM1 ISR (IRQ4) ───────────────────────── */
void secure_uart_irq_handler(void) {
    suart_state_t *s = &g_uart_state;

    /* ── Drain RX FIFO → ring buffer (under rx_lock) ────────────────── */
    spinlock_acquire(&s->rx_lock);

    /* Check LSR for hardware errors first */
    uint8_t lsr = suart_inb(s->base_port + UART_LSR);

    if (lsr & LSR_FRAMING_ERR) s->error_flags |= SUART_ERR_FRAMING;
    if (lsr & LSR_PARITY_ERR)  s->error_flags |= SUART_ERR_PARITY;

    while (lsr & LSR_DATA_READY) {
        uint8_t byte = suart_inb(s->base_port + UART_DATA);
        uint32_t next_head = (s->rx_head + 1u) & (SUART_RX_BUF_SIZE - 1u);
        if (next_head != (s->rx_tail & (SUART_RX_BUF_SIZE - 1u))) {
            /* BOUNDS: ring not full — safe to write                      */
            s->rx_buf[s->rx_head & (SUART_RX_BUF_SIZE - 1u)] = byte;
            s->rx_head++;
        } else {
            /* Ring overrun — record error, discard byte                  */
            s->error_flags |= SUART_ERR_OVERRUN;
        }
        lsr = suart_inb(s->base_port + UART_LSR);
    }

    spinlock_release(&s->rx_lock);

    /* Wake any blocked readers — safe to call from IRQ context          */
    wq_wake_one(&s->rx_wq);
}

/* ── Boot entry point ────────────────────────────────────────────────── */
int secure_uart_init(void) {
    /* Initialise private state */
    memset(&g_uart_state, 0, sizeof(g_uart_state));
    g_uart_state.base_port = 0x3F8u;   /* COM1                          */
    wq_init(&g_uart_state.rx_wq);
    wq_init(&g_uart_state.tx_wq);

    /* Register driver (assigns major)                                    */
    if (drv_register(&secure_uart_driver) != 0) return -1;

    /* Register device (assigns minor, calls probe)                       */
    if (dev_add(&secure_uart_device, &secure_uart_driver) != 0) return -1;

    kprintf("suart: secure UART driver ready on /dev/ttyS0\n");
    return 0;
}
