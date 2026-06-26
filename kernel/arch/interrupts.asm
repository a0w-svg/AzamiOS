; extern C functions
extern  exception_handler
extern irq_handler
extern g_active_context

; -----------------------------------------
; NASM PREPROCESSORS MACROS
; -----------------------------------------

; first MACRO without hardware error code
%macro ISR_NOERRCODE 1
global isr_%1
isr_%1:
    push 0
    push %1
    jmp isr_common_stub
%endmacro

; macro with error code
%macro ISR_ERRCODE 1
global isr_%1
isr_%1:
    push %1
    jmp isr_common_stub
%endmacro

; hardware interrupts (IRQ)
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
ISR_NOERRCODE 7  ; Device Not Available (No Math Coprocessor)
ISR_ERRCODE   8  ; Double Fault (has error code!)
ISR_NOERRCODE 9  ; Coprocessor Segment Overrun
ISR_ERRCODE   10 ; Invalid TSS (has error code!)
ISR_ERRCODE   11 ; Segment Not Present (has error code!)
ISR_ERRCODE   12 ; Stack-Segment Fault (has error code!)
ISR_ERRCODE   13 ; General Protection Fault (has error code!)
ISR_ERRCODE   14 ; Page Fault (has error code!)
ISR_NOERRCODE 15 ; Intel Reserved
ISR_NOERRCODE 16 ; x87 FPU Floating-Point Error
ISR_ERRCODE   17 ; Alignment Check (has error code!)
ISR_NOERRCODE 18 ; Machine Check
ISR_NOERRCODE 19 ; SIMD Floating-Point Exception
ISR_NOERRCODE 20 ; Virtualization Exception
ISR_ERRCODE   21 ; Control Protection Exception (has error code)
ISR_NOERRCODE 22 ; Intel Reserved
ISR_NOERRCODE 23 ; Intel Reserved
ISR_NOERRCODE 24 ; Intel Reserved
ISR_NOERRCODE 25 ; Intel Reserved
ISR_NOERRCODE 26 ; Intel Reserved
ISR_NOERRCODE 27 ; Intel Reserved
ISR_NOERRCODE 28 ; Intel Reserved
ISR_ERRCODE   29 ; VMM Communication Exception (has error code!)
ISR_ERRCODE   30 ; Security Exception (has error code)
ISR_NOERRCODE 31 ; Intel Reserved

; syscalls
ISR_NOERRCODE 128 ; Invoke interrupt 0x80

; Generate hardware interrupts (IRQ 0-15)

IRQ 0, 32  ; IRQ0  - PIT(system clock)
IRQ 1, 33  ; IRQ1  - Keyboard PS/2
IRQ 2, 34  ; IRQ2  - Cascade (used internally by PIC)
IRQ 3, 35  ; IRQ3  - COM2 (Serial Port)
IRQ 4, 36  ; IRQ4  - COM1 (Serial Port)
IRQ 5, 37  ; IRQ5  - LPT2 (Parallel Port)
IRQ 6, 38  ; IRQ6  - Floppy Disk (Floppy Drive)
IRQ 7, 39  ; IRQ7  - LPT1 / Spurious Interrupt
IRQ 8, 40  ; IRQ8  - CMOS RTC
IRQ 9, 41  ; IRQ9  - free / various devices
IRQ 10, 42 ; IRQ10 - free / various devices
IRQ 11, 43 ; IRQ11 - free / various devices
IRQ 12, 44 ; IRQ12 - Mouse PS/2
IRQ 13, 45 ; IRQ13 - FPU / Coprocessor
IRQ 14, 46 ; IRQ14 - Primary ATA Hard Disk
IRQ 15, 47 ; IRQ15 - Secondary ATA Hard Disk

; common innterupts handlers
isr_common_stub:
    pusha ; save cpu state
    mov ax, ds ; lower 16 bits of eax = ds
    push eax ; save the data segment descriptor
    mov ax, 0x10 ; kernel data segment descriptor
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push esp ; registers_t *r
    cld ; C code following the System V ABI requires DF flag to be clean on fuction entry
    call exception_handler ; call C ISR handler
    pop eax ; restore saved CPU state
    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popa
    add esp, 8 ; cleans up the pushed error code and pushed ISR number;
    iret ; pops the last 5 registers at once: CS, EIP, EFLAGS and ESP

irq_common_stub:
    pusha ; save CPU state
    mov ax, ds ; lower 16 bits of EAX = ds
    push eax ; save the data segment
    mov ax, 0x10 ; kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov [g_active_context], esp ; save current stack context
    push esp ; registers_t *r
    cld ; cleans DF flag
    call irq_handler
    add esp, 4 ; pop pushed argument
    mov eax, [g_active_context]
    test eax, eax
    jz .no_switch
    mov esp, eax ; switch stack pointer!
.no_switch:
    pop ebx ; restore data segment
    mov ds, bx
    mov es, bx
    mov fs, bx
    mov gs, bx
    popa
    add esp, 8
    iret
