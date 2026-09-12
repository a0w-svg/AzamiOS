/* ============================================================================
 * AzamiOS — QEMU Firmware Configuration (fw_cfg) Driver
 * File: drivers/misc/fw_cfg.c
 *
 * Implements the QEMU Firmware Configuration interface via I/O ports 0x510
 * and 0x511. Provides /dev/fw_cfg for userspace and in-kernel introspection of
 * hypervisor state, system UUID, CPU configuration, and firmware files
 * (such as ACPI and SMBIOS tables).
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "fw_cfg.h"
#include "../base/platform.h"
#include "../../fs/vfs.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/cpu/spinlock.h"

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

static spinlock_t g_fw_cfg_lock = SPINLOCK_INIT;
static bool       g_fw_cfg_present = false;
static u16        g_fw_cfg_current_key = 0xFFFF;
static u32        g_fw_cfg_current_offset = 0;

static u8         g_fw_cfg_uuid[16];
static u16        g_fw_cfg_nb_cpus = 0;
static u64        g_fw_cfg_ram_size = 0;

static fw_cfg_entry_t *g_fw_cfg_files = NULL;
static u32             g_fw_cfg_file_count = 0;

/* Helper: big-endian conversion */
static inline u32 be32_to_cpu(u32 val)
{
    return __builtin_bswap32(val);
}

static inline u16 be16_to_cpu(u16 val)
{
    return __builtin_bswap16(val);
}

void fw_cfg_select(u16 key)
{
    outw(FW_CFG_PORT_SEL, key);
    g_fw_cfg_current_key = key;
    g_fw_cfg_current_offset = 0;
}

void fw_cfg_read(void *buf, size_t len)
{
    u8 *p = (u8 *)buf;
    for (size_t i = 0; i < len; i++) {
        p[i] = inb(FW_CFG_PORT_DATA);
    }
    g_fw_cfg_current_offset += (u32)len;
}

bool fw_cfg_is_present(void)
{
    return g_fw_cfg_present;
}

u32 fw_cfg_get_file_count(void)
{
    return g_fw_cfg_file_count;
}

const fw_cfg_entry_t *fw_cfg_get_file_by_name(const char *name)
{
    if (!g_fw_cfg_files || !name) return NULL;
    for (u32 i = 0; i < g_fw_cfg_file_count; i++) {
        if (strcmp(g_fw_cfg_files[i].name, name) == 0) {
            return &g_fw_cfg_files[i];
        }
    }
    return NULL;
}

const fw_cfg_entry_t *fw_cfg_get_file_by_index(u32 index)
{
    if (!g_fw_cfg_files || index >= g_fw_cfg_file_count) return NULL;
    return &g_fw_cfg_files[index];
}

/* ── /dev/fw_cfg character device file operations ────────────────────────── */

static s64 dev_fw_cfg_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp;
    if (!g_fw_cfg_present || !buf || len == 0) return 0;

    irqflags_t flags = spinlock_lock_irqsave(&g_fw_cfg_lock);

    /* If an offset was provided and differs, advance or re-select */
    if (offset && *offset != g_fw_cfg_current_offset) {
        if (g_fw_cfg_current_key != 0xFFFF) {
            fw_cfg_select(g_fw_cfg_current_key);
            for (u64 i = 0; i < *offset; i++) {
                (void)inb(FW_CFG_PORT_DATA);
            }
            g_fw_cfg_current_offset = (u32)*offset;
        }
    }

    fw_cfg_read(buf, len);

    if (offset) {
        *offset = g_fw_cfg_current_offset;
    }

    spinlock_unlock_irqrestore(&g_fw_cfg_lock, flags);
    return (s64)len;
}

static s64 dev_fw_cfg_ioctl(struct file *filp, u32 cmd, u64 arg)
{
    (void)filp;
    if (!g_fw_cfg_present) return -ENODEV;

    if (cmd == FW_CFG_IOC_SELECT) {
        u16 key = (u16)(arg & 0xFFFF);
        irqflags_t flags = spinlock_lock_irqsave(&g_fw_cfg_lock);
        fw_cfg_select(key);
        spinlock_unlock_irqrestore(&g_fw_cfg_lock, flags);
        return 0;
    }

    return -ENOTTY;
}

