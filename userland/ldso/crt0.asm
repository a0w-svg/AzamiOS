; ============================================================================
; AzamiOS — ld-azami.so entry point (Phase 5a dynamic linking milestone)
; File: userland/ldso/crt0.asm
;
; Deliberately NOT userland/libc/crt0.asm: ld-azami.so cannot link against
; libc.a at all (it IS the thing that would need to resolve libc's own
; external symbols, if libc itself were ever built as a shared object — see
; ldso.c's top-of-file comment). This stub only pops argc/computes argv/envp,
; exactly like the real crt0 does, then calls ld_main(argc, argv, envp)
; instead of __libc_init()+main().
;
; ld_main() never returns in the success path — it jumps directly to the
; main executable's real entry point once relocation is done, reusing the
; exact same initial RSP the kernel handed *this* process (argc/argv/envp/
; auxv are still sitting on the stack at that point, so the main
; executable's own crt0 sees exactly what it would have seen if the kernel
; had jumped there directly with no interpreter involved at all). The
; fallback exit(1) below only fires if ld_main() fails outright — e.g. it
; couldn't open a dependency .so.
; ============================================================================

[bits 64]
global _start
extern ld_main

section .text
_start:
    xor rbp, rbp

    ; Same initial stack layout every crt0 in this codebase relies on:
    ; [rsp] = argc, [rsp+8..] = argv[0..], NULL, envp[0..], NULL, auxv...
    pop r12                 ; argc
    mov r13, rsp             ; argv
    lea r14, [r13 + r12*8 + 8] ; envp = &argv[argc+1]

    and rsp, -16
    mov rcx, rsp              ; 4th arg: the aligned initial RSP itself

    mov rdi, r12
    mov rsi, r13
    mov rdx, r14
    call ld_main wrt ..plt     ; R_X86_64_PLT32, not plain PC32 — ld -shared
                                ; rejects a bare PC32 call relocation even to
                                ; a symbol that resolves within this same
                                ; object, since hand-written asm (unlike
                                ; gcc -fpic codegen) doesn't otherwise mark
                                ; the call as PLT-safe for -shared output

    ; ld_main() only returns on failure (rax = nonzero exit code).
    mov rdi, rax
    mov rax, 60              ; SYS_exit
    syscall
    hlt
