/* ============================================================================
 * AzamiOS — i6300esb: Intel 6300ESB watchdog timer
 * File: drivers/watchdog/i6300esb.c
 *
 * The southbridge watchdog found on Intel 6300ESB boards and emulated by QEMU's
 * `-device i6300esb`.  Two cascaded counters: the first expiring signals, the
 * second reboots the machine.  Both are programmed through a small MMIO
 * window whose timer registers are protected by a two-step unlock sequence,
 * while enable/lock live in PCI configuration space.
 *
 * Exposed as /dev/watchdog with the Linux watchdog ioctl interface, and armed
 * only while that device is open — probing must never leave a timer running
 * that nothing is pinging.  Closing honours the "magic close" convention: a
 * write of 'V' before close disarms cleanly.
 *
 * Deviation from Linux: closing *without* the magic character also disarms,
 * rather than leaving the machine to reset.  Nothing in this system supervises
 * the watchdog across a process exit, so a forgotten descriptor would only
 * ever be a surprise reboot.  Setting nowayout restores the Linux behaviour.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "../base/pci_bus.h"
#include "../../fs/vfs.h"
#include "../../kernel/uaccess.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

/* ── MMIO registers ──────────────────────────────────────────────────────── */
#define ESB_TIMER1_REG   0x00   /* u32: preload for the pre-timeout counter */
#define ESB_TIMER2_REG   0x04   /* u32: preload for the reset counter       */
#define ESB_GINTSR_REG   0x08   /* u32: general interrupt status            */
#define ESB_RELOAD_REG   0x0C   /* u16: unlock / reload / timeout flag      */

/* ── PCI configuration registers ─────────────────────────────────────────── */
#define ESB_CONFIG_REG   0x60   /* u16 */
#define ESB_LOCK_REG     0x68   /* u8  */

/* ── Register bits ───────────────────────────────────────────────────────── */
#define ESB_UNLOCK1      0x80
#define ESB_UNLOCK2      0x86
#define ESB_WDT_RELOAD   (1U << 8)
#define ESB_WDT_TIMEOUT  (1U << 9)
#define ESB_WDT_FUNC     (1U << 2)   /* lock reg: watchdog, not free-run    */
#define ESB_WDT_ENABLE   (1U << 1)
#define ESB_WDT_LOCK     (1U << 0)

/* ── Linux watchdog ioctl interface ──────────────────────────────────────── */
#define WDIOC_GETSUPPORT     0x80285700
#define WDIOC_GETSTATUS      0x80045701
#define WDIOC_GETBOOTSTATUS  0x80045702
#define WDIOC_SETOPTIONS     0x80045704
#define WDIOC_KEEPALIVE      0x80045705
#define WDIOC_SETTIMEOUT     0xC0045706
#define WDIOC_GETTIMEOUT     0x80045707

#define WDIOF_CARDRESET      0x0020
#define WDIOF_SETTIMEOUT     0x0080
#define WDIOF_MAGICCLOSE     0x0100
#define WDIOF_KEEPALIVEPING  0x8000

#define WDIOS_DISABLECARD    0x0001
#define WDIOS_ENABLECARD     0x0002

struct watchdog_info {
    u32 options;
    u32 firmware_version;
    u8  identity[32];
};

#define ESB_TIMEOUT_MIN      1
#define ESB_TIMEOUT_MAX      2046
#define ESB_TIMEOUT_DEFAULT  30

typedef struct esb_device {
    volatile u8 *mmio;
    u8   bus, slot, func;
    u32  timeout;          /* seconds */
    bool running;
    bool opened;
    bool expect_close;
    bool boot_reset;       /* the previous boot ended in a watchdog reset */
    bool nowayout;
    spinlock_t lock;
} esb_device_t;

static esb_device_t g_esb;

/* ── Register access ─────────────────────────────────────────────────────── */

static inline void esb_writew(u32 off, u16 val)
{
    *(volatile u16 *)(g_esb.mmio + off) = val;
}

static inline u16 esb_readw(u32 off)
{
    return *(volatile u16 *)(g_esb.mmio + off);
}

static inline void esb_writel(u32 off, u32 val)
{
    *(volatile u32 *)(g_esb.mmio + off) = val;
}

/* The timer preload registers ignore writes unless immediately preceded by
 * this two-step sequence, which must be repeated before every write. */
static void esb_unlock(void)
{
    esb_writew(ESB_RELOAD_REG, ESB_UNLOCK1);
    esb_writew(ESB_RELOAD_REG, ESB_UNLOCK2);
}

