; kernel/arch/interrupts.asm - 64-bit interrupt service routines
; Per-core context switching: g_active_context is GONE.
; exception_handler and irq_handler return the stack pointer to restore in RAX.
; The stubs load RSP from RAX directly - no shared global memory needed.
[bits 64]

extern exception_handler
extern irq_handler
extern syscall_handler

; syscall_entry uses per-core TSS.RSP0 from the g_cpu_rsp0 array (indexed by APIC ID).
; We read LAPIC ID from MMIO at 0xFEE00020 [bits 27:24] to pick the right core's RSP0.
extern g_cpu_rsp0

%macro ISR_NOERRCODE 1
global isr_%1
isr_%1:
    push 0
    push %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
global isr_%1
isr_%1:
    push %1
    jmp isr_common_stub
%endmacro

%macro IRQ 2
global irq_%1
irq_%1:
    push %1
    push %2
    jmp irq_common_stub
%endmacro

; Generate processor exception stubs
ISR_NOERRCODE 0  ; Divide Error
ISR_NOERRCODE 1  ; Debug Exception
ISR_NOERRCODE 2  ; NMI Interrupt
ISR_NOERRCODE 3  ; Breakpoint
ISR_NOERRCODE 4  ; Overflow
ISR_NOERRCODE 5  ; BOUND Range Exceeded
ISR_NOERRCODE 6  ; Invalid Opcode
ISR_NOERRCODE 7  ; Device Not Available
ISR_ERRCODE   8  ; Double Fault
ISR_NOERRCODE 9  ; Coprocessor Segment Overrun
ISR_ERRCODE   10 ; Invalid TSS
ISR_ERRCODE   11 ; Segment Not Present
ISR_ERRCODE   12 ; Stack-Segment Fault
ISR_ERRCODE   13 ; General Protection Fault
ISR_ERRCODE   14 ; Page Fault
ISR_NOERRCODE 15 ; Intel Reserved
ISR_NOERRCODE 16 ; x87 FPU Floating-Point Error
ISR_ERRCODE   17 ; Alignment Check
ISR_NOERRCODE 18 ; Machine Check
ISR_NOERRCODE 19 ; SIMD Floating-Point Exception
ISR_NOERRCODE 20 ; Virtualization Exception
ISR_ERRCODE   21 ; Control Protection Exception
ISR_NOERRCODE 22 ; Intel Reserved
ISR_NOERRCODE 23 ; Intel Reserved
ISR_NOERRCODE 24 ; Intel Reserved
ISR_NOERRCODE 25 ; Intel Reserved
ISR_NOERRCODE 26 ; Intel Reserved
ISR_NOERRCODE 27 ; Intel Reserved
ISR_NOERRCODE 28 ; Intel Reserved
ISR_ERRCODE   29 ; VMM Communication Exception
ISR_ERRCODE   30 ; Security Exception
ISR_NOERRCODE 31 ; Intel Reserved

; syscall soft-int fallback (int 0x80)
global isr_128
isr_128:
    push 0
    push 128
    jmp isr_common_stub

; TLB shootdown IPI - vector 252
ISR_NOERRCODE 252

IRQ 0, 32  ; IRQ0  - PIT Timer
IRQ 1, 33  ; IRQ1  - Keyboard PS/2
IRQ 2, 34  ; IRQ2  - Cascade
IRQ 3, 35  ; IRQ3  - COM2
IRQ 4, 36  ; IRQ4  - COM1
IRQ 5, 37  ; IRQ5  - LPT2
IRQ 6, 38  ; IRQ6  - Floppy Disk
IRQ 7, 39  ; IRQ7  - LPT1
IRQ 8, 40  ; IRQ8  - CMOS RTC
IRQ 9, 41  ; IRQ9  - free
IRQ 10, 42 ; IRQ10 - free
IRQ 11, 43 ; IRQ11 - free
IRQ 12, 44 ; IRQ12 - Mouse PS/2
IRQ 13, 45 ; IRQ13 - FPU
IRQ 14, 46 ; IRQ14 - Primary ATA
IRQ 15, 47 ; IRQ15 - Secondary ATA

