/* ============================================================================
 * AzamiOS — Parallel Port (LPT) Interrupt-Driven Driver
 * File: drivers/lpt.c
 * ============================================================================ */

#include "lpt.h"
#include "../../include/azami/defs.h"
#include "../../fs/vfs.h"
#include "../../arch/x86_64/cpu/idt.h"
#include "../../hal/irq.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/lib/string.h"
#include "../../kernel/uaccess.h"

#define LPT_REG_DATA    0
#define LPT_REG_STATUS  1
#define LPT_REG_CTRL    2

#define CTRL_STROBE     0x01
#define CTRL_AUTO_LF    0x02
#define CTRL_INIT       0x04
#define CTRL_SELECT     0x08
#define CTRL_IRQ_EN     0x10

#define RING_BUFFER_SIZE 2048

typedef struct {
    u16 port;
    u8 irq;
    
    char tx_buf[RING_BUFFER_SIZE];
    u32 tx_head;
    u32 tx_tail;
    
    spinlock_t lock;
    thread_t *tx_waiter;
    bool tx_running;
} lpt_port_t;

static lpt_port_t g_lpt1 = { .port = LPT1_PORT, .irq = 7, .lock = SPINLOCK_INIT };
static lpt_port_t g_lpt2 = { .port = LPT2_PORT, .irq = 5, .lock = SPINLOCK_INIT };

extern void idt_register_irq(u8 vector, void (*fn)(pt_regs_t *, void *), void *ctx);

static void lpt_send_byte(lpt_port_t *p, u8 data)
{
    /* Write data to port */
    outb(p->port + LPT_REG_DATA, data);
    
    /* Delay briefly (optional on fast CPUs, usually OUT instructions are slow enough, but to be safe) */
    cpu_pause();
    
    /* Pulse STROBE low (asserted) then high (de-asserted).
       Note: PC parallel port STROBE is hardware-inverted, so bit 0 = 1 means STROBE is pulled LOW (active) */
    u8 ctrl = inb(p->port + LPT_REG_CTRL);
    outb(p->port + LPT_REG_CTRL, ctrl | CTRL_STROBE);
    
    cpu_pause();
    cpu_pause();
    cpu_pause();
    
    outb(p->port + LPT_REG_CTRL, ctrl & ~CTRL_STROBE);
}

static void lpt_handle_interrupt(lpt_port_t *p)
{
    spinlock_lock(&p->lock);
    
    if (p->tx_head != p->tx_tail) {
        /* Send next byte */
        lpt_send_byte(p, p->tx_buf[p->tx_tail]);
        p->tx_tail = (p->tx_tail + 1) % RING_BUFFER_SIZE;
        
        if (p->tx_waiter && ((p->tx_head - p->tx_tail + RING_BUFFER_SIZE) % RING_BUFFER_SIZE) < (RING_BUFFER_SIZE / 2)) {
            /* Wake up writer if buffer has drained below half */
            sched_unblock(p->tx_waiter);
            p->tx_waiter = NULL;
        }
    } else {
        p->tx_running = false;
        if (p->tx_waiter) {
            sched_unblock(p->tx_waiter);
            p->tx_waiter = NULL;
        }
    }
    
    spinlock_unlock(&p->lock);
}

static void lpt_irq_handler_lpt1(pt_regs_t *r, void *ctx)
{
    (void)r; (void)ctx;
    lpt_handle_interrupt(&g_lpt1);

}

static void lpt_irq_handler_lpt2(pt_regs_t *r, void *ctx)
{
    (void)r; (void)ctx;
    lpt_handle_interrupt(&g_lpt2);

}

static void lpt_start_tx(lpt_port_t *p)
{
    if (!p->tx_running && p->tx_head != p->tx_tail) {
        p->tx_running = true;
        lpt_send_byte(p, p->tx_buf[p->tx_tail]);
        p->tx_tail = (p->tx_tail + 1) % RING_BUFFER_SIZE;
    }
}

static s64 lpt_fops_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)buf; (void)len; (void)offset;
    /* SPP LPT is unidirectional write-only */
    return -1;
}

static s64 lpt_fops_write(struct file *filp, const void *buf, size_t len, u64 *offset)
{
    lpt_port_t *p = (lpt_port_t *)filp->private_data;
    if (!buf) return -EINVAL;
    const char *src = (const char *)buf;
    size_t count = 0;
    
    while (count < len) {
        spinlock_lock(&p->lock);
        u32 next = (p->tx_head + 1) % RING_BUFFER_SIZE;
        
        if (next != p->tx_tail) {
            char ch = src[count];
            p->tx_buf[p->tx_head] = ch;
            p->tx_head = next;
            count++;
            lpt_start_tx(p);
            spinlock_unlock(&p->lock);
        } else {
            p->tx_waiter = sched_current_thread();
            lpt_start_tx(p);
            spinlock_unlock(&p->lock);
            sched_block(THREAD_BLOCKED);
        }
    }
    
    if (offset) *offset += len;
    return len;
}

static s64 lpt_fops_ioctl(struct file *filp, u32 cmd, u64 arg)
{
    lpt_port_t *p = (lpt_port_t *)filp->private_data;
    
    switch (cmd) {
        case LPT_RESET:
            spinlock_lock(&p->lock);
            /* INIT is hardware-inverted; bit 2 = 0 means pull INIT low (active) */
            u8 ctrl = inb(p->port + LPT_REG_CTRL);
            outb(p->port + LPT_REG_CTRL, ctrl & ~CTRL_INIT);
            
            /* Give it some time to reset */
            for (volatile int i = 0; i < 50000; i++) cpu_pause();
            
            outb(p->port + LPT_REG_CTRL, ctrl | CTRL_INIT);
            spinlock_unlock(&p->lock);
            return 0;
            
        case LPT_GET_STATUS: {
            u8 status = inb(p->port + LPT_REG_STATUS);
            if (arg) {
                if ((uintptr_t)arg >= 0x8000000000000000ULL) return -EFAULT;
                if (copy_to_user((void *)(uintptr_t)arg, &status, 1) != 0) return -EFAULT;
            }
            return status;
        }
        
        default:
            return -1;
    }
}

static file_operations_t lpt_fops = {
    .read = lpt_fops_read,
    .write = lpt_fops_write,
    .ioctl = lpt_fops_ioctl,
};

static void lpt_init_port(lpt_port_t *p)
{
    /* Set basic control bits: INIT=1, SELECT=1, IRQ_EN=1 */
    outb(p->port + LPT_REG_CTRL, CTRL_INIT | CTRL_SELECT | CTRL_IRQ_EN);
}

void lpt_register_devfs(void)
{
    lpt_init_port(&g_lpt1);
    lpt_init_port(&g_lpt2);
    
    /* Register ISRs (IRQ 7 = vector 39, IRQ 5 = vector 37) */
    idt_register_irq(39, lpt_irq_handler_lpt1, NULL);
    idt_register_irq(37, lpt_irq_handler_lpt2, NULL);
    
    hal_irq_enable(7, 39);
    hal_irq_enable(5, 37);
    
    devfs_register_device("lp0", &lpt_fops, &g_lpt1);
    devfs_register_device("lp1", &lpt_fops, &g_lpt2);
}
