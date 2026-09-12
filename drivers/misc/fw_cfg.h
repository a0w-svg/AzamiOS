/* ============================================================================
 * AzamiOS — QEMU Firmware Configuration (fw_cfg) Driver
 * File: drivers/misc/fw_cfg.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../fs/vfs.h"

/* Standard I/O ports for x86 fw_cfg */
#define FW_CFG_PORT_SEL      0x510
#define FW_CFG_PORT_DATA     0x511
#define FW_CFG_PORT_DMA      0x514

/* Standard fw_cfg selector keys */
#define FW_CFG_SIGNATURE     0x0000
#define FW_CFG_ID            0x0001
#define FW_CFG_UUID          0x0002
#define FW_CFG_RAM_SIZE      0x0003
#define FW_CFG_NOGRAPHIC     0x0004
#define FW_CFG_NB_CPUS       0x0005
#define FW_CFG_MACHINE_ID    0x0006
#define FW_CFG_KERNEL_ADDR   0x0007
#define FW_CFG_KERNEL_SIZE   0x0008
#define FW_CFG_KERNEL_CMDLINE 0x0009
#define FW_CFG_INITRD_ADDR   0x000A
#define FW_CFG_INITRD_SIZE   0x000B
#define FW_CFG_BOOT_DEVICE   0x000C
#define FW_CFG_FILE_DIR      0x0019

/* Feature flags returned in FW_CFG_ID */
#define FW_CFG_VERSION_TRAD  0x01
#define FW_CFG_VERSION_DMA   0x02

/* File name length in directory entry */
#define FW_CFG_MAX_FILE_NAME 56
#define FW_CFG_MAX_FILES     128

typedef struct __attribute__((packed)) {
    u32 size;       /* big-endian */
    u16 select;     /* big-endian */
    u16 reserved;
    char name[FW_CFG_MAX_FILE_NAME];
} fw_cfg_file_t;

typedef struct {
    char name[FW_CFG_MAX_FILE_NAME];
    u32  size;
    u16  select;
} fw_cfg_entry_t;

/* Linux-compatible fw_cfg ioctls */
#define FW_CFG_IOC_MAGIC     'r'
#define FW_CFG_IOC_SELECT    0x40027201  /* _IOW('r', 1, uint16_t) */

void fw_cfg_init(void);
bool fw_cfg_is_present(void);
void fw_cfg_select(u16 key);
void fw_cfg_read(void *buf, size_t len);
u32  fw_cfg_get_file_count(void);
const fw_cfg_entry_t *fw_cfg_get_file_by_name(const char *name);
const fw_cfg_entry_t *fw_cfg_get_file_by_index(u32 index);
