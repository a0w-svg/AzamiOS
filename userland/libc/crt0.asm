; ============================================================================
; AzamiOS Userspace C Runtime Startup (crt0.asm)
; System V AMD64 ABI Process Entry Point
; ============================================================================

[bits 64]
global _start
extern main
extern exit
extern __libc_init

section .text
_start:
    ; Terminate stack trace frame (clear RBP)
    xor rbp, rbp

    ; Stack layout set up by kernel:
    ; [rsp] = argc
    ; [rsp + 8] = argv[0]
    ; ...
    ; [rsp + 8*(argc+1)] = NULL
    ; [rsp + 8*(argc+2)] = envp[0]

    ; Pop argc into R12
    pop r12

    ; R13 = argv pointer
    mov r13, rsp

    ; R14 = envp pointer = &argv[argc + 1]
    lea r14, [r13 + r12*8 + 8]

    ; Align stack to 16 bytes for System V AMD64 ABI
    and rsp, -16

    ; Initialize libc environment
    mov rdi, r12
    mov rsi, r13
    mov rdx, r14
    call __libc_init

    ; Call main(argc, argv, envp)
    mov rdi, r12
    mov rsi, r13
    mov rdx, r14
    call main

    ; Exit with return code from main
    mov rdi, rax
    call exit

    ; Fallback sys_exit if exit() returns
    mov rax, 60  ; SYS_exit
    syscall
    hlt