static void esb_set_timeout(u32 seconds)
{
    /* The counters tick at the PCI clock divided by 2^15, so one second is
     * 1 << 9 counts with the divider configured below. */
    u32 preload = seconds << 9;

    esb_unlock();
    esb_writel(ESB_TIMER1_REG, preload);
    esb_unlock();
    esb_writel(ESB_TIMER2_REG, preload);
    g_esb.timeout = seconds;
}

static void esb_keepalive(void)
{
    esb_unlock();
    esb_writew(ESB_RELOAD_REG, ESB_WDT_RELOAD);
}

static void esb_start(void)
{
    esb_set_timeout(g_esb.timeout);
    esb_keepalive();

    u8 lock = ESB_WDT_ENABLE | ESB_WDT_FUNC;
    if (g_esb.nowayout) lock |= ESB_WDT_LOCK;
    pci_config_write8(g_esb.bus, g_esb.slot, g_esb.func, ESB_LOCK_REG, lock);

    g_esb.running = true;
    pr_debug("[I6300ESB] armed, %u second timeout\n", g_esb.timeout);
}

static void esb_stop(void)
{
    pci_config_write8(g_esb.bus, g_esb.slot, g_esb.func, ESB_LOCK_REG, 0x00);
    esb_unlock();
    esb_writew(ESB_RELOAD_REG, ESB_WDT_TIMEOUT | ESB_WDT_RELOAD);
    g_esb.running = false;
    pr_debug("[I6300ESB] disarmed\n");
}

/* ── /dev/watchdog ───────────────────────────────────────────────────────── */

static s64 esb_open(inode_t *inode, file_t *filp)
{
    (void)inode; (void)filp;

    spinlock_lock(&g_esb.lock);
    if (g_esb.opened) {
        spinlock_unlock(&g_esb.lock);
        return -(s64)EBUSY;
    }
    g_esb.opened       = true;
    g_esb.expect_close = false;
    esb_start();
    spinlock_unlock(&g_esb.lock);
    return 0;
}

static s64 esb_release(inode_t *inode, file_t *filp)
{
    (void)inode; (void)filp;

    spinlock_lock(&g_esb.lock);
    if (g_esb.nowayout && !g_esb.expect_close) {
        /* Locked mode: the hardware will not stop, so keep pinging is the
         * caller's problem — exactly as on Linux. */
        pr_debug("[I6300ESB] closed without magic character; watchdog stays armed\n");
    } else {
        if (!g_esb.expect_close) {
            pr_debug("[I6300ESB] closed without magic character; disarming anyway\n");
        }
        esb_stop();
    }
    g_esb.opened = false;
    spinlock_unlock(&g_esb.lock);
    return 0;
}

static s64 esb_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (len == 0) return 0;

    /* Any write is a keepalive; a 'V' anywhere in it arms the magic close. */
    /* Kernel buffer — see fs/vfs.h. The old copy_from_user() always failed,
     * so the magic-close 'V' was never seen and the watchdog could not be
     * disarmed cleanly. */
    const char *probe = (const char *)buf;
    size_t n = len < 64 ? len : 64;
    for (size_t i = 0; i < n; i++) {
        if (probe[i] == 'V') { g_esb.expect_close = true; break; }
    }

    spinlock_lock(&g_esb.lock);
    if (g_esb.running) esb_keepalive();
    spinlock_unlock(&g_esb.lock);
    return (s64)len;
}

static s64 esb_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)buf; (void)len; (void)offset;
    return 0;
}

