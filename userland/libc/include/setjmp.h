/* ============================================================================
 * AzamiOS Userspace — Non-Local Jumps (setjmp.h)
 * File: userland/libc/include/setjmp.h
 * ============================================================================ */
#pragma once

typedef struct {
    unsigned long __rbx;
    unsigned long __rsp;
    unsigned long __rbp;
    unsigned long __r12;
    unsigned long __r13;
    unsigned long __r14;
    unsigned long __r15;
    unsigned long __rip;
} __jmp_buf_tag;

typedef __jmp_buf_tag jmp_buf[1];
typedef __jmp_buf_tag sigjmp_buf[1];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

int  sigsetjmp(sigjmp_buf env, int savesigs);
void siglongjmp(sigjmp_buf env, int val) __attribute__((noreturn));
