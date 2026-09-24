/* ============================================================================
 * AzamiOS — UART (16550) Interrupt-Driven Driver
 * File: drivers/uart.c
 * ============================================================================ */

#include "uart.h"
#include "../../include/azami/defs.h"
#include "../../fs/vfs.h"
#include "../../arch/x86_64/cpu/idt.h"
#include "../../hal/irq.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/uaccess.h"

/* 16550 register offsets (relative to base port) */
#define UART_RBR   0   /* Receive Buffer Register  (read) */
#define UART_THR   0   /* Transmit Holding Register (write) */
#define UART_IER   1   /* Interrupt Enable Register */
#define UART_IIR   2   /* Interrupt Identification Register (read) */
#define UART_FCR   2   /* FIFO Control Register (write) */
#define UART_LCR   3   /* Line Control Register */
#define UART_MCR   4   /* Modem Control Register */
#define UART_LSR   5   /* Line Status Register */
#define UART_MSR   6   /* Modem Status Register */
#define UART_DLL   0   /* Divisor Latch Low  (when DLAB=1) */
#define UART_DLH   1   /* Divisor Latch High (when DLAB=1) */

#define LSR_THRE   0x20  /* Transmit Holding Register Empty */
#define LSR_DR     0x01  /* Data Ready (receive) */

#define IIR_INT_MASK 0x0F
#define IIR_MODEM    0x00  /* Modem status  — cleared by reading MSR */
#define IIR_TX_EMPTY 0x02
#define IIR_RX_DATA  0x04
#define IIR_LINE_ST  0x06  /* Receiver line status — cleared by reading LSR */
#define IIR_RX_TIMEO 0x0C  /* Character timeout — cleared by reading RBR */

/* Backstop on the interrupt-service loop. Every cause the 16550 can raise is
 * acknowledged below, so this should never be reached; it exists so that an
 * unexpected one cannot wedge a CPU inside an interrupt handler. */
#define UART_ISR_MAX_ITERS 1024u

#define RING_BUFFER_SIZE 2048

typedef struct {
    u16 port;
    u8 irq;
    
    char rx_buf[RING_BUFFER_SIZE];
    u32 rx_head;
    u32 rx_tail;
    
    char tx_buf[RING_BUFFER_SIZE];
    u32 tx_head;
    u32 tx_tail;
    
    spinlock_t lock;
    thread_t *rx_waiter;
    thread_t *tx_waiter;
    
    bool tx_running;
} uart_port_t;

static uart_port_t g_com1 = { .port = UART_COM1, .irq = 4, .lock = SPINLOCK_INIT };
static uart_port_t g_com2 = { .port = UART_COM2, .irq = 3, .lock = SPINLOCK_INIT };


static void uart_set_baud(u16 port, u32 baud)
{
    u32 divisor = 115200 / baud;
    outb(port + UART_LCR, 0x80);   /* Enable DLAB */
    outb(port + UART_DLL, divisor & 0xFF);
    outb(port + UART_DLH, (divisor >> 8) & 0xFF);
    outb(port + UART_LCR, 0x03);   /* 8N1 */
}

void uart_init(u16 port)
{
    outb(port + UART_IER, 0x00);   /* Disable all interrupts temporarily */
    uart_set_baud(port, 115200);   /* Default baud */
    outb(port + UART_FCR, 0xC7);   /* Enable FIFO, clear TX/RX, 14-byte threshold */
    outb(port + UART_MCR, 0x0B);   /* DTR + RTS + OUT2 */
    outb(port + UART_IER, 0x01);   /* Enable RX interrupts only initially */
}

/* ── Polling implementation for kernel prints/panics ─────────────────────── */

/* Bound on the THRE wait below. A 16550 at 115200 baud clears the holding
 * register in well under 100 us, so this is orders of magnitude more slack
 * than a working port needs. */
#define UART_TX_POLL_SPINS 200000u

static inline void uart_wait_tx_poll(u16 port)
{
    /*
     * Bounded, deliberately.
     *
     * Every pr_*() line and every panic() message goes through here. If no
     * device answers at this port, or a virtual one's FIFO is full because
     * nothing on the host is draining it, THRE never comes up — and an
     * unbounded wait then hangs the machine inside the very call that was
     * trying to say what went wrong. Losing a character is strictly better
     * than losing the panic and the machine with it.
     */
    u32 spins = UART_TX_POLL_SPINS;
    while (!(inb(port + UART_LSR) & LSR_THRE)) {
        if (--spins == 0) return;
        cpu_pause();
    }
}

void uart_putc(u16 port, char c)
{
    uart_wait_tx_poll(port);
    if (c == '\n') {
        outb(port + UART_THR, '\r');
        uart_wait_tx_poll(port);
    }
    outb(port + UART_THR, (u8)c);
}

void uart_puts(u16 port, const char *s)
{
    while (*s) uart_putc(port, *s++);
}

