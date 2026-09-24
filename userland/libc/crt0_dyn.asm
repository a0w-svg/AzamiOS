; ============================================================================
; AzamiOS Userspace — Dynamic-linked C Runtime Startup (crt0_dyn.asm)
; System V AMD64 ABI Process Entry Point, PIE + ld-azami.so variant
;
; Same stack-unpacking and call sequence as crt0.asm (__init_tls, then
; __libc_init, then main, then exit), but every one of those symbols now
; lives in libc.so — a separate ET_DYN object this PIE executable only
; DT_NEEDED-depends on, resolved by ld-azami.so at load time — rather than
; being statically linked into this same executable the way crt0.asm's
; targets always are. A plain `call sym` compiles to a PC32 relocation,
; which only works when the callee's final address is known (or trivially
; PC-relative-reachable) at static link time; here it isn't; the call needs
; to go through the PLT/GOT so ld-azami.so's JUMP_SLOT/GLOB_DAT relocations
; can point it at libc.so's actual runtime load address. `call sym wrt
; ..plt` is exactly the idiom userland/ldso/crt0.asm and
; userland/examples/dltest_crt0.asm already use for the same reason —
; confirmed to link and run correctly there before this file copied it.
;
; Deliberately a separate file rather than editing crt0.asm in place: every
; existing statically-linked app (~150 of them) depends on crt0.asm working
; exactly as it does today, and that file is the highest-blast-radius file
; in the whole libc. This file is only ever linked into an app built with
; the DYNAMIC=1 path in userland/Makefile.
; ============================================================================

[bits 64]
global _start
extern main
extern exit
extern __libc_init
extern __init_tls

section .text
_start:
    xor rbp, rbp

    ; Stack layout set up by kernel — identical to crt0.asm:
    ; [rsp] = argc, [rsp+8..] = argv[0..], NULL, envp[0..], NULL, auxv...
    pop r12                     ; argc
    mov r13, rsp                 ; argv
    lea r14, [r13 + r12*8 + 8]   ; envp = &argv[argc+1]

    and rsp, -16

    ; Set up the main thread's TLS block (%fs) before anything else touches
    ; a __thread variable such as errno. Under ld-azami.so this is usually
    ; already a no-op on the libc.so side (setup_static_tls() in ldso.c has
    ; already pointed %fs at the combined block before jumping here — see
    ; tls.c's __init_tls(), which detects that and steps aside), but the
    ; call must still happen: it also stashes auxv for dlopen()/dlsym() to
    ; find ld.so's exports later (see ldso_bridge.c).
    mov rdi, r14
    call __init_tls wrt ..plt

    mov rdi, r12
    mov rsi, r13
    mov rdx, r14
    call __libc_init wrt ..plt

    mov rdi, r12
    mov rsi, r13
    mov rdx, r14
    call main wrt ..plt

    mov rdi, rax
    call exit wrt ..plt

    ; Fallback sys_exit if exit() returns
    mov rax, 60  ; SYS_exit
    syscall
    hlt