static s64 esb_ioctl(file_t *filp, u32 cmd, u64 arg)
{
    (void)filp;
    void *uarg = (void *)(uintptr_t)arg;

    switch (cmd) {
    case WDIOC_GETSUPPORT: {
        struct watchdog_info info;
        memset(&info, 0, sizeof(info));
        info.options = WDIOF_SETTIMEOUT | WDIOF_MAGICCLOSE |
                       WDIOF_KEEPALIVEPING | WDIOF_CARDRESET;
        info.firmware_version = 0;
        strncpy((char *)info.identity, "i6300ESB", sizeof(info.identity) - 1);
        return copy_to_user(uarg, &info, sizeof(info)) == 0 ? 0 : -(s64)EFAULT;
    }
    case WDIOC_GETSTATUS: {
        int status = g_esb.running ? 1 : 0;
        return copy_to_user(uarg, &status, sizeof(status)) == 0 ? 0 : -(s64)EFAULT;
    }
    case WDIOC_GETBOOTSTATUS: {
        int status = g_esb.boot_reset ? WDIOF_CARDRESET : 0;
        return copy_to_user(uarg, &status, sizeof(status)) == 0 ? 0 : -(s64)EFAULT;
    }
    case WDIOC_KEEPALIVE:
        spinlock_lock(&g_esb.lock);
        if (g_esb.running) esb_keepalive();
        spinlock_unlock(&g_esb.lock);
        return 0;
    case WDIOC_SETOPTIONS: {
        int options = 0;
        if (copy_from_user(&options, uarg, sizeof(options)) != 0) return -(s64)EFAULT;
        spinlock_lock(&g_esb.lock);
        if (options & WDIOS_DISABLECARD) esb_stop();
        if (options & WDIOS_ENABLECARD)  esb_start();
        spinlock_unlock(&g_esb.lock);
        return 0;
    }
    case WDIOC_SETTIMEOUT: {
        int seconds = 0;
        if (copy_from_user(&seconds, uarg, sizeof(seconds)) != 0) return -(s64)EFAULT;
        if (seconds < ESB_TIMEOUT_MIN || seconds > ESB_TIMEOUT_MAX) return -(s64)EINVAL;

        spinlock_lock(&g_esb.lock);
        esb_set_timeout((u32)seconds);
        if (g_esb.running) esb_keepalive();
        spinlock_unlock(&g_esb.lock);

        return copy_to_user(uarg, &seconds, sizeof(seconds)) == 0 ? 0 : -(s64)EFAULT;
    }
    case WDIOC_GETTIMEOUT: {
        int seconds = (int)g_esb.timeout;
        return copy_to_user(uarg, &seconds, sizeof(seconds)) == 0 ? 0 : -(s64)EFAULT;
    }
    default:
        return -(s64)ENOTTY;
    }
}

static file_operations_t g_esb_fops = {
    .read    = esb_read,
    .write   = esb_write,
    .ioctl   = esb_ioctl,
    .open    = esb_open,
    .release = esb_release,
};

/* ── PCI binding ─────────────────────────────────────────────────────────── */

static int esb_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;
    if (g_esb.mmio) return -EBUSY;

    pci_device_info_t *info = to_pci_info(dm);
    phys_addr_t bar = pci_get_bar(dm->hal, 0);
    if (!info || !bar) return -ENODEV;

    g_esb.mmio = (volatile u8 *)vmm_map_io(bar, 0x1000);
    if (!g_esb.mmio) return -ENOMEM;

    g_esb.bus  = info->bus;
    g_esb.slot = info->slot;
    g_esb.func = info->func;
    g_esb.timeout = ESB_TIMEOUT_DEFAULT;
    spinlock_init(&g_esb.lock);

    /*
     * Config register: WDT_OUTPUT enabled, counters clocked at the PCI clock
     * divided by 2^15 (~1 kHz), and no interrupt on the first timeout — the
     * only useful response here is the reset the second counter delivers.
     */
    pci_config_write16(g_esb.bus, g_esb.slot, g_esb.func, ESB_CONFIG_REG, 0x0003);

    u8 lock = pci_config_read8(g_esb.bus, g_esb.slot, g_esb.func, ESB_LOCK_REG);
    if (lock & ESB_WDT_LOCK) {
        /* Firmware locked the timer on; it can no longer be disarmed. */
        pr_debug("[I6300ESB] timer is hardware-locked — cannot be disarmed\n");
        g_esb.nowayout = true;
        g_esb.running  = true;
    } else {
        pci_config_write8(g_esb.bus, g_esb.slot, g_esb.func, ESB_LOCK_REG, 0x00);
    }

    /* A latched timeout flag means the last boot was the watchdog's doing. */
    esb_unlock();
    if (esb_readw(ESB_RELOAD_REG) & ESB_WDT_TIMEOUT) {
        g_esb.boot_reset = true;
    }
    esb_unlock();
    esb_writew(ESB_RELOAD_REG, ESB_WDT_TIMEOUT | ESB_WDT_RELOAD);

    devfs_register_device("watchdog", &g_esb_fops, &g_esb);
    dm_set_drvdata(dm, &g_esb);

    pr_debug("[I6300ESB] watchdog at 0x%016llx — /dev/watchdog, %u s default%s\n",
             (unsigned long long)bar, g_esb.timeout,
             g_esb.boot_reset ? ", last boot was a watchdog reset" : "");
    return 0;
}

static void esb_remove(dm_device_t *dm)
{
    (void)dm;
    if (g_esb.mmio && g_esb.running && !g_esb.nowayout) esb_stop();
}

static const pci_device_id_t esb_pci_ids[] = {
    { PCI_DEVICE(0x8086, 0x25AB) },   /* Intel 6300ESB watchdog */
    { 0 }
};

static pci_driver_t esb_pci_driver = {
    .drv      = { .name = "i6300esb" },
    .id_table = esb_pci_ids,
    .probe    = esb_probe,
    .remove   = esb_remove,
};

void i6300esb_init(void)
{
    pci_driver_register(&esb_pci_driver);
}
