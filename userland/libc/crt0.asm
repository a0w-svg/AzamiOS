; ============================================================================
; AzamiOS Userspace C Runtime Startup (crt0.asm)
; System V AMD64 ABI Process Entry Point
; ============================================================================

[bits 64]
global _start
extern main
extern exit

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

    ; Pop argc into RDI
    pop rdi

    ; RSI = argv pointer
    mov rsi, rsp

    ; RDX = envp pointer = &argv[argc + 1]
    lea rdx, [rsi + rdi*8 + 8]

    ; Align stack to 16 bytes for System V AMD64 ABI
    and rsp, -16

    ; Call main(argc, argv, envp)
    call main

    ; Exit with return code from main
    mov rdi, rax
    call exit

    ; Fallback sys_exit if exit() returns
    mov rax, 60  ; SYS_exit
    syscall
    hlt
