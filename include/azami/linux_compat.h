/* ============================================================================
 * AzamiOS — Linux Driver Compatibility Layer
 * File: include/azami/linux_compat.h
 *
 * Umbrella header for ported Linux drivers. Provides Linux-compatible API
 * names and types so that Linux driver sources can compile with minimal
 * changes against the AzamiOS kernel infrastructure.
 *
 * Usage in a ported driver:
 *   #include <azami/linux_compat.h>
 *
 * This pulls in: DMA API, workqueues, tasklets, timers, jiffies, wait
 * queues, completions, kernel threads, PCI, IRQ, MMIO access, and
 * logging macros.
 * ============================================================================ */
#pragma once

/* ── Core types and macros ─────────────────────────────────────────────── */
#include "types.h"
#include "defs.h"
#include "../../hal/device.h"

/* ── Logging: printk, pr_err, dev_err, etc. ────────────────────────────── */
#include "dev_printk.h"

/* ── MMIO and I/O port access: readl/writel, ioremap, inb/outb ─────────── */
#include "ioport.h"

/* ── Memory allocation ─────────────────────────────────────────────────── */
#include "../../kernel/mm/kmalloc.h"

/* GFP flag compatibility — AzamiOS kmalloc has no flags, these are no-ops */
#define GFP_KERNEL    0
#define GFP_ATOMIC    0
#define GFP_DMA       0
#define GFP_DMA32     0
#define GFP_NOIO      0
#define GFP_NOWAIT    0
#define __GFP_ZERO    0
#define __GFP_HIGH    0

/* Linux kmalloc/kzalloc with GFP flags — just ignore the flags */
#define kmalloc_gfp(size, gfp)    kmalloc(size)
#define kzalloc_gfp(size, gfp)    kzalloc(size)
#define kcalloc_gfp(n, s, gfp)   kcalloc(n, s)
#define kmalloc_array(n, s, gfp)  kcalloc(n, s)
#define kstrdup(s, gfp)           __kstrdup(s)
static inline char *__kstrdup(const char *s) {
    if (!s) return NULL;
    size_t len = 0;
    while (s[len]) len++;
    char *d = kmalloc(len + 1);
    if (d) { for (size_t i = 0; i <= len; i++) d[i] = s[i]; }
    return d;
}


/* ── DMA API ───────────────────────────────────────────────────────────── */
#include "../../kernel/mm/dma.h"

/* ── IRQ handling ──────────────────────────────────────────────────────── */
#include "../../hal/irq.h"

/* Linux IRQ flag compatibility */
#define IRQF_SHARED       0x0080
#define IRQF_TRIGGER_LOW   0
#define IRQF_TRIGGER_HIGH  0
#define IRQF_TRIGGER_RISING  0
#define IRQF_TRIGGER_FALLING 0

/* Linux request_irq compat — map to AzamiOS hal_irq API */
#define IRQ_NONE 0
#define IRQ_HANDLED 1
#define IRQ_WAKE_THREAD 2
typedef int irqreturn_t;
typedef irqreturn_t (*linux_irq_handler_t)(int irq, void *dev_id);

int request_irq(unsigned int irq, linux_irq_handler_t handler, unsigned long flags, const char *name, void *dev);
void free_irq(unsigned int irq, void *dev_id);

#define PCI_IRQ_LEGACY 1
#define PCI_IRQ_MSI    2
#define PCI_IRQ_MSIX   4
#define PCI_IRQ_ALL_TYPES (PCI_IRQ_LEGACY | PCI_IRQ_MSI | PCI_IRQ_MSIX)
int pci_alloc_irq_vectors(device_t *dev, unsigned int min_vecs, unsigned int max_vecs, unsigned int flags);
void pci_free_irq_vectors(device_t *dev);
int pci_irq_vector(device_t *dev, unsigned int nr);
void *devm_kmalloc(device_t *dev, size_t size, unsigned int gfp);
void *devm_kzalloc(device_t *dev, size_t size, unsigned int gfp);
void devm_kfree(device_t *dev, void *p);
int devm_request_irq(device_t *dev, unsigned int irq, linux_irq_handler_t handler, unsigned long irqflags, const char *devname, void *dev_id);
void devm_free_irq(device_t *dev, unsigned int irq, void *dev_id);
void devm_release_all(device_t *dev);
/* ── PCI subsystem ─────────────────────────────────────────────────────── */
#include "../../hal/pci.h"

