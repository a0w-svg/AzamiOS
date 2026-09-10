/* ============================================================================
 * AzamiOS — I2C / SMBus core and i2c-dev
 * File: drivers/i2c/i2c-core.c
 *
 * Adapter registration plus the /dev/i2c-N character device.  The device is
 * the Linux i2c-dev interface: a descriptor carries a target address set with
 * I2C_SLAVE, read()/write() do plain byte transfers to it, and the two
 * ioctls that matter — I2C_RDWR for raw I2C message vectors and I2C_SMBUS for
 * single SMBus transactions — are passed through to the adapter's algorithm.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "i2c.h"
#include "../../fs/vfs.h"
#include "../../kernel/uaccess.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/cpu/spinlock.h"

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

#define I2C_MAX_ADAPTERS   8
#define I2C_RDWR_MAX_MSGS  16

static i2c_adapter_t *g_adapters;
static u32            g_adapter_count;
static dm_class_t     g_i2c_class = { .name = "i2c" };
static spinlock_t     g_i2c_lock = SPINLOCK_INIT;

/* Per-open state: which chip on the bus this descriptor is talking to. */
typedef struct i2c_client_file {
    i2c_adapter_t *adap;
    u16            addr;
    bool           addr_set;
    bool           tenbit;
} i2c_client_file_t;

/* ── Adapter registry ────────────────────────────────────────────────────── */

void i2c_core_init(void)
{
    dm_class_register(&g_i2c_class);
}

i2c_adapter_t *i2c_adapter_nth(u32 n)
{
    u32 i = 0;
    for (i2c_adapter_t *a = g_adapters; a; a = a->next, i++) {
        if (i == n) return a;
    }
    return NULL;
}

u32 i2c_adapter_count(void) { return g_adapter_count; }

static i2c_adapter_t *i2c_adapter_by_nr(int nr)
{
    for (i2c_adapter_t *a = g_adapters; a; a = a->next) {
        if (a->nr == nr) return a;
    }
    return NULL;
}

/* ── i2c-dev file operations ─────────────────────────────────────────────── */

static file_operations_t g_i2cdev_fops;

static s64 i2cdev_open(inode_t *inode, file_t *filp)
{
    i2c_adapter_t *adap = inode ? (i2c_adapter_t *)inode->i_private : NULL;
    if (!adap) return -(s64)ENODEV;

    i2c_client_file_t *cf = (i2c_client_file_t *)kzalloc(sizeof(i2c_client_file_t));
    if (!cf) return -(s64)ENOMEM;

    cf->adap = adap;
    filp->private_data = cf;
    return 0;
}

static s64 i2cdev_release(inode_t *inode, file_t *filp)
{
    (void)inode;
    if (filp && filp->private_data) {
        kfree(filp->private_data);
        filp->private_data = NULL;
    }
    return 0;
}

/* Plain read()/write() are byte transfers to the address set by I2C_SLAVE. */
static s64 i2cdev_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)offset;
    i2c_client_file_t *cf = filp ? (i2c_client_file_t *)filp->private_data : NULL;
    if (!cf || !buf || len == 0) return -(s64)EINVAL;
    if (!cf->addr_set) return -(s64)EINVAL;

    const i2c_algorithm_t *algo = cf->adap->algo;
    if (algo->master_xfer) {
        u8 kbuf[64];
        size_t n = len < sizeof(kbuf) ? len : sizeof(kbuf);
        struct i2c_msg msg = { .addr = cf->addr, .flags = I2C_M_RD, .len = (u16)n, .buf = kbuf };
        int ret = algo->master_xfer(cf->adap, &msg, 1);
        if (ret < 0) return (s64)ret;
        /* Kernel buffer — see fs/vfs.h. */
        memcpy(buf, kbuf, n);
        return (s64)n;
    }
    if (algo->smbus_xfer) {
        /* SMBus-only controllers can still do a receive-byte. */
        union i2c_smbus_data d;
        int ret = algo->smbus_xfer(cf->adap, cf->addr, 0, I2C_SMBUS_READ, 0,
                                   I2C_SMBUS_BYTE, &d);
        if (ret < 0) return (s64)ret;
        memcpy(buf, &d.byte, 1);
        return 1;
    }
    return -(s64)ENOTSUP;
}