void uart_write(u16 port, const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) uart_putc(port, buf[i]);
}

int uart_getc(u16 port)
{
    if (inb(port + UART_LSR) & LSR_DR) {
        return (int)inb(port + UART_RBR);
    }
    return -1;
}

/* ── Interrupt Driven Implementation ─────────────────────────────────────── */

static void uart_handle_interrupt(uart_port_t *p)
{
    irqflags_t irqf = spinlock_lock_irqsave(&p->lock);
    
    /*
     * Every branch below must *clear* the cause it handles, or the 16550 keeps
     * reporting it and this loop never ends — inside an interrupt handler,
     * with interrupts disabled and the port lock held, which takes the CPU
     * with it.
     *
     * Receiver-line-status (break, overrun, parity, framing) and modem-status
     * (CTS/DSR/RI/DCD changes) used to have no branch at all. Both are
     * reachable in ordinary use — a break or a wrong baud rate raises the
     * first, plugging a terminal into a virtual port raises the second — and
     * neither is cleared by anything the old handler did, so either one hung
     * the machine. They are acknowledged explicitly now, and the iteration
     * cap catches anything still unaccounted for.
     */
    u32 guard = UART_ISR_MAX_ITERS;

    while (guard--) {
        u8 iir = inb(p->port + UART_IIR);
        if (iir & 0x01) break; /* No more interrupts pending */
        
        u8 cause = iir & IIR_INT_MASK;
        
        if (cause == IIR_LINE_ST) {
            /* Reading LSR is what clears overrun/parity/framing/break. The
             * byte that arrived with the error, if any, is drained below on
             * the next pass. */
            (void)inb(p->port + UART_LSR);
        }
        else if (cause == IIR_MODEM) {
            (void)inb(p->port + UART_MSR);
        }
        else if (cause == IIR_RX_DATA || cause == IIR_RX_TIMEO) {
            /* Bounded by the FIFO depth several times over: a device that
             * reports Data Ready forever must not hold the CPU here. */
            u32 rx_guard = RING_BUFFER_SIZE;
            while ((inb(p->port + UART_LSR) & LSR_DR) && rx_guard--) {
                u8 c = inb(p->port + UART_RBR);
                u32 next = (p->rx_head + 1) % RING_BUFFER_SIZE;
                if (next != p->rx_tail) {
                    p->rx_buf[p->rx_head] = c;
                    p->rx_head = next;
                }
            }
            if (p->rx_waiter) {
                sched_unblock(p->rx_waiter);
                p->rx_waiter = NULL;
            }
        }
        else if (cause == IIR_TX_EMPTY) {
            if (p->tx_head != p->tx_tail) {
                outb(p->port + UART_THR, p->tx_buf[p->tx_tail]);
                p->tx_tail = (p->tx_tail + 1) % RING_BUFFER_SIZE;
            } else {
                p->tx_running = false;
                /* Disable TX interrupts */
                u8 ier = inb(p->port + UART_IER);
                outb(p->port + UART_IER, ier & ~0x02);
            }
            if (p->tx_waiter) {
                sched_unblock(p->tx_waiter);
                p->tx_waiter = NULL;
            }
        }
        else {
            /* Unknown cause: read the status registers that acknowledge the
             * remaining sources, then give up rather than spin on it. */
            (void)inb(p->port + UART_LSR);
            (void)inb(p->port + UART_MSR);
            break;
        }
    }
    
    spinlock_unlock_irqrestore(&p->lock, irqf);
}

static void uart_irq_handler_com1(pt_regs_t *r, void *ctx)
{
    (void)r; (void)ctx;
    uart_handle_interrupt(&g_com1);

}

static void uart_irq_handler_com2(pt_regs_t *r, void *ctx)
{
    (void)r; (void)ctx;
    uart_handle_interrupt(&g_com2);

}

static void uart_start_tx(uart_port_t *p)
{
    if (!p->tx_running && p->tx_head != p->tx_tail) {
        p->tx_running = true;
        u8 ier = inb(p->port + UART_IER);
        outb(p->port + UART_IER, ier | 0x02); /* Enable TX interrupt */
        
        /* Kickstart */
        if (inb(p->port + UART_LSR) & LSR_THRE) {
            outb(p->port + UART_THR, p->tx_buf[p->tx_tail]);
            p->tx_tail = (p->tx_tail + 1) % RING_BUFFER_SIZE;
        }
    }
}

