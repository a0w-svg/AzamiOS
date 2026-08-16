; =============================================================================
; AzamiOS — ISR Stubs (x86_64, NASM)
; File: arch/x86_64/cpu/isr.asm
;
; This file generates 256 ISR entry stubs plus the isr_common_stub.
;
; Frame layout after isr_common_stub pushes everything (see idt.h pt_regs_t):
;
;   [RSP+0  ]  ds      (ax pushed via "mov rax,ds; push rax")
;   [RSP+8  ]  r15
;   [RSP+16 ]  r14
;   [RSP+24 ]  r13
;   [RSP+32 ]  r12
;   [RSP+40 ]  r11
;   [RSP+48 ]  r10
;   [RSP+56 ]  r9
;   [RSP+64 ]  r8
;   [RSP+72 ]  rbp
;   [RSP+80 ]  rdi
;   [RSP+88 ]  rsi
;   [RSP+96 ]  rdx
;   [RSP+104]  rcx
;   [RSP+112]  rbx
;   [RSP+120]  rax
;   [RSP+128]  int_no   ← pushed by ISR macro
;   [RSP+136]  err_code ← pushed by CPU (or dummy 0 by macro for no-error vecs)
;   [RSP+144]  rip      ← CPU
;   [RSP+152]  cs       ← CPU
;   [RSP+160]  rflags   ← CPU
;   [RSP+168]  rsp      ← CPU (only on ring switch)
;   [RSP+176]  ss       ← CPU (only on ring switch)
;
; Calling convention:
;   isr_common_stub calls:
;     void isr_dispatch(pt_regs_t *regs)  — RDI = RSP (pointer to frame)
;   isr_dispatch does NOT return a new RSP. Context switches happen by
;   modifying the saved RSP field inside the frame (done by the scheduler)
;   and the IRETQ restores it naturally.
; =============================================================================

bits 64
section .text

extern isr_dispatch     ; defined in isr.c

; =============================================================================
; Macro: ISR stub for vectors WITHOUT a CPU-pushed error code.
; We push a dummy zero so the frame is uniform.
; =============================================================================
%macro ISR_NOERRCODE 1
global isr_%1
isr_%1:
    push qword 0        ; dummy err_code (8 bytes on x86_64 stack)
    push qword %1       ; int_no
    jmp  isr_common_stub
%endmacro

; =============================================================================
; Macro: ISR stub for vectors WITH a CPU-pushed error code (already on stack).
; =============================================================================
%macro ISR_ERRCODE 1
global isr_%1
isr_%1:
    push qword %1       ; int_no (err_code already on stack from CPU)
    jmp  isr_common_stub
%endmacro

; ── CPU Exception stubs (vectors 0–31) ───────────────────────────────────────
ISR_NOERRCODE  0   ; #DE  Divide Error
ISR_NOERRCODE  1   ; #DB  Debug
ISR_NOERRCODE  2   ;      NMI Interrupt      ← uses IST2
ISR_NOERRCODE  3   ; #BP  Breakpoint         ← DPL=3 (user callable via int 3)
ISR_NOERRCODE  4   ; #OF  Overflow
ISR_NOERRCODE  5   ; #BR  BOUND Range Exceeded
ISR_NOERRCODE  6   ; #UD  Invalid Opcode
ISR_NOERRCODE  7   ; #NM  Device Not Available
ISR_ERRCODE    8   ; #DF  Double Fault       ← uses IST1 (dedicated stack)
ISR_NOERRCODE  9   ;      Coprocessor Segment Overrun (legacy, never fires)
ISR_ERRCODE   10   ; #TS  Invalid TSS
ISR_ERRCODE   11   ; #NP  Segment Not Present
ISR_ERRCODE   12   ; #SS  Stack-Segment Fault
ISR_ERRCODE   13   ; #GP  General Protection Fault
ISR_ERRCODE   14   ; #PF  Page Fault
ISR_NOERRCODE 15   ;      Reserved
ISR_NOERRCODE 16   ; #MF  x87 FPU Floating-Point Error
ISR_ERRCODE   17   ; #AC  Alignment Check
ISR_NOERRCODE 18   ; #MC  Machine Check      ← uses IST3
ISR_NOERRCODE 19   ; #XM  SIMD Floating-Point Exception
ISR_NOERRCODE 20   ; #VE  Virtualization Exception
ISR_ERRCODE   21   ; #CP  Control Protection Exception
ISR_NOERRCODE 22   ; Reserved
ISR_NOERRCODE 23   ; Reserved
ISR_NOERRCODE 24   ; Reserved
ISR_NOERRCODE 25   ; Reserved
ISR_NOERRCODE 26   ; Reserved
ISR_NOERRCODE 27   ; Reserved
ISR_NOERRCODE 28   ; #HV  Hypervisor Injection Exception
ISR_ERRCODE   29   ; #VC  VMM Communication Exception
ISR_ERRCODE   30   ; #SX  Security Exception
ISR_NOERRCODE 31   ; Reserved

