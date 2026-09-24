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
/* LINUX_REBOOT_CMD_KEXEC — jump into whatever image kexec_file_load()
 * staged, instead of resetting the hardware. See kernel/kexec.c. */
#define RB_KEXEC        0x45584543

int reboot(int cmd);

/* kexec_file_load(2) — stage a kernel image for a later reboot(RB_KEXEC).
 * @initrd_fd must be -1: this kernel has no initrd boot mechanism. @flags
 * must be 0: no KEXEC_FILE_* behaviour is implemented. */
int kexec_file_load(int kernel_fd, int initrd_fd, unsigned long cmdline_len,
                     const char *cmdline_ptr, unsigned long flags);