static s64 i2cdev_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)offset;
    i2c_client_file_t *cf = filp ? (i2c_client_file_t *)filp->private_data : NULL;
    if (!cf || !buf || len == 0) return -(s64)EINVAL;
    if (!cf->addr_set) return -(s64)EINVAL;

    const i2c_algorithm_t *algo = cf->adap->algo;
    if (algo->master_xfer) {
        u8 kbuf[64];
        size_t n = len < sizeof(kbuf) ? len : sizeof(kbuf);
        /* Kernel buffer — see fs/vfs.h. */
        memcpy(kbuf, buf, n);
        struct i2c_msg msg = { .addr = cf->addr, .flags = 0, .len = (u16)n, .buf = kbuf };
        int ret = algo->master_xfer(cf->adap, &msg, 1);
        return ret < 0 ? (s64)ret : (s64)n;
    }
    if (algo->smbus_xfer) {
        u8 b;
        memcpy(&b, buf, 1);
        union i2c_smbus_data d = { .byte = 0 };
        int ret = algo->smbus_xfer(cf->adap, cf->addr, 0, I2C_SMBUS_WRITE, b,
                                   I2C_SMBUS_BYTE, &d);
        return ret < 0 ? (s64)ret : 1;
    }
    return -(s64)ENOTSUP;
}

static s64 i2cdev_ioctl_rdwr(i2c_client_file_t *cf, u64 arg)
{
    const i2c_algorithm_t *algo = cf->adap->algo;
    if (!algo->master_xfer) return -(s64)ENOTSUP;

    struct i2c_rdwr_ioctl_data rdwr;
    if (copy_from_user(&rdwr, (void *)(uintptr_t)arg, sizeof(rdwr)) != 0) return -(s64)EFAULT;
    if (rdwr.nmsgs == 0 || rdwr.nmsgs > I2C_RDWR_MAX_MSGS) return -(s64)EINVAL;

    struct i2c_msg umsgs[I2C_RDWR_MAX_MSGS];
    if (copy_from_user(umsgs, (void *)(uintptr_t)rdwr.msgs,
                       rdwr.nmsgs * sizeof(struct i2c_msg)) != 0) {
        return -(s64)EFAULT;
    }

    /* Bounce every message body into the kernel: the algorithm must not be
     * handed user pointers to dereference. */
    static const u32 MSG_MAX = 256;
    u8 *bufs[I2C_RDWR_MAX_MSGS];
    struct i2c_msg kmsgs[I2C_RDWR_MAX_MSGS];
    u32 allocated = 0;
    s64 ret = 0;

    for (u32 i = 0; i < rdwr.nmsgs; i++) {
        if (umsgs[i].len > MSG_MAX) { ret = -(s64)EINVAL; goto out; }
        bufs[i] = (u8 *)kzalloc(umsgs[i].len ? umsgs[i].len : 1);
        if (!bufs[i]) { ret = -(s64)ENOMEM; goto out; }
        allocated++;

        kmsgs[i] = umsgs[i];
        kmsgs[i].buf = bufs[i];

        if (!(umsgs[i].flags & I2C_M_RD) && umsgs[i].len) {
            if (copy_from_user(bufs[i], umsgs[i].buf, umsgs[i].len) != 0) {
                ret = -(s64)EFAULT;
                goto out;
            }
        }
    }

    ret = algo->master_xfer(cf->adap, kmsgs, (int)rdwr.nmsgs);
    if (ret < 0) goto out;

    for (u32 i = 0; i < rdwr.nmsgs; i++) {
        if ((umsgs[i].flags & I2C_M_RD) && umsgs[i].len) {
            if (copy_to_user(umsgs[i].buf, bufs[i], umsgs[i].len) != 0) {
                ret = -(s64)EFAULT;
                goto out;
            }
        }
    }

out:
    for (u32 i = 0; i < allocated; i++) kfree(bufs[i]);
    return ret;
}

static s64 i2cdev_ioctl_smbus(i2c_client_file_t *cf, u64 arg)
{
    const i2c_algorithm_t *algo = cf->adap->algo;
    if (!algo->smbus_xfer) return -(s64)ENOTSUP;

    struct i2c_smbus_ioctl_data req;
    if (copy_from_user(&req, (void *)(uintptr_t)arg, sizeof(req)) != 0) return -(s64)EFAULT;
    if (req.size > I2C_SMBUS_I2C_BLOCK_DATA) return -(s64)EINVAL;

    union i2c_smbus_data data;
    memset(&data, 0, sizeof(data));

    /* A quick command carries no payload; everything else does, and a write
     * needs it copied in first. */
    bool has_data = (req.size != I2C_SMBUS_QUICK) &&
                    !(req.size == I2C_SMBUS_BYTE && req.read_write == I2C_SMBUS_READ);
    if (has_data && req.data) {
        if (copy_from_user(&data, (void *)(uintptr_t)req.data, sizeof(data)) != 0) {
            return -(s64)EFAULT;
        }
    }

    int ret = algo->smbus_xfer(cf->adap, cf->addr, cf->tenbit ? I2C_M_TEN : 0,
                               (char)req.read_write, req.command, (int)req.size, &data);
    if (ret < 0) return (s64)ret;

    if (req.read_write == I2C_SMBUS_READ && req.size != I2C_SMBUS_QUICK && req.data) {
        if (copy_to_user((void *)(uintptr_t)req.data, &data, sizeof(data)) != 0) {
            return -(s64)EFAULT;
        }
    }
    return 0;
}

