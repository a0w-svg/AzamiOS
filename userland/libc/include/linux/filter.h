/* ============================================================================
 * AzamiOS — <linux/filter.h> compatibility subset
 *
 * Classic BPF: struct sock_filter, struct sock_fprog, and the BPF_STMT/
 * BPF_JUMP macros real seccomp filters (and this kernel's interpreter,
 * kernel/security/seccomp.c) are built from. Same layout and opcode values
 * as the real Linux header, so a filter written the normal way — a plain
 * array of BPF_STMT()/BPF_JUMP() entries passed to seccomp(2) or
 * prctl(PR_SET_SECCOMP, ...) — needs no AzamiOS-specific changes.
 * ============================================================================ */
#ifndef _LINUX_FILTER_H
#define _LINUX_FILTER_H

#include <stdint.h>

struct sock_filter {
    uint16_t code;
    uint8_t  jt;
    uint8_t  jf;
    uint32_t k;
};

struct sock_fprog {
    unsigned short       len;
    struct sock_filter   *filter;
};

/* ── Instruction classes ─────────────────────────────────────────────────── */
#define BPF_LD    0x00
#define BPF_LDX   0x01
#define BPF_ALU   0x04
#define BPF_JMP   0x05
#define BPF_RET   0x06
#define BPF_MISC  0x07

/* ── BPF_LD/BPF_LDX addressing modes ────────────────────────────────────── */
#define BPF_W     0x00   /* 4-byte load width — the only width seccomp uses */
#define BPF_ABS   0x20
#define BPF_IMM   0x00
#define BPF_MEM   0x60

/* ── BPF_ALU/BPF_JMP operations ─────────────────────────────────────────── */
#define BPF_ADD   0x00
#define BPF_SUB   0x10
#define BPF_MUL   0x20
#define BPF_DIV   0x30
#define BPF_OR    0x40
#define BPF_AND   0x50
#define BPF_LSH   0x60
#define BPF_RSH   0x70
#define BPF_NEG   0x80
#define BPF_MOD   0x90
#define BPF_XOR   0xa0

#define BPF_JA    0x00
#define BPF_JEQ   0x10
#define BPF_JGT   0x20
#define BPF_JGE   0x30
#define BPF_JSET  0x40

#define BPF_K     0x00
#define BPF_X     0x08

/* ── BPF_RET ─────────────────────────────────────────────────────────────── */
#define BPF_A     0x10

/* ── BPF_MISC ────────────────────────────────────────────────────────────── */
#define BPF_TAX   0x00
#define BPF_TXA   0x80

/* Build one instruction: BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offset) loads a
 * word, BPF_STMT(BPF_RET|BPF_K, action) returns. */
#define BPF_STMT(code, k) { (unsigned short)(code), 0, 0, (uint32_t)(k) }

/* Build one conditional-jump instruction: on true, skip @jt instructions
 * forward; on false, skip @jf. */
#define BPF_JUMP(code, k, jt, jf) \
    { (unsigned short)(code), (uint8_t)(jt), (uint8_t)(jf), (uint32_t)(k) }

#endif /* _LINUX_FILTER_H */