; ──────────────────────────────────────────────────────────────────────────────
; isr_common_stub  -  exception/int 0x80 path
; exception_handler(registers_t *rdi) returns uintptr_t in RAX = new RSP to load
; ──────────────────────────────────────────────────────────────────────────────
isr_common_stub:
    push rax
    push rcx
    push rdx
    push rbx
    push rsp
    push rbp
    push rsi
    push rdi
    mov rax, ds
    push rax

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rdi, rsp                    ; arg0 = pointer to current register frame
    cld
    call exception_handler          ; returns new RSP (or same RSP) in RAX

    test rax, rax
    jz .no_restore
    mov rsp, rax                    ; load per-core returned stack pointer
.no_restore:

    pop rax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    pop rdi
    pop rsi
    pop rbp
    pop rax ; euseless
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16
    iretq

; ──────────────────────────────────────────────────────────────────────────────
; irq_common_stub  -  hardware IRQ path (IRQ0..IRQ15)
; irq_handler(registers_t *rdi) returns uintptr_t in RAX = new RSP to load
; ──────────────────────────────────────────────────────────────────────────────
irq_common_stub:
    push rax
    push rcx
    push rdx
    push rbx
    push rsp
    push rbp
    push rsi
    push rdi
    mov rax, ds
    push rax

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rdi, rsp                    ; arg0 = pointer to current register frame
    cld
    call irq_handler                ; returns new RSP (or same RSP) in RAX

    test rax, rax
    jz .no_restore
    mov rsp, rax                    ; load per-core returned stack pointer
.no_restore:

    pop rax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    pop rdi
    pop rsi
    pop rbp
    pop rax ; euseless
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16
    iretq

; ──────────────────────────────────────────────────────────────────────────────
; syscall_entry  -  fast SYSCALL (64-bit only)
; On entry: RAX=syscall#  RCX=user RIP  R11=user RFLAGS  RSP=user RSP
; Switch to per-core kernel stack via g_cpu_rsp0[apic_id].
; syscall_handler(registers_t *rdi) returns uintptr_t (new RSP) in RAX.
; ──────────────────────────────────────────────────────────────────────────────
global syscall_entry
syscall_entry:
    mov r12, rsp                    ; save userspace RSP

    ; Read APIC ID from MMIO [bits 27:24] to get per-core kernel stack
    mov r13, 0xFEE00020             ; LAPIC ID register address
    mov r13d, [r13]                 ; read 32-bit ID register
    shr r13d, 24                    ; APIC ID is bits [27:24]
    and r13d, 0xFF                  ; mask to 8 bits
    lea r14, [rel g_cpu_rsp0]       ; base of RSP0 table
    mov rsp, [r14 + r13*8]          ; RSP = g_cpu_rsp0[apic_id]

    ; Build registers_t frame (must match isr_common_stub layout exactly)
    push 0x23                       ; registers_t.ss  (User Data Selector)
    push r12                        ; registers_t.esp (user RSP)
    push r11                        ; registers_t.eflags (RFLAGS from R11)
    push 0x2B                       ; registers_t.cs  (User Code Selector)
    push rcx                        ; registers_t.eip (user RIP from RCX)

    push 0                          ; registers_t.err_code
    push 128                        ; registers_t.int_no

    push rax                        ; registers_t.edi (syscall number in rdi slot)
    push r10                        ; registers_t.esi (arg2 — R10 per SYSCALL ABI)
    push rdx                        ; registers_t.ebp (arg3)
    push rbx                        ; registers_t.euseless (arg1)
    push r12                        ; registers_t.ebx (user RSP)
    push rbp                        ; registers_t.edx (arg6)
    push rsi                        ; registers_t.ecx (arg4)
    push rdi                        ; registers_t.eax (arg5)

    mov ax, ds
    push rax                        ; registers_t.ds

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rdi, rsp                    ; arg0 = pointer to register frame
    cld
    call syscall_handler            ; returns new RSP in RAX

    test rax, rax
    jz .no_restore
    mov rsp, rax
.no_restore:

    pop rax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    pop rdi
    pop rsi
    pop rbp
    pop rax                         ; euseless
    pop rbx
    pop rdx
    pop rcx
    pop rax                         ; RAX = syscall return value
    add rsp, 16                     ; skip int_no, err_code

    pop rcx                         ; user RIP for SYSRETQ
    add rsp, 8                      ; skip CS
    pop r11                         ; user RFLAGS for SYSRETQ
    pop rsp                         ; user RSP

    db 0x48, 0x0F, 0x07             ; sysretq
