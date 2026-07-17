; setjmp64.asm — AzamiOS libc: non-local jumps (x86_64 64-bit)
[BITS 64]

global setjmp
global longjmp

section .text

; int setjmp(jmp_buf env)
; RDI = env pointer
; Saves: rbx, rsp, rbp, r12, r13, r14, r15, rip
setjmp:
    mov [rdi + 0], rbx
    lea rax, [rsp + 8]      ; caller's RSP before call
    mov [rdi + 8], rax
    mov [rdi + 16], rbp
    mov [rdi + 24], r12
    mov [rdi + 32], r13
    mov [rdi + 40], r14
    mov [rdi + 48], r15
    mov rax, [rsp]          ; return address (RIP)
    mov [rdi + 56], rax
    xor eax, eax            ; return 0
    ret

; void longjmp(jmp_buf env, int val)
; RDI = env pointer, ESI = val
longjmp:
    mov eax, esi
    test eax, eax
    jnz .val_ok
    inc eax                 ; return 1 if 0 passed
.val_ok:
    mov rbx, [rdi + 0]
    mov rsp, [rdi + 8]
    mov rbp, [rdi + 16]
    mov r12, [rdi + 24]
    mov r13, [rdi + 32]
    mov r14, [rdi + 40]
    mov r15, [rdi + 48]
    jmp qword [rdi + 56]
