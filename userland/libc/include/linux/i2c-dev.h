/* ============================================================================
 * AzamiOS libc — <linux/i2c-dev.h>: the /dev/i2c-N interface
 * ============================================================================ */
#ifndef _LINUX_I2C_DEV_H
#define _LINUX_I2C_DEV_H

#include <stdint.h>
#include <stddef.h>

/* ── ioctls ──────────────────────────────────────────────────────────────── */
#define I2C_RETRIES     0x0701  /* retries on ACK failure (accepted, ignored) */
#define I2C_TIMEOUT     0x0702  /* transfer timeout       (accepted, ignored) */
#define I2C_SLAVE       0x0703  /* set the target chip address                */
#define I2C_TENBIT      0x0704  /* select 10-bit addressing                   */
#define I2C_FUNCS       0x0705  /* read the adapter's capability mask         */
#define I2C_SLAVE_FORCE 0x0706  /* set the address even if a driver claims it */
#define I2C_RDWR        0x0707  /* combined raw I2C transfer                  */
#define I2C_PEC         0x0708  /* SMBus packet error checking                */
#define I2C_SMBUS       0x0720  /* one SMBus transaction                      */

/* ── Adapter capabilities ────────────────────────────────────────────────── */
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

/* ── Raw I2C ─────────────────────────────────────────────────────────────── */
#define I2C_M_RD  0x0001
#define I2C_M_TEN 0x0010

struct i2c_msg {
    uint16_t addr;
    uint16_t flags;
    uint16_t len;
    uint8_t *buf;
};

struct i2c_rdwr_ioctl_data {
    struct i2c_msg *msgs;
    unsigned int    nmsgs;
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
    uint8_t  byte;
    uint16_t word;
    uint8_t  block[I2C_SMBUS_BLOCK_MAX + 2];   /* block[0] is the length */
};

struct i2c_smbus_ioctl_data {
    uint8_t               read_write;
    uint8_t               command;
    unsigned int          size;
    union i2c_smbus_data *data;
};

#endif /* _LINUX_I2C_DEV_H */
