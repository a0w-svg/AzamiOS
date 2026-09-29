#include <azami/linux_compat.h>
#include <azami/types.h>
#include <azami/defs.h>

struct devres_node {
    void *res;
    void (*free)(device_t *dev, void *res);
    struct devres_node *next;
};

static struct {
    device_t *dev;
    struct devres_node *head;
} devres_hash[256];

static spinlock_t devres_lock = SPINLOCK_INIT;

static struct devres_node **get_devres_head(device_t *dev) {
    for (int i = 0; i < 256; i++) {
        if (devres_hash[i].dev == dev) return &devres_hash[i].head;
    }
    for (int i = 0; i < 256; i++) {
        if (devres_hash[i].dev == NULL) {
            devres_hash[i].dev = dev;
            return &devres_hash[i].head;
        }
    }
    return NULL;
}

static void add_devres(device_t *dev, void *res, void (*free)(device_t *, void *)) {
    struct devres_node *node = kmalloc(sizeof(*node));
    if (!node) return;
    node->res = res;
    node->free = free;
    
    irqflags_t flags = spinlock_lock_irqsave(&devres_lock);
    struct devres_node **head = get_devres_head(dev);
    if (head) {
        node->next = *head;
        *head = node;
    } else {
        kfree(node); // Hash table full
    }
    spinlock_unlock_irqrestore(&devres_lock, flags);
}

static void free_kmalloc_res(device_t *dev, void *res) {
    (void)dev;
    kfree(res);
}

void *devm_kmalloc(device_t *dev, size_t size, unsigned int gfp) {
    (void)gfp;
    void *p = kmalloc(size);
    if (p) add_devres(dev, p, free_kmalloc_res);
    return p;
}

void *devm_kzalloc(device_t *dev, size_t size, unsigned int gfp) {
    (void)gfp;
    void *p = kzalloc(size);
    if (p) add_devres(dev, p, free_kmalloc_res);
    return p;
}

void devm_kfree(device_t *dev, void *p) {
    irqflags_t flags = spinlock_lock_irqsave(&devres_lock);
    struct devres_node **head = get_devres_head(dev);
    if (head) {
        struct devres_node **curr = head;
        while (*curr) {
            if ((*curr)->res == p) {
                struct devres_node *node = *curr;
                *curr = node->next;
                spinlock_unlock_irqrestore(&devres_lock, flags);
                node->free(dev, node->res);
                kfree(node);
                return;
            }
            curr = &(*curr)->next;
        }
    }
    spinlock_unlock_irqrestore(&devres_lock, flags);
}

struct devres_irq {
    int irq;
    void *dev_id;
};

static void free_irq_res(device_t *dev, void *res) {
    (void)dev;
    struct devres_irq *di = res;
    free_irq(di->irq, di->dev_id);
    kfree(di);
}

int devm_request_irq(device_t *dev, unsigned int irq, linux_irq_handler_t handler, unsigned long irqflags, const char *devname, void *dev_id) {
    int ret = request_irq(irq, handler, irqflags, devname, dev_id);
    if (ret == 0) {
        struct devres_irq *di = kmalloc(sizeof(*di));
        if (di) {
            di->irq = irq;
            di->dev_id = dev_id;
            add_devres(dev, di, free_irq_res);
        }
    }
    return ret;
}

void devm_free_irq(device_t *dev, unsigned int irq, void *dev_id) {
    irqflags_t flags = spinlock_lock_irqsave(&devres_lock);
    struct devres_node **head = get_devres_head(dev);
    if (head) {
        struct devres_node **curr = head;
        while (*curr) {
            if ((*curr)->free == free_irq_res) {
                struct devres_irq *di = (*curr)->res;
                if (di->irq == (int)irq && di->dev_id == dev_id) {
                    struct devres_node *node = *curr;
                    *curr = node->next;
                    spinlock_unlock_irqrestore(&devres_lock, flags);
                    node->free(dev, node->res);
                    kfree(node);
                    return;
                }
            }
            curr = &(*curr)->next;
        }
    }
    spinlock_unlock_irqrestore(&devres_lock, flags);
}

void devm_release_all(device_t *dev) {
    irqflags_t flags = spinlock_lock_irqsave(&devres_lock);
    struct devres_node **head = get_devres_head(dev);
    if (head) {
        struct devres_node *curr = *head;
        *head = NULL;
        for (int i = 0; i < 256; i++) {
            if (devres_hash[i].dev == dev) {
                devres_hash[i].dev = NULL;
                break;
            }
        }
        spinlock_unlock_irqrestore(&devres_lock, flags);
        
        while (curr) {
            struct devres_node *next = curr->next;
            curr->free(dev, curr->res);
            kfree(curr);
            curr = next;
        }
    } else {
        spinlock_unlock_irqrestore(&devres_lock, flags);
    }
}
