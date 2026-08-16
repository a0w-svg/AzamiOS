/* ============================================================================
 * AzamiOS — Parallel Port (LPT) Driver
 * File: drivers/char/lpt.h
 * ============================================================================ */
#pragma once
#include "../../include/azami/types.h"

/* Common parallel port bases */
#define LPT1_PORT 0x378
#define LPT2_PORT 0x278

/* IOCTL Commands */
#define LPT_RESET       0x5001
#define LPT_GET_STATUS  0x5002

/* Status bits (returned by LPT_GET_STATUS) */
#define LPT_STATUS_BUSY      0x80  /* 0 = Busy, 1 = Ready (inverted by hardware usually) */
#define LPT_STATUS_ACK       0x40
#define LPT_STATUS_PAPER_OUT 0x20
#define LPT_STATUS_SELECT_IN 0x10
#define LPT_STATUS_ERROR     0x08

/** lpt_register_devfs() — Register LPT devices to devfs. */
void lpt_register_devfs(void);
