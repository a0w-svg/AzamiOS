; =============================================================================
; AzamiOS — Context Switch Assembly Stub (x86_64)
; File: arch/x86_64/cpu/switch_to.asm
;
; void switch_to_asm(u64 *old_rsp, u64 new_rsp);
; RDI = old_rsp (pointer to u64 slot holding old thread's kernel_rsp)
; RSI = new_rsp (new thread's kernel_rsp value to load into RSP)
; =============================================================================

bits 64
section .text

global switch_to_asm
global fpu_save_asm
global fpu_restore_asm

; void fpu_save_asm(void *fpu_state);
; RDI = pointer to 16-byte aligned 512-byte area
fpu_save_asm:
    test rdi, rdi
    jz .done_save
    fxsave64 [rdi]
.done_save:
    ret

; void fpu_restore_asm(const void *fpu_state);
; RDI = pointer to 16-byte aligned 512-byte area
fpu_restore_asm:
    test rdi, rdi
    jz .done_restore
    fxrstor64 [rdi]
.done_restore:
    ret

switch_to_asm:
    ; Push callee-saved (non-volatile) System V AMD64 registers onto current stack
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Save current RSP pointer into the address passed in RDI
    mov [rdi], rsp

    ; Load new thread's stack pointer from RSI
    mov rsp, rsi

    ; Pop callee-saved registers of the new thread
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; Return to new thread's RIP (top of stack after popping registers)
    ret

; =============================================================================
; thread_entry_trampoline
; Called when a kernel thread starts for the first time.
; R15 = entry function (passed to RDI)
; R14 = argument (passed to RSI)
; =============================================================================
global thread_entry_trampoline
global user_thread_entry_trampoline
extern sched_post_switch
extern isr_restore_stub

user_thread_entry_trampoline:
    call sched_post_switch
    jmp isr_restore_stub

thread_entry_trampoline:
    call sched_post_switch
    sti
    mov rdi, r14   ; arg1 for fn is arg (which is in R14)
    mov rax, r15   ; fn is in R15
    test rax, rax
    jz .halt_loop
    call rax       ; call fn(arg)
    extern sched_exit_thread
    call sched_exit_thread
.halt_loop:
    cli
    hlt
    jmp .halt_loop
