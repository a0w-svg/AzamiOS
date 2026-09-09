; ============================================================================
; AzamiOS — Minimal PIE crt0 for Phase 5a dynamic-linking test apps
; File: userland/examples/dltest_crt0.asm
;
; A PIE test app can't link against userland/libc/crt0.o or libc.a — both
; are built -fno-pic -fno-pie, the wrong relocation model for code meant to
; run at a load address chosen at runtime. Same minimal pop-argc/compute-
; argv-envp shape as every other crt0 in this codebase, just calling main()
; directly with no libc/TLS init in between (this test app doesn't need
; either).
; ============================================================================

[bits 64]
global _start
extern main

section .text
_start:
    xor rbp, rbp
    pop r12                    ; argc
    mov r13, rsp                ; argv
    lea r14, [r13 + r12*8 + 8]  ; envp
    and rsp, -16

    mov rdi, r12
    mov rsi, r13
    mov rdx, r14
    call main wrt ..plt        ; see ldso/crt0.asm's comment on the same line

    mov rdi, rax
    mov rax, 60                 ; SYS_exit
    syscall
    hlt
