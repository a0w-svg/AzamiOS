/* ============================================================================
 * AzamiOS — I2C / SMBus core
 * File: drivers/i2c/i2c.h
 *
 * A Linux-shaped I2C subsystem: controllers register an i2c_adapter carrying
 * an i2c_algorithm, and the core gives every adapter a /dev/i2c-N node
 * speaking the Linux i2c-dev ioctl interface (I2C_SLAVE, I2C_FUNCS, I2C_RDWR,
 * I2C_SMBUS).  A driver for a chip on the bus — or a userspace tool like
 * i2cdetect — therefore works the same way it would on Linux.
 *
 * SMBus is expressed as a distinct operation rather than synthesised from raw
 * I2C messages, because most PC controllers (the ICH SMBus block among them)
 * only implement the SMBus protocols in hardware and cannot do arbitrary I2C
 * transfers at all.
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../include/azami/defs.h"
#include "../base/base.h"

struct i2c_adapter;

/* ── Raw I2C messages ────────────────────────────────────────────────────── */
#define I2C_M_RD        0x0001
#define I2C_M_TEN       0x0010

struct i2c_msg {
    u16 addr;
    u16 flags;
    u16 len;
    u8 *buf;
};

/* ── SMBus ───────────────────────────────────────────────────────────────── */
#define I2C_SMBUS_READ   1
#define I2C_SMBUS_WRITE  0

#define I2C_SMBUS_QUICK             0
#define I2C_SMBUS_BYTE              1
#define I2C_SMBUS_BYTE_DATA         2
#define I2C_SMBUS_WORD_DATA         3
#define I2C_SMBUS_PROC_CALL         4
#define I2C_SMBUS_BLOCK_DATA        5
#define I2C_SMBUS_I2C_BLOCK_BROKEN  6
#define I2C_SMBUS_BLOCK_PROC_CALL   7
#define I2C_SMBUS_I2C_BLOCK_DATA    8

#define I2C_SMBUS_BLOCK_MAX 32

union i2c_smbus_data {
    u8  byte;
    u16 word;
    u8  block[I2C_SMBUS_BLOCK_MAX + 2];   /* block[0] is the length */
};

/* ── Functionality bits (Linux values) ───────────────────────────────────── */
#define I2C_FUNC_I2C                    0x00000001
#define I2C_FUNC_10BIT_ADDR             0x00000002
#define I2C_FUNC_SMBUS_QUICK            0x00010000
#define I2C_FUNC_SMBUS_READ_BYTE        0x00020000
#define I2C_FUNC_SMBUS_WRITE_BYTE       0x00040000
#define I2C_FUNC_SMBUS_READ_BYTE_DATA   0x00080000
#define I2C_FUNC_SMBUS_WRITE_BYTE_DATA  0x00100000
#define I2C_FUNC_SMBUS_READ_WORD_DATA   0x00200000
#define I2C_FUNC_SMBUS_WRITE_WORD_DATA  0x00400000
#define I2C_FUNC_SMBUS_PROC_CALL        0x00800000
#define I2C_FUNC_SMBUS_READ_BLOCK_DATA  0x01000000
#define I2C_FUNC_SMBUS_WRITE_BLOCK_DATA 0x02000000

/* ── i2c-dev ioctls ──────────────────────────────────────────────────────── */
#define I2C_RETRIES     0x0701
#define I2C_TIMEOUT     0x0702
#define I2C_SLAVE       0x0703
#define I2C_TENBIT      0x0704
#define I2C_FUNCS       0x0705
#define I2C_SLAVE_FORCE 0x0706
#define I2C_RDWR        0x0707
#define I2C_PEC         0x0708
#define I2C_SMBUS       0x0720

struct i2c_rdwr_ioctl_data {
    u64 msgs;      /* struct i2c_msg __user * */
    u32 nmsgs;
};

struct i2c_smbus_ioctl_data {
    u8  read_write;
    u8  command;
    u32 size;
    u64 data;      /* union i2c_smbus_data __user * */
};

/* ── Adapters ────────────────────────────────────────────────────────────── */

typedef struct i2c_algorithm {
    /** Raw I2C transfer.  NULL on controllers that only do SMBus. */
    int (*master_xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);

    /** One SMBus transaction. */
    int (*smbus_xfer)(struct i2c_adapter *adap, u16 addr, u16 flags,
                      char read_write, u8 command, int size,
                      union i2c_smbus_data *data);

    /** Bitmask of I2C_FUNC_* the controller supports. */
    u32 (*functionality)(struct i2c_adapter *adap);
} i2c_algorithm_t;

typedef struct i2c_adapter {
    char  name[48];
    int   nr;                       /* N in /dev/i2c-N */
    const i2c_algorithm_t *algo;
    void *algo_data;
    dm_device_t *dm;
    struct i2c_adapter *next;
} i2c_adapter_t;

/** i2c_core_init() — register the "i2c" device class.  Call before adapters. */
void i2c_core_init(void);

/**
 * i2c_add_adapter(adap) — publish a controller.
 *
 * Assigns the next free bus number, creates /dev/i2c-N and joins the "i2c"
 * class so the adapter appears under /sys/class/i2c.
 */
int i2c_add_adapter(i2c_adapter_t *adap);

/** i2c_del_adapter(adap) — withdraw a controller. */
void i2c_del_adapter(i2c_adapter_t *adap);

/** i2c_adapter_nth(n) / i2c_adapter_count() — enumerate registered adapters. */
i2c_adapter_t *i2c_adapter_nth(u32 n);
u32            i2c_adapter_count(void);

/** i2c_smbus_read_byte_data(adap, addr, cmd) → value, or negative errno. */
int i2c_smbus_read_byte_data(i2c_adapter_t *adap, u16 addr, u8 command);

/** i801_init() — register the ICH/PIIX4 SMBus controller driver. */
void i801_init(void);