/* ── Spinlocks ─────────────────────────────────────────────────────────── */
#include "../../arch/x86_64/cpu/spinlock.h"

/* Linux-style spin_lock wrappers */
#define spin_lock_init(l)              spinlock_init(l)
#define spin_lock(l)                   spinlock_lock(l)
#define spin_unlock(l)                 spinlock_unlock(l)
#define spin_lock_irqsave(l, f)        do { (f) = spinlock_lock_irqsave(l); } while(0)
#define spin_unlock_irqrestore(l, f)   spinlock_unlock_irqrestore(l, f)
#define spin_lock_irq(l)               do { cpu_cli(); spinlock_lock(l); } while(0)
#define spin_unlock_irq(l)             do { spinlock_unlock(l); cpu_sti(); } while(0)
#define spin_lock_bh(l)                spinlock_lock(l)
#define spin_unlock_bh(l)              spinlock_unlock(l)

/* ── Mutexes (simplified as spinlocks for now) ─────────────────────────── */
typedef spinlock_t mutex_t;
#define DEFINE_MUTEX(name) spinlock_t name = SPINLOCK_INIT
#define mutex_init(m)      spinlock_init(m)
#define mutex_lock(m)      spinlock_lock(m)
#define mutex_unlock(m)    spinlock_unlock(m)
#define mutex_trylock(m)   spinlock_try_lock(m)

/* ── Atomic operations ─────────────────────────────────────────────────── */
typedef struct { _Atomic int counter; } atomic_t;
typedef struct { _Atomic long counter; } atomic_long_t;

#define ATOMIC_INIT(v)         { .counter = (v) }
#define atomic_read(v)         __atomic_load_n(&(v)->counter, __ATOMIC_RELAXED)
#define atomic_set(v, i)       __atomic_store_n(&(v)->counter, (i), __ATOMIC_RELAXED)
#define atomic_add(i, v)       __atomic_fetch_add(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic_sub(i, v)       __atomic_fetch_sub(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic_inc(v)          atomic_add(1, v)
#define atomic_dec(v)          atomic_sub(1, v)
#define atomic_inc_return(v)   (__atomic_fetch_add(&(v)->counter, 1, __ATOMIC_SEQ_CST) + 1)
#define atomic_dec_return(v)   (__atomic_fetch_sub(&(v)->counter, 1, __ATOMIC_SEQ_CST) - 1)
#define atomic_dec_and_test(v) (atomic_dec_return(v) == 0)
#define atomic_add_return(i,v) (__atomic_fetch_add(&(v)->counter, (i), __ATOMIC_SEQ_CST) + (i))
#define atomic_xchg(v, n)     __atomic_exchange_n(&(v)->counter, (n), __ATOMIC_SEQ_CST)
#define atomic_cmpxchg(v, o, n) ({ int __old = (o); \
    __atomic_compare_exchange_n(&(v)->counter, &__old, (n), false, \
    __ATOMIC_SEQ_CST, __ATOMIC_RELAXED); __old; })

/* READ_ONCE / WRITE_ONCE */
#ifndef READ_ONCE
#define READ_ONCE(x)   (*(volatile __typeof__(x) *)&(x))
#endif
#ifndef WRITE_ONCE
#define WRITE_ONCE(x, val) do { *(volatile __typeof__(x) *)&(x) = (val); } while (0)
#endif

/* ── Jiffies, HZ, and delays ──────────────────────────────────────────── */
#include "../../kernel/jiffies.h"

/* ── Kernel timers (timer_setup / mod_timer / del_timer) ───────────────── */
#include "../../kernel/timer_list.h"

/* ── Wait queues ───────────────────────────────────────────────────────── */
#include "../../kernel/wait.h"

/* ── Completions ───────────────────────────────────────────────────────── */
#include "../../kernel/completion.h"

/* ── Workqueues and tasklets ───────────────────────────────────────────── */
#include "../../kernel/workqueue.h"
#include "../../kernel/softirq.h"

