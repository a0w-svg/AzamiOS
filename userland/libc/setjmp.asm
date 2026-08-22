; ============================================================================
; AzamiOS Userspace — POSIX setjmp / longjmp (setjmp.asm)
; File: userland/libc/setjmp.asm
; ============================================================================

[BITS 64]
global setjmp
global longjmp
global sigsetjmp
global siglongjmp
global _setjmp
global _longjmp

section .text

; ----------------------------------------------------------------------------
; int setjmp(jmp_buf env) / int _setjmp(jmp_buf env)
; RDI = env (pointer to __jmp_buf_tag)
; ----------------------------------------------------------------------------
setjmp:
_setjmp:
    mov [rdi + 0],  rbx
    lea rdx, [rsp + 8]      ; rsp before call
    mov [rdi + 8],  rdx
    mov [rdi + 16], rbp
    mov [rdi + 24], r12
    mov [rdi + 32], r13
    mov [rdi + 40], r14
    mov [rdi + 48], r15
    mov rdx, [rsp]          ; return RIP
    mov [rdi + 56], rdx

    xor eax, eax            ; return 0 on initial call
    ret

; ----------------------------------------------------------------------------
; void longjmp(jmp_buf env, int val) / void _longjmp(jmp_buf env, int val)
; RDI = env
; RSI = val (return value)
; ----------------------------------------------------------------------------
longjmp:
_longjmp:
    mov eax, esi
    test eax, eax
    jnz .val_ok
    mov eax, 1              ; longjmp must not return 0
.val_ok:
    mov rbx, [rdi + 0]
    mov rsp, [rdi + 8]
    mov rbp, [rdi + 16]
    mov r12, [rdi + 24]
    mov r13, [rdi + 32]
    mov r14, [rdi + 40]
    mov r15, [rdi + 48]
    mov rdx, [rdi + 56]     ; target RIP
    jmp rdx

; ----------------------------------------------------------------------------
; int sigsetjmp(sigjmp_buf env, int savesigs)
; ----------------------------------------------------------------------------
sigsetjmp:
    jmp setjmp

; ----------------------------------------------------------------------------
; void siglongjmp(sigjmp_buf env, int val)
; ----------------------------------------------------------------------------
siglongjmp:
    jmp longjmp
