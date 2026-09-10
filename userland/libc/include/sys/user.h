/* ============================================================================
 * AzamiOS Userspace — User Register Structures (sys/user.h)
 * File: userland/libc/include/sys/user.h
 * ============================================================================ */
#pragma once

#include <stdint.h>

struct user_regs_struct {
    unsigned long r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8, rax, rcx, rdx;
    unsigned long rsi, rdi, orig_rax, rip, cs, eflags, rsp, ss;
    unsigned long fs_base, gs_base, ds, es, fs, gs;
};

struct user_fpregs_struct {
    uint16_t cwd;
    uint16_t swd;
    uint16_t ftw;
    uint16_t fop;
    uint64_t rip;
    uint64_t rdp;
    uint32_t mxcsr;
    uint32_t mxcr_mask;
    uint32_t st_space[32];
    uint32_t xmm_space[64];
    uint32_t padding[24];
};