static file_operations_t g_fw_cfg_fops = {
    .read    = dev_fw_cfg_read,
    .write   = NULL,
    .open    = NULL,
    .release = NULL,
    .ioctl   = dev_fw_cfg_ioctl,
};

/* ── Platform Driver Binding ─────────────────────────────────────────────── */

static int fw_cfg_probe(platform_device_t *pdev)
{
    (void)pdev;
    irqflags_t flags = spinlock_lock_irqsave(&g_fw_cfg_lock);

    /* 1. Probe signature */
    fw_cfg_select(FW_CFG_SIGNATURE);
    char sig[5] = {0};
    fw_cfg_read(sig, 4);

    if (memcmp(sig, "QEMU", 4) != 0) {
        spinlock_unlock_irqrestore(&g_fw_cfg_lock, flags);
        pr_debug("[FWCFG] Signature mismatch ('%c%c%c%c') — fw_cfg not present\n",
                 sig[0], sig[1], sig[2], sig[3]);
        return -ENODEV;
    }

    g_fw_cfg_present = true;

    /* 2. Read feature bitmap */
    u32 features = 0;
    fw_cfg_select(FW_CFG_ID);
    fw_cfg_read(&features, sizeof(features));

    /* 3. Read System UUID */
    fw_cfg_select(FW_CFG_UUID);
    fw_cfg_read(g_fw_cfg_uuid, 16);

    /* 4. Read CPU Count & RAM Size */
    fw_cfg_select(FW_CFG_NB_CPUS);
    fw_cfg_read(&g_fw_cfg_nb_cpus, sizeof(g_fw_cfg_nb_cpus));

    fw_cfg_select(FW_CFG_RAM_SIZE);
    fw_cfg_read(&g_fw_cfg_ram_size, sizeof(g_fw_cfg_ram_size));

    /* 5. Read Firmware Directory */
    fw_cfg_select(FW_CFG_FILE_DIR);
    u32 count_be = 0;
    fw_cfg_read(&count_be, sizeof(count_be));
    u32 raw_count = be32_to_cpu(count_be);

    if (raw_count > 0 && raw_count < 1024) {
        g_fw_cfg_files = (fw_cfg_entry_t *)kzalloc(raw_count * sizeof(fw_cfg_entry_t));
        if (g_fw_cfg_files) {
            g_fw_cfg_file_count = raw_count;
            for (u32 i = 0; i < raw_count; i++) {
                fw_cfg_file_t raw_file;
                fw_cfg_read(&raw_file, sizeof(raw_file));
                strncpy(g_fw_cfg_files[i].name, raw_file.name, FW_CFG_MAX_FILE_NAME - 1);
                g_fw_cfg_files[i].size = be32_to_cpu(raw_file.size);
                g_fw_cfg_files[i].select = be16_to_cpu(raw_file.select);
            }
        }
    }

    spinlock_unlock_irqrestore(&g_fw_cfg_lock, flags);

    devfs_register_device("fw_cfg", &g_fw_cfg_fops, NULL);

    pr_debug("[FWCFG] QEMU Firmware Configuration detected (features=0x%x, cpus=%u)\n",
             features, (unsigned)g_fw_cfg_nb_cpus);
    pr_debug("[FWCFG] System UUID: %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
             g_fw_cfg_uuid[0], g_fw_cfg_uuid[1], g_fw_cfg_uuid[2], g_fw_cfg_uuid[3],
             g_fw_cfg_uuid[4], g_fw_cfg_uuid[5], g_fw_cfg_uuid[6], g_fw_cfg_uuid[7],
             g_fw_cfg_uuid[8], g_fw_cfg_uuid[9], g_fw_cfg_uuid[10], g_fw_cfg_uuid[11],
             g_fw_cfg_uuid[12], g_fw_cfg_uuid[13], g_fw_cfg_uuid[14], g_fw_cfg_uuid[15]);
    pr_debug("[FWCFG] Enumerated %u firmware files in QEMU directory (/dev/fw_cfg)\n",
             g_fw_cfg_file_count);

    return 0;
}

static platform_driver_t g_fw_cfg_pdrv = {
    .drv   = { .name = "qemu_fw_cfg" },
    .probe = fw_cfg_probe,
};

void fw_cfg_init(void)
{
    platform_driver_register(&g_fw_cfg_pdrv);
}
