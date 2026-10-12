; ============================================================================
; AzamiOS Userspace — clone() and vfork() (clone.asm)
; File: userland/libc/clone.asm
;
; clone(fn, child_stack, flags, arg, ptid, tls, ctid):
;   The child must not return into the caller's frame (it runs on a different
;   stack), so fn and arg are parked on the new stack before the syscall; the
;   child pops them, calls fn(arg) and exits with its return value.
;
; vfork():
;   The child shares the parent's stack, so the return address must not live
;   on it across the syscall: it is held in RDX (preserved by the kernel in
;   the child's saved registers as well) and pushed back afterwards, once
;   in the child and once in the parent when it resumes.
; ============================================================================

[BITS 64]
global clone
global vfork

extern __syscall_ret

section .text

clone:
    test rdi, rdi
    jz   .einval
    test rsi, rsi
    jz   .einval
    and  rsi, -16
    sub  rsi, 16
    mov  [rsi + 8], rcx         ; arg
    mov  [rsi], rdi             ; fn
    mov  rdi, rdx               ; flags
    mov  rdx, r8                ; parent_tid
    mov  r10, [rsp + 8]         ; child_tid (7th argument, on the stack)
    mov  r8,  r9                ; tls
    mov  eax, 56                ; SYS_clone
    syscall
    test rax, rax
    jz   .child
    js   .err
    ret
.child:
    xor  ebp, ebp
    pop  rax                    ; fn
    pop  rdi                    ; arg
    call rax
    mov  edi, eax
    mov  eax, 60                ; SYS_exit
    syscall
.einval:
    mov  rax, -22
.err:
    mov  rdi, rax
    jmp  __syscall_ret wrt ..plt

vfork:
    pop  rdx
    mov  eax, 58                ; SYS_vfork
    syscall
    push rdx
    test rax, rax
    jns  .vok
    mov  rdi, rax
    jmp  __syscall_ret wrt ..plt
.vok:
    ret

section .note.GNU-stack noalloc noexec nowrite progbits

