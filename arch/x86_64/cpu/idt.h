/* ============================================================================
 * AzamiOS — Interrupt Descriptor Table (x86_64)
 * File: arch/x86_64/cpu/idt.h
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"
#include "../../../include/azami/defs.h"

/* ── IDT gate types ───────────────────────────────────────────────────────── */
#define IDT_TYPE_INT_GATE  0xE  /* 64-bit interrupt gate (IF cleared on entry) */
#define IDT_TYPE_TRAP_GATE 0xF  /* 64-bit trap gate     (IF preserved)         */

/* ── IST indices (0 = no IST; 1–7 = use IST stack N from the TSS) ─────────── */
#define IST_NONE   0
#define IST_DF     1   /* #DF double fault */
#define IST_NMI    2   /* NMI */
#define IST_MC     3   /* #MC machine check */
#define IST_DBG    4   /* #DB debug / breakpoint */

/* ── 16-byte IDT gate descriptor ─────────────────────────────────────────── */
typedef struct __packed {
    u16 offset_low;    /* Handler address bits  0–15 */
    u16 selector;      /* Code segment selector (SEL_KERNEL_CODE = 0x08) */
    u8  ist;           /* Bits [2:0] = IST index; bits [7:3] = reserved (0) */
    u8  type_attr;     /* Gate type (0xE/0xF), DPL, P */
    u16 offset_mid;    /* Handler address bits 16–31 */
    u32 offset_high;   /* Handler address bits 32–63 */
    u32 reserved;      /* Must be zero */
} idt_entry_t;

BUILD_ASSERT(sizeof(idt_entry_t) == 16, "IDT gate must be 16 bytes");

/* ── IDTR pointer ─────────────────────────────────────────────────────────── */
typedef struct __packed {
    u16 limit;
    u64 base;
} idt_ptr_t;

/* ── x86_64 saved register frame (pushed by ISR stubs in isr.asm) ─────────── *
 *                                                                               *
 * Stack layout after isr_common_stub runs (low addresses at top):              *
 *                                                                               *
 *   OFFSET  FIELD           SOURCE                                             *
 *   ──────  ─────────────   ─────────────────────────────────────────────────  *
 *   +0      ds              pushed by stub (mov rax,ds; push rax)             *
 *   +8      r15             pushed by stub                                     *
 *   +16     r14             pushed by stub                                     *
 *   +24     r13             pushed by stub                                     *
 *   +32     r12             pushed by stub                                     *
 *   +40     r11             pushed by stub                                     *
 *   +48     r10             pushed by stub                                     *
 *   +56     r9              pushed by stub                                     *
 *   +64     r8              pushed by stub                                     *
 *   +72     rbp             pushed by stub                                     *
 *   +80     rdi             pushed by stub                                     *
 *   +88     rsi             pushed by stub                                     *
 *   +96     rdx             pushed by stub                                     *
 *   +104    rcx             pushed by stub                                     *
 *   +112    rbx             pushed by stub                                     *
 *   +120    rax             pushed by stub                                     *
 *   +128    int_no          pushed by ISR macro (exception number)            *
 *   +136    err_code        pushed by CPU (or dummy 0 for no-error exceptions) *
 *   +144    rip             pushed by CPU on interrupt entry                   *
 *   +152    cs              pushed by CPU                                      *
 *   +160    rflags          pushed by CPU                                      *
 *   +168    rsp             pushed by CPU (only on ring switch)                *
 *   +176    ss              pushed by CPU (only on ring switch)                *
 *                                                                               *
 * Note: int_no is pushed BEFORE err_code in the stub; the CPU pushes err_code *
 * last in its own push sequence. int_no is pushed first then jmp common_stub   *
 * where err_code is already on the stack (CPU pushed it for errors, or our     *
 * stub pushed a dummy 0 before int_no for no-error vectors).                   *
 *                                                                               *
 * The ordering in the struct below matches the push order (first push = lowest *
 * address = first field in struct, since the stack grows downward):            *
 * ─────────────────────────────────────────────────────────────────────────── */
typedef struct __packed {
    u64 ds;
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    /* Pushed by the ISR macro: */
    u64 int_no;
    u64 err_code;
    /* Pushed by the CPU: */
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;   /* only valid on ring transition (user → kernel) */
    u64 ss;    /* only valid on ring transition */
} pt_regs_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/** idt_init() — Populate all 256 IDT entries and load the IDTR. */
void idt_init(void);

/** idt_set_gate() — Install a single IDT gate.
 *  @vector   Interrupt vector (0–255).
 *  @handler  64-bit address of the ISR stub.
 *  @ist      IST index (0 = use current RSP, 1–7 = IST stack from TSS).
 *  @dpl      Descriptor Privilege Level (0 = kernel-only, 3 = user callable).
 *  @type     IDT_TYPE_INT_GATE or IDT_TYPE_TRAP_GATE.
 */
void idt_set_gate(u8 vector, uintptr_t handler, u8 ist, u8 dpl, u8 type);

/* Called from C interrupt dispatcher (isr.c) */
void exception_handler(pt_regs_t *r);
void irq_handler(pt_regs_t *r);
