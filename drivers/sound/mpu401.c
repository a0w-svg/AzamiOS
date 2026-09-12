/* ============================================================================
 * AzamiOS — MPU-401 MIDI Interface Driver
 * File: drivers/sound/mpu401.c
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "mpu401.h"
#include "../../fs/vfs.h"

static u16  g_mpu_data = MPU401_DEFAULT_DATA_PORT;
static u16  g_mpu_cmd  = MPU401_DEFAULT_CMD_PORT;
static bool g_mpu_ready = false;

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

void mpu401_write_byte(u8 byte)
{
    if (!g_mpu_ready) return;

    for (int timeout = 0; timeout < 10000; timeout++) {
        if (!(inb(g_mpu_cmd) & MPU401_STATUS_OUTPUT_BUSY)) {
            outb(g_mpu_data, byte);
            return;
        }
        cpu_pause();
    }
}

/* ── Character Device Operations for /dev/midi ───────────────────────────── */

static s64 mpu_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (!buf || len == 0 || !g_mpu_ready) return 0;

    const u8 *p = (const u8 *)buf;
    for (size_t i = 0; i < len; i++) {
        mpu401_write_byte(p[i]);
    }
    return (s64)len;
}

static s64 mpu_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (!buf || len == 0 || !g_mpu_ready) return 0;

    u8 *p = (u8 *)buf;
    size_t count = 0;

    while (count < len) {
        if (inb(g_mpu_cmd) & MPU401_STATUS_INPUT_EMPTY) break;
        p[count++] = inb(g_mpu_data);
    }
    return (s64)count;
}

static file_operations_t g_mpu_fops = {
    .read  = mpu_read,
    .write = mpu_write,
    .ioctl = NULL,
};

/* ── Platform Driver ─────────────────────────────────────────────────────── */

static int mpu401_probe(platform_device_t *pdev)
{
    u16 data_port = MPU401_DEFAULT_DATA_PORT;
    const platform_resource_t *res = platform_get_resource(pdev, PLATFORM_RES_IO, 0);
    if (res && res->start > 0) {
        data_port = (u16)res->start;
    }

    u16 cmd_port = data_port + 1;

    /* Verify presence: read status, send UART mode command */
    u8 status = inb(cmd_port);
    if (status == 0xFF) {
        /* Floating bus, no controller present */
        return -ENODEV;
    }

    /* Put controller in UART mode */
    for (int retry = 0; retry < 3; retry++) {
        outb(cmd_port, MPU401_CMD_UART_MODE);
        for (int timeout = 0; timeout < 5000; timeout++) {
            if (!(inb(cmd_port) & MPU401_STATUS_INPUT_EMPTY)) {
                u8 ack = inb(data_port);
                if (ack == MPU401_ACK) goto found;
            }
            cpu_pause();
        }
    }

found:
    g_mpu_data  = data_port;
    g_mpu_cmd   = cmd_port;
    g_mpu_ready = true;

    devfs_register_device("midi", &g_mpu_fops, NULL);
    devfs_register_device("midi0", &g_mpu_fops, NULL);

    pr_debug("[MPU401] MPU-401 MIDI controller ready at I/O 0x%04X (/dev/midi, /dev/midi0)\n",
             data_port);
    return 0;
}

static platform_driver_t g_mpu401_driver = {
    .drv = {
        .name = "mpu401",
    },
    .probe  = mpu401_probe,
    .remove = NULL,
};

int mpu401_init(void)
{
    return platform_driver_register(&g_mpu401_driver);
}