/* ── Kernel threads ────────────────────────────────────────────────────── */
#include "../../kernel/kthread.h"

/* ── String helpers ────────────────────────────────────────────────────── */
#include "../../kernel/lib/string.h"

/* ── Kernel debugging helpers ──────────────────────────────────────────── */
#define WARN_ON(cond) ({ int __ret = !!(cond); \
    if (__ret) kprintf("WARNING: %s at %s:%d\n", #cond, __FILE__, __LINE__); __ret; })
#define WARN_ON_ONCE(cond) ({ \
    static bool __warned = false; \
    int __ret = !!(cond); \
    if (__ret && !__warned) { __warned = true; \
        kprintf("WARNING: %s at %s:%d\n", #cond, __FILE__, __LINE__); } __ret; })
#define WARN(cond, fmt, ...) ({ int __ret = !!(cond); \
    if (__ret) kprintf("WARNING: " fmt "\n", ##__VA_ARGS__); __ret; })

/* ── Endianness helpers (x86_64 is little-endian) ──────────────────────── */
#define cpu_to_le16(x) ((u16)(x))
#define cpu_to_le32(x) ((u32)(x))
#define cpu_to_le64(x) ((u64)(x))
#define le16_to_cpu(x) ((u16)(x))
#define le32_to_cpu(x) ((u32)(x))
#define le64_to_cpu(x) ((u64)(x))
#define cpu_to_be16(x) __builtin_bswap16(x)
#define cpu_to_be32(x) __builtin_bswap32(x)
#define cpu_to_be64(x) __builtin_bswap64(x)
#define be16_to_cpu(x) __builtin_bswap16(x)
#define be32_to_cpu(x) __builtin_bswap32(x)
#define be64_to_cpu(x) __builtin_bswap64(x)

typedef u16 __le16;
typedef u32 __le32;
typedef u64 __le64;
typedef u16 __be16;
typedef u32 __be32;
typedef u64 __be64;

/* ── Miscellaneous Linux-isms ──────────────────────────────────────────── */
#define MODULE_LICENSE(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_DEVICE_TABLE(type, table)
#define MODULE_FIRMWARE(x)
#define module_param(name, type, perm)
#define module_param_named(oname, name, type, perm)
#define MODULE_PARM_DESC(name, desc)

#define __init
#define __exit
#define __devinit
#define __devexit
#define __iomem    volatile

#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)

/* Rounding macros */
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define round_up(x, y)     ((((x) - 1) | ((y) - 1)) + 1)
#define round_down(x, y)   ((x) & ~((y) - 1))

/* ── Firmware loading ──────────────────────────────────────────────────── */
struct firmware {
    size_t size;
    const u8 *data;
};

int request_firmware(const struct firmware **fw, const char *name, device_t *device);
void release_firmware(const struct firmware *fw);

/* ── Network device abstraction ────────────────────────────────────────── */
#include "linux_netdev.h"

/* Error pointer encoding (for functions that return PTR or -errno) */
#define MAX_ERRNO 4095
#define IS_ERR_VALUE(x) ((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)
static inline void *ERR_PTR(long error) { return (void *)error; }
static inline long PTR_ERR(const void *ptr) { return (long)ptr; }
static inline bool IS_ERR(const void *ptr) { return IS_ERR_VALUE((unsigned long)ptr); }
static inline bool IS_ERR_OR_NULL(const void *ptr) { return !ptr || IS_ERR(ptr); }

/* ── RCU (Read-Copy-Update) stubs ──────────────────────────────────────── */
/* AzamiOS does not implement RCU; these stubs exist so drivers that use
 * rcu_read_lock() in non-critical paths compile without changes. */
#define rcu_read_lock()         do {} while(0)
#define rcu_read_unlock()       do {} while(0)
#define rcu_dereference(p)      (p)
#define rcu_assign_pointer(p,v) do { (p) = (v); } while(0)
#define synchronize_rcu()       do {} while(0)
#define call_rcu(head, func)    do { (func)(head); } while(0)

/* ── Kref (kernel reference counting) ──────────────────────────────────── */
struct kref {
    atomic_t refcount;
};

static inline void kref_init(struct kref *kref) {
    atomic_set(&kref->refcount, 1);
}

static inline void kref_get(struct kref *kref) {
    atomic_inc(&kref->refcount);
}

static inline int kref_put(struct kref *kref, void (*release)(struct kref *)) {
    if (atomic_dec_and_test(&kref->refcount)) {
        release(kref);
        return 1;
    }
    return 0;
}

/* ── Bitmap operations ─────────────────────────────────────────────────── */
#define BITS_PER_LONG      64
#ifndef BIT
#define BIT(nr)            (1UL << (nr))
#endif
#define BIT_MASK(nr)       (1UL << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)       ((nr) / BITS_PER_LONG)
#define BITS_TO_LONGS(nr)  DIV_ROUND_UP(nr, BITS_PER_LONG)
#define DECLARE_BITMAP(name, bits)  unsigned long name[BITS_TO_LONGS(bits)]

static inline void set_bit(int nr, volatile unsigned long *addr) {
    __atomic_or_fetch(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST);
}

static inline void clear_bit(int nr, volatile unsigned long *addr) {
    __atomic_and_fetch(&addr[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_SEQ_CST);
}

static inline int test_bit(int nr, const volatile unsigned long *addr) {
    return 1UL & (addr[BIT_WORD(nr)] >> (nr % BITS_PER_LONG));
}

static inline int test_and_set_bit(int nr, volatile unsigned long *addr) {
    unsigned long old = __atomic_fetch_or(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST);
    return !!(old & BIT_MASK(nr));
}

static inline int test_and_clear_bit(int nr, volatile unsigned long *addr) {
    unsigned long old = __atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_SEQ_CST);
    return !!(old & BIT_MASK(nr));
}

static inline unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size) {
    for (unsigned long i = 0; i < BITS_TO_LONGS(size); i++) {
        if (~addr[i]) {
            unsigned long bit = i * BITS_PER_LONG + __builtin_ctzl(~addr[i]);
            return bit < size ? bit : size;
        }
    }
    return size;
}

static inline unsigned long find_first_bit(const unsigned long *addr, unsigned long size) {
    for (unsigned long i = 0; i < BITS_TO_LONGS(size); i++) {
        if (addr[i]) {
            unsigned long bit = i * BITS_PER_LONG + __builtin_ctzl(addr[i]);
            return bit < size ? bit : size;
        }
    }
    return size;
}

/* ── IDR / IDA (integer ID allocator) ──────────────────────────────────── */
/* Simplified integer ID allocator using a bitmap. Max 1024 IDs. */
#define IDR_MAX 1024
typedef struct {
    DECLARE_BITMAP(bitmap, IDR_MAX);
    void *ptrs[IDR_MAX];
    spinlock_t lock;
} idr_t;

static inline void idr_init(idr_t *idr) {
    for (int i = 0; i < (int)BITS_TO_LONGS(IDR_MAX); i++) idr->bitmap[i] = 0;
    for (int i = 0; i < IDR_MAX; i++) idr->ptrs[i] = NULL;
    spinlock_init(&idr->lock);
}

static inline int idr_alloc(idr_t *idr, void *ptr) {
    spinlock_lock(&idr->lock);
    unsigned long bit = find_first_zero_bit(idr->bitmap, IDR_MAX);
    if (bit >= IDR_MAX) { spinlock_unlock(&idr->lock); return -1; }
    set_bit((int)bit, idr->bitmap);
    idr->ptrs[bit] = ptr;
    spinlock_unlock(&idr->lock);
    return (int)bit;
}

static inline void *idr_find(idr_t *idr, int id) {
    if (id < 0 || id >= IDR_MAX) return NULL;
    return test_bit(id, idr->bitmap) ? idr->ptrs[id] : NULL;
}

static inline void idr_remove(idr_t *idr, int id) {
    if (id < 0 || id >= IDR_MAX) return;
    spinlock_lock(&idr->lock);
    clear_bit(id, idr->bitmap);
    idr->ptrs[id] = NULL;
    spinlock_unlock(&idr->lock);
}

static inline void idr_destroy(idr_t *idr) {
    for (int i = 0; i < IDR_MAX; i++) idr->ptrs[i] = NULL;
    for (int i = 0; i < (int)BITS_TO_LONGS(IDR_MAX); i++) idr->bitmap[i] = 0;
}

/* ── Scatter-gather list ───────────────────────────────────────────────── */
/* struct scatterlist, sg_table, sg_init_one(), sg_init_table() and
 * for_each_sg() are already provided by kernel/mm/dma.h (included above
 * via the DMA API section). The following are additional SG accessors that
 * Linux drivers may use but dma.h does not currently export. */
#ifndef sg_dma_address
#define sg_dma_address(sg) ((sg)->dma_address)
#endif
#ifndef sg_dma_len
#define sg_dma_len(sg)     ((sg)->dma_length)
#endif

/* ── Notifier chain stubs ──────────────────────────────────────────────── */
struct notifier_block {
    int (*notifier_call)(struct notifier_block *, unsigned long, void *);
    struct notifier_block *next;
    int priority;
};

#define NOTIFY_DONE     0x0000
#define NOTIFY_OK       0x0001
#define NOTIFY_STOP_MASK 0x8000

static inline int register_reboot_notifier(struct notifier_block *nb) { (void)nb; return 0; }
static inline int unregister_reboot_notifier(struct notifier_block *nb) { (void)nb; return 0; }

/* ── Power management stubs ────────────────────────────────────────────── */
#define PM_SUSPEND_ON     0
#define PM_SUSPEND_MEM    3
#define PM_SUSPEND_STANDBY 2
#define pm_runtime_get_sync(dev)    0
#define pm_runtime_put(dev)         do {} while(0)
#define pm_runtime_put_sync(dev)    do {} while(0)
#define pm_runtime_set_active(dev)  do {} while(0)
#define pm_runtime_enable(dev)      do {} while(0)
#define pm_runtime_disable(dev)     do {} while(0)

/* ── Misc Linux compat ─────────────────────────────────────────────────── */
/* Device model stubs */
typedef struct class { const char *name; } class_t;
#define class_create(owner, name) ({ static class_t __cls = { .name = name }; &__cls; })
#define class_destroy(cls) do {} while(0)
#define device_create(cls, parent, devt, drvdata, fmt, ...) NULL
#define device_destroy(cls, devt) do {} while(0)

#define MKDEV(ma, mi) (((ma) << 20) | (mi))
#define MAJOR(dev)    ((unsigned int)((dev) >> 20))
#define MINOR(dev)    ((unsigned int)((dev) & 0xfffff))

/* Register/character device stubs */
#define alloc_chrdev_region(dev, baseminor, count, name) 0
#define unregister_chrdev_region(from, count) do {} while(0)

struct cdev {
    void *owner;
};
#define cdev_init(cdev, fops) do { (cdev)->owner = NULL; } while(0)
#define cdev_add(cdev, dev, count) 0
#define cdev_del(cdev) do {} while(0)

/* Refcount (hardened reference counting) */
typedef atomic_t refcount_t;
#define REFCOUNT_INIT(n) ATOMIC_INIT(n)
#define refcount_set(r, n) atomic_set(r, n)
#define refcount_read(r)   atomic_read(r)
#define refcount_inc(r)    atomic_inc(r)
#define refcount_dec_and_test(r) atomic_dec_and_test(r)

/* simple_strtoul and friends */
static inline unsigned long simple_strtoul(const char *cp, char **endp, unsigned int base) {
    unsigned long result = 0;
    if (!cp) return 0;
    while (*cp == ' ' || *cp == '\t') cp++;
    if (base == 0) {
        if (*cp == '0') {
            cp++;
            if (*cp == 'x' || *cp == 'X') { base = 16; cp++; }
            else base = 8;
        } else base = 10;
    }
    while (*cp) {
        unsigned int digit;
        if (*cp >= '0' && *cp <= '9') digit = *cp - '0';
        else if (*cp >= 'a' && *cp <= 'f') digit = *cp - 'a' + 10;
        else if (*cp >= 'A' && *cp <= 'F') digit = *cp - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        cp++;
    }
    if (endp) *endp = (char *)cp;
    return result;
}