static s64 uart_fops_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    uart_port_t *p = (uart_port_t *)filp->private_data;
    if (!buf) return -EINVAL;
    size_t count = 0;
    char *out = (char *)buf;
    
    while (count < len) {
        irqflags_t irqf = spinlock_lock_irqsave(&p->lock);
        
        if (p->rx_head != p->rx_tail) {
            char ch = p->rx_buf[p->rx_tail];
            p->rx_tail = (p->rx_tail + 1) % RING_BUFFER_SIZE;
            spinlock_unlock_irqrestore(&p->lock, irqf);
            out[count++] = ch;
        } else {
            if (count > 0) {
                spinlock_unlock_irqrestore(&p->lock, irqf);
                break; /* Return what we have */
            }
            /* BUG-26: set waiter under the lock so any IRQ that fires after
             * unlock will call sched_unblock and mark us READY before we block.
             * sched_unblock handles THREAD_RUNNING → THREAD_READY, so the
             * subsequent sched_block will see THREAD_READY and abort blocking. */
            p->rx_waiter = sched_current_thread();
            spinlock_unlock_irqrestore(&p->lock, irqf);
            sched_block(THREAD_BLOCKED);
            /* Re-check: loop will exit naturally when data arrives or return early */
        }
    }
    
    return (s64)count;
}

static s64 uart_fops_write(struct file *filp, const void *buf, size_t len, u64 *offset)
{
    uart_port_t *p = (uart_port_t *)filp->private_data;
    if (!buf) return -EINVAL;
    const char *src = (const char *)buf;
    size_t count = 0;
    
    while (count < len) {
        irqflags_t irqf = spinlock_lock_irqsave(&p->lock);
        u32 next = (p->tx_head + 1) % RING_BUFFER_SIZE;
        
        if (next != p->tx_tail) {
            char ch = src[count];
            p->tx_buf[p->tx_head] = ch;
            p->tx_head = next;
            count++;
            uart_start_tx(p);
            spinlock_unlock_irqrestore(&p->lock, irqf);
        } else {
            /* BUG-27: same lost-wakeup pattern as rx_waiter — set under lock
             * so the IRQ handler sees it and calls sched_unblock before we
             * call sched_block; sched_block aborts if state is already READY. */
            p->tx_waiter = sched_current_thread();
            uart_start_tx(p);
            spinlock_unlock_irqrestore(&p->lock, irqf);
            sched_block(THREAD_BLOCKED);
        }
    }
    
    if (offset) *offset += len;
    return len;
}

static s64 uart_fops_ioctl(struct file *filp, u32 cmd, u64 arg)
{
    uart_port_t *p = (uart_port_t *)filp->private_data;
    
    switch (cmd) {
        case UART_SET_BAUD: {
            u32 baud = (u32)arg;
            if (baud == 0 || 115200 % baud != 0) return -1;
            spinlock_lock(&p->lock);
            uart_set_baud(p->port, baud);
            spinlock_unlock(&p->lock);
            return 0;
        }
        case 0x5401: { /* TCGETS */
            if (!arg || arg >= 0x8000000000000000ULL) return -1;
            struct {
                u32 c_iflag;
                u32 c_oflag;
                u32 c_cflag;
                u32 c_lflag;
                u8  c_line;
                u8  c_cc[32];
                u32 c_ispeed;
                u32 c_ospeed;
            } term;
            __builtin_memset(&term, 0, sizeof(term));
            term.c_iflag = 0x0500;
            term.c_oflag = 0x0005;
            term.c_cflag = 0x00BF;
            term.c_lflag = 0x8A3B;
            term.c_ispeed = 38400;
            term.c_ospeed = 38400;
            if (copy_to_user((void *)arg, &term, sizeof(term)) != 0) return -1;
            return 0;
        }
        case 0x5402:   /* TCSETS */
        case 0x5403:   /* TCSETSW */
        case 0x5404:   /* TCSETSF */
        case 0x5414:   /* TIOCSWINSZ */
            return 0;
        case 0x5413: { /* TIOCGWINSZ */
            if (!arg || arg >= 0x8000000000000000ULL) return -1;
            struct {
                u16 ws_row;
                u16 ws_col;
                u16 ws_xpixel;
                u16 ws_ypixel;
            } ws = { .ws_row = 25, .ws_col = 80, .ws_xpixel = 640, .ws_ypixel = 400 };
            if (copy_to_user((void *)arg, &ws, sizeof(ws)) != 0) return -1;
            return 0;
        }
        default:
            return -1;
    }
}

static file_operations_t uart_fops = {
    .read = uart_fops_read,
    .write = uart_fops_write,
    .ioctl = uart_fops_ioctl,
};

void uart_register_devfs(void)
{
    /* Initialize hardware */
    uart_init(UART_COM1);
    uart_init(UART_COM2);
    
    /* Register ISRs (IRQ 4 = vector 36, IRQ 3 = vector 35) */
    idt_register_irq(36, uart_irq_handler_com1, NULL);
    idt_register_irq(35, uart_irq_handler_com2, NULL);
    hal_irq_enable(4, 36);
    hal_irq_enable(3, 35);
    
    devfs_register_device("ttyS0", &uart_fops, &g_com1);
    devfs_register_device("ttyS1", &uart_fops, &g_com2);
}
