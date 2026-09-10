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
global xsave_save_asm
global xsave_restore_asm
global xsaveopt_save_asm
global xsavec_save_asm
global xsave_probe_asm
global mwait_probe_asm

; void fpu_save_asm(void *fpu_state);
; RDI = pointer to 16-byte aligned 512-byte FXSAVE area
fpu_save_asm:
    test rdi, rdi
    jz .done_save
    fxsave64 [rdi]
.done_save:
    ret

; void fpu_restore_asm(const void *fpu_state);
; RDI = pointer to 16-byte aligned 512-byte FXSAVE area
fpu_restore_asm:
    test rdi, rdi
    jz .done_restore
    fxrstor64 [rdi]
.done_restore:
    ret

; void xsave_save_asm(void *area, u64 mask);
; RDI = 64-byte aligned XSAVE area, RSI = component bitmap (EDX:EAX)
; Saves x87 + SSE (XMM) + AVX (YMM) + any other enabled XCR0 state.
xsave_save_asm:
    test rdi, rdi
    jz .xs_done
    mov  rax, rsi
    mov  rdx, rsi
    shr  rdx, 32
    xsave64 [rdi]
.xs_done:
    ret

; void xsaveopt_save_asm(void *area, u64 mask);
; Same contract as xsave_save_asm, but the CPU may skip writing components that
; are still in their init state or unmodified since the last XRSTOR from this
; very address. Only valid because each thread owns its save area for life.
xsaveopt_save_asm:
    test rdi, rdi
    jz .xo_done
    mov  rax, rsi
    mov  rdx, rsi
    shr  rdx, 32
    xsaveopt64 [rdi]
.xo_done:
    ret

; void xsavec_save_asm(void *area, u64 mask);
; Compacted format: components are packed back-to-back and XCOMP_BV[63] is set,
; so the image is smaller than the standard layout and skips holes for state the
; CPU has but we do not enable. XRSTOR reads both layouts, so the restore path
; is shared.
xsavec_save_asm:
    test rdi, rdi
    jz .xc_done
    mov  rax, rsi
    mov  rdx, rsi
    shr  rdx, 32
    xsavec64 [rdi]
.xc_done:
    ret

; int xsave_probe_asm(void *area, u64 mask, u32 variant);
; RDI = 64-byte aligned scratch area, RSI = XCR0 mask, EDX = XSAVE_VARIANT_*
; Returns 0 if the instruction executed, 1 if it raised #UD.
;
; CPUID is not proof that an instruction exists: QEMU's TCG advertises XSAVEC in
; leaf 0xD and then #UDs on it. Every entry point below has an .extable fixup,
; so a bad instruction returns an error instead of panicking the boot, and
; cpu_enable_features_bsp() can fall back to a variant that really is there.
xsave_probe_asm:
    mov  r8d, edx           ; variant (RDX is about to hold the mask high half)
    mov  rax, rsi
    mov  rdx, rsi
    shr  rdx, 32
    cmp  r8d, 3             ; XSAVE_VARIANT_XSAVEC
    jne  .try_opt
.probe_xsavec:
    xsavec64 [rdi]
    jmp  .ok
.try_opt:
    cmp  r8d, 2             ; XSAVE_VARIANT_XSAVEOPT
    jne  .try_plain
.probe_xsaveopt:
    xsaveopt64 [rdi]
    jmp  .ok
.try_plain:
.probe_xsave:
    xsave64 [rdi]
.ok:
    xor  eax, eax
    ret
.probe_fault:
    mov  eax, 1
    ret

; int mwait_probe_asm(void *scratch);
; Returns 0 if MONITOR/MWAIT executed, 1 if either faulted.
;
; Arming the monitor and then storing to the watched line means the following
; MWAIT has a break event waiting for it and returns immediately, so the probe
; cannot park the boot CPU. (MWAIT with no armed monitor is also architecturally
; a NOP, so this is belt and braces.) Both instructions carry .extable fixups
; because a hypervisor may advertise MONITOR in CPUID and still fault on them.
mwait_probe_asm:
    mov  rax, rdi
    xor  ecx, ecx
    xor  edx, edx
.probe_monitor:
    monitor
    mov  qword [rdi], 1
    xor  eax, eax
    xor  ecx, ecx
.probe_mwait:
    mwait
    xor  eax, eax
    ret
.probe_fault:
    mov  eax, 1
    ret

; void xsave_restore_asm(const void *area, u64 mask);
xsave_restore_asm:
    test rdi, rdi
    jz .xr_done
    mov  rax, rsi
    mov  rdx, rsi
    shr  rdx, 32
    xrstor64 [rdi]
.xr_done:
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

; =============================================================================
; Exception table entries for xsave_probe_asm.
; The linker script gathers every .extable input section between
; __extable_start and __extable_end; search_extable() matches the faulting RIP
; against the first quadword of each 16-byte pair.
; =============================================================================
section .extable
align 8
    dq xsave_probe_asm.probe_xsavec
    dq xsave_probe_asm.probe_fault

    dq xsave_probe_asm.probe_xsaveopt
    dq xsave_probe_asm.probe_fault

    dq xsave_probe_asm.probe_xsave
    dq xsave_probe_asm.probe_fault

    dq mwait_probe_asm.probe_monitor
    dq mwait_probe_asm.probe_fault

    dq mwait_probe_asm.probe_mwait
    dq mwait_probe_asm.probe_fault
