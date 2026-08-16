/* ============================================================================
 * AzamiOS Userspace — System Reboot Header (sys/reboot.h)
 * File: userland/libc/include/sys/reboot.h
 * ============================================================================ */
#pragma once

#define RB_AUTOBOOT     0x01234567
#define RB_HALT_SYSTEM  0xCDEF0123
#define RB_ENABLE_CAD   0x89ABCDEF
#define RB_DISABLE_CAD  0x00000000
#define RB_POWER_OFF    0x4321FEDC

int reboot(int cmd);