; ── Hardware IRQ stubs (vectors 32–47, from PIC / I/O APIC) ─────────────────
%assign irq_vec 32
%rep 16
ISR_NOERRCODE irq_vec
%assign irq_vec irq_vec+1
%endrep

; ── LAPIC timer (vector 48) ──────────────────────────────────────────────────
ISR_NOERRCODE 48

; ── LAPIC spurious (vector 255) ──────────────────────────────────────────────
ISR_NOERRCODE 255

; ── TLB shootdown IPI (vector 251) ───────────────────────────────────────────
ISR_NOERRCODE 251

; ── Remaining vectors 49–250 and 252–254 (generic stubs) ─────────────────────
%assign vec 49
%rep 202
ISR_NOERRCODE vec
%assign vec vec+1
%endrep

; =============================================================================
; isr_common_stub — saves full pt_regs frame, calls isr_dispatch(pt_regs *)
; =============================================================================
isr_common_stub:
    ; If we came from user mode (CS RPL = 3), we must swapgs to get kernel GS.
    ; Stack has: [rsp+0]=int_no, [rsp+8]=err_code, [rsp+16]=rip, [rsp+24]=cs
    test qword [rsp + 24], 3
    jz .skip_swapgs_entry
    swapgs
.skip_swapgs_entry:
    ; Save all general-purpose registers.
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Save the current data segment selector so we can restore it on return.
    mov  rax, ds
    push rax

    ; Switch all data segments to kernel data (selector 0x10).
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    ; Note: FS/GS are per-CPU and must NOT be overwritten here.

    ; RSP now points to the bottom of the pt_regs_t frame.
    ; Pass it as the first argument (RDI) to isr_dispatch().
    mov  rdi, rsp
    cld                     ; ensure string instructions go forward
    mov  rbx, rsp           ; Save original RSP (rbx is callee-saved)
    and  rsp, -16           ; Align stack to 16 bytes for System V ABI
    call isr_dispatch
    mov  rsp, rbx           ; Restore original RSP

    ; ── Restore ──────────────────────────────────────────────────────────────
global isr_restore_stub
isr_restore_stub:
    ; Restore saved DS (pop into RAX, then mov to segment register).
    pop  rax
    mov  ds, ax
    mov  es, ax

    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rbp
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax

    ; Check if we are returning to user mode (CS RPL = 3)
    ; Stack currently has: [rsp+0]=int_no, [rsp+8]=err_code, [rsp+16]=rip, [rsp+24]=cs
    test qword [rsp + 24], 3
    jz .skip_swapgs_exit
    swapgs
.skip_swapgs_exit:

    ; Skip int_no and err_code (16 bytes) that we pushed earlier.
    add  rsp, 16

    ; Return from interrupt: restores RIP, CS, RFLAGS, (RSP, SS on ring switch).
    iretq
