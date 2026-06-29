; kernel/arch/interrupts64.asm - 64-bit interrupt service routines
[bits 64]

extern exception_handler
extern irq_handler
extern g_active_context

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

; Generate processor error interrupts
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

; syscalls
ISR_NOERRCODE 128 ; Invoke interrupt 0x80

; Generate hardware interrupts (IRQ 0-15)
IRQ 0, 32  ; IRQ0  - PIT
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

    mov rdi, rsp
    cld
    call exception_handler

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

    mov rax, g_active_context
    mov [rax], rsp
    mov rdi, rsp
    cld
    call irq_handler

    mov rax, g_active_context
    mov rsp, [rax]

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
