#include <azami/linux_compat.h>
#include <azami/types.h>
#include <azami/defs.h>
#include "../../hal/irq.h"
#include "../../arch/x86_64/cpu/idt.h"

struct irq_wrapper {
    int irq;
    linux_irq_handler_t handler;
    void *dev_id;
};

static struct irq_wrapper wrappers[HAL_NR_IRQS];

static void irq_wrapper_func(pt_regs_t *r, void *ctx) {
    (void)r;
    struct irq_wrapper *w = ctx;
    if (w && w->handler) {
        w->handler(w->irq, w->dev_id);
    }
}

int request_irq(unsigned int irq, linux_irq_handler_t handler, unsigned long flags, const char *name, void *dev) {
    (void)flags;
    (void)name;
    if (irq >= HAL_NR_IRQS) return -1;
    
    wrappers[irq].irq = irq;
    wrappers[irq].handler = handler;
    wrappers[irq].dev_id = dev;
    
    idt_register_irq(irq + 32, irq_wrapper_func, &wrappers[irq]);
    hal_irq_enable(irq, irq + 32);
    return 0;
}

void free_irq(unsigned int irq, void *dev_id) {
    (void)dev_id;
    if (irq >= HAL_NR_IRQS) return;
    hal_irq_disable(irq);
    wrappers[irq].handler = NULL;
}

int pci_alloc_irq_vectors(device_t *dev, unsigned int min_vecs, unsigned int max_vecs, unsigned int flags) {
    (void)min_vecs;
    (void)max_vecs;
    (void)flags;
    pci_device_info_t *info = pci_get_device_info(dev);
    if (!info) return -1;
    return 1;
}

void pci_free_irq_vectors(device_t *dev) {
    (void)dev;
}

int pci_irq_vector(device_t *dev, unsigned int nr) {
    if (nr > 0) return -1;
    pci_device_info_t *info = pci_get_device_info(dev);
    if (!info) return -1;
    return info->interrupt_line;
}