static s64 i2cdev_ioctl(file_t *filp, u32 cmd, u64 arg)
{
    i2c_client_file_t *cf = filp ? (i2c_client_file_t *)filp->private_data : NULL;
    if (!cf) return -(s64)EBADF;

    switch (cmd) {
    case I2C_SLAVE:
    case I2C_SLAVE_FORCE:
        /* 7-bit addresses stop at 0x7F; 10-bit ones at 0x3FF. */
        if (arg > (cf->tenbit ? 0x3FFu : 0x7Fu)) return -(s64)EINVAL;
        cf->addr = (u16)arg;
        cf->addr_set = true;
        return 0;

    case I2C_TENBIT:
        cf->tenbit = arg != 0;
        return 0;

    case I2C_RETRIES:
    case I2C_TIMEOUT:
    case I2C_PEC:
        /* Accepted and ignored: transfers here are synchronous and unretried. */
        return 0;

    case I2C_FUNCS: {
        u64 funcs = cf->adap->algo->functionality ? cf->adap->algo->functionality(cf->adap) : 0;
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EFAULT;
        return copy_to_user((void *)(uintptr_t)arg, &funcs, sizeof(funcs)) == 0
               ? 0 : -(s64)EFAULT;
    }

    case I2C_RDWR:
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EFAULT;
        return i2cdev_ioctl_rdwr(cf, arg);

    case I2C_SMBUS:
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EFAULT;
        if (!cf->addr_set) return -(s64)EINVAL;
        return i2cdev_ioctl_smbus(cf, arg);

    default:
        return -(s64)ENOTTY;
    }
}

static file_operations_t g_i2cdev_fops = {
    .read    = i2cdev_read,
    .write   = i2cdev_write,
    .ioctl   = i2cdev_ioctl,
    .open    = i2cdev_open,
    .release = i2cdev_release,
};

/* ── Registration ────────────────────────────────────────────────────────── */

int i2c_add_adapter(i2c_adapter_t *adap)
{
    if (!adap || !adap->algo) return -EINVAL;
    if (g_adapter_count >= I2C_MAX_ADAPTERS) return -ENOSPC;

    spinlock_lock(&g_i2c_lock);

    int nr = 0;
    while (i2c_adapter_by_nr(nr)) nr++;
    adap->nr = nr;

    adap->next = NULL;
    i2c_adapter_t **tail = &g_adapters;
    while (*tail) tail = &(*tail)->next;
    *tail = adap;
    g_adapter_count++;

    spinlock_unlock(&g_i2c_lock);

    char node[24];
    snprintf(node, sizeof(node), "i2c-%d", adap->nr);
    devfs_register_device(node, &g_i2cdev_fops, adap);

    /* Publish in the driver model so the bus shows up under /sys/class/i2c. */
    dm_device_t *dev = dm_device_alloc(node, NULL);
    if (dev) {
        dev->parent = adap->dm;
        snprintf(dev->modalias, sizeof(dev->modalias), "i2c:%s", adap->name);
        if (dm_device_register(dev) == 0) {
            /* Linux's i2c-dev major is 89. */
            dm_device_add_class(dev, &g_i2c_class, (89u << 20) | (u32)adap->nr);
        }
    }

    u32 funcs = adap->algo->functionality ? adap->algo->functionality(adap) : 0;
    pr_debug("[I2C] adapter %d: %s (/dev/%s, funcs 0x%08x)\n",
             adap->nr, adap->name, node, funcs);
    return 0;
}

void i2c_del_adapter(i2c_adapter_t *adap)
{
    if (!adap) return;

    spinlock_lock(&g_i2c_lock);
    i2c_adapter_t **pp = &g_adapters;
    while (*pp) {
        if (*pp == adap) { *pp = adap->next; g_adapter_count--; break; }
        pp = &(*pp)->next;
    }
    spinlock_unlock(&g_i2c_lock);
}

/* ── In-kernel convenience wrapper ───────────────────────────────────────── */

int i2c_smbus_read_byte_data(i2c_adapter_t *adap, u16 addr, u8 command)
{
    if (!adap || !adap->algo->smbus_xfer) return -ENOTSUP;

    union i2c_smbus_data data;
    memset(&data, 0, sizeof(data));
    int ret = adap->algo->smbus_xfer(adap, addr, 0, I2C_SMBUS_READ, command,
                                     I2C_SMBUS_BYTE_DATA, &data);
    return ret < 0 ? ret : (int)data.byte;
}
