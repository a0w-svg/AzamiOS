; extern C functions
extern exception_handler
extern irq_handler
; set globals 
global isr_0
global isr_1
global isr_2
global isr_3
global isr_4
global isr_5
global isr_6
global isr_7
global isr_8
global isr_9
global isr_10
global isr_11
global isr_12
global isr_13
global isr_14
global isr_15
global isr_16
global isr_17
global isr_18
global isr_19
global isr_20
global isr_21
global isr_22
global isr_23
global isr_24
global isr_25
global isr_26
global isr_27
global isr_28
global isr_29
global isr_30
global isr_31

; irq globals variables to use on c
global irq_0
global irq_1
global irq_2
global irq_3
global irq_4
global irq_5
global irq_6
global irq_7
global irq_8
global irq_9
global irq_10
global irq_11
global irq_12
global irq_13
global irq_14
global irq_15
;isr variables initialize
isr_0: ; Divide Error
    push 0 ; pushes zero, because it isn't error, but fault;
    push 0 ; interrupt number;
    jmp isr_common_stub 
isr_1: ; Debug Exception,
    push 0 ; pushes zero, because it isn't error, but fault/trap;
    push 1 ; interrupt number;
    jmp isr_common_stub
isr_2: ; NMI Interrupt (Nonmaskable Interrupt)
    push 0  ; pushes zero, because it isn't error, but interrrupt;
    push 2  ; interupt number;
    jmp isr_common_stub
isr_3: ; Breakpoint
    push 0 ; pushes zero, because it isn't error, but trap;
    push 3 ; interrupt number;
    jmp isr_common_stub
isr_4: ; Overflow
    push 0 ; pushes zero, because it isn't error, but trap;
    push 4 ; interrupt number;
    jmp isr_common_stub
isr_5: ; BOUND Range Exceeded
    push 0 ; pushes zero, because it isn't error, but fault;
    push 5 ; interrupt number;
    jmp isr_common_stub
isr_6: ; Invalid Opcode(Undefined Opcode)
    push  0 ; pushes zero, because it isn't error, but fault;
    push  6 ; interrupt number;
    jmp isr_common_stub
isr_7: ; Device Not Available(No Math Coprocessor)
    push 0 ; pushes zero, because it isn't error, but fault;
    push 7 ; interrupt number;
    jmp isr_common_stub
isr_8: ; Double Fault
    push 0 ; pushes zero, because it is error, but it isn't fatal error;
    push 8 ; interrupt number;
    jmp isr_common_stub
isr_9: ; Coprocessor Segment Overrun (reserved)
    push 0 ; pushes zero, because it isn't error, but fault;
    push 9 ; interrupt number
    jmp isr_common_stub
isr_10: ; Invalid TSS
    push 1 ; pushes one, because it is an error;
    push 10 ; interrupt number
    jmp isr_common_stub
isr_11: ; Segment Not Present
    push 1 ; pushes one, because it is an error;
    push 11 ; interrupt number;
    jmp isr_common_stub
isr_12: ; Stack-Segment Fault
    push 1 ; pushes one, because it is an error;
    push 12 ; interrupt number;
    jmp isr_common_stub
isr_13: ; General Protection
    push 1 ; pushes one, because it is an error;
    push 13 ; interrupt number;
    jmp isr_common_stub
isr_14: ; Page Fault
    push 1 ; pushes one, because it is an error;
    push 14 ; interrupt number;
    jmp isr_common_stub
isr_15: ; intel reserved. Do not use.
    push 0 ; pushes zero, because it isn't error, but fault;
    push 15 ; interrupt number;
    jmp isr_common_stub
isr_16: ; x87 FPU Floating-Point Error(Math Fault)
    push 0 ; pushes zero, because it isn't error, but fault;
    push 16 ; interrupt number;
    jmp isr_common_stub
isr_17: ; Alignment Check
    push 0 ; pushes zero, because it isn't error, but fault;
    push 17 ; interrupt number;
    jmp isr_common_stub
isr_18: ; Machine Check
    push 0 ; pushes zero, because it isn't error, but abort;
    push 18 ; interrupt number;
    jmp isr_common_stub
isr_19: ; SIMD Floating-Point Exception
    push 0 ; pushes zero, because it isn't error, but fault;
    push 19 ; interrupt number;
    jmp isr_common_stub
isr_20: ; Virtualization Exception
    push 0 ; pushes zero, because it isn't error, but fault;
    push 20 ; interrupt number;
    jmp isr_common_stub
isr_21: ; Control Protection Exception
    push 0 ; pushes zero, because it isn't an error, but fault;
    push 21 ; interrupt number;
    jmp isr_common_stub
isr_22: ; Intel Reserved. Do not use;
    push 0 ; pushes zero, because it isn't an error, but fault;
    push 22 ; interrupt number;
    jmp isr_common_stub
isr_23: ; Intel Reserved. Do not use;
    push 0 ; pushes zero, because it isn't an error, but fault;
    push 23 ; interrupt number;
    jmp isr_common_stub
isr_24: ; Intel Reserved. Do not use;
    push 0 ; pushes zero, because it isn't an error, but fault;
    push 24 ; interrupt number;
    jmp isr_common_stub
isr_25: ; Intel Reserved. Do not use;
    push 0 ; pushes zero, because it isn't an error, but fault;
    push 25 ; interrupt number;
    jmp isr_common_stub
isr_26: ; Intel Reserved. Do not use;
    push 0 ; pushes zero, because it isn't an error, but fault;
    push 26 ; interrupt number;
    jmp isr_common_stub
isr_27: ; Intel Reserved. Do not use;
    push  0 ; pushes zero, because it isn't an error, but fault;
    push 27 ; interrupt number;
    jmp isr_common_stub
isr_28: ; Intel Reserved. Do not use;
    push 0 ; pushes zero, because it isn't an error, but fault;
    push 28 ; interrupt number;
    jmp isr_common_stub
isr_29: ; Intel Reserved. Do not use;
    push 0 ; pushes zero, because it isn't an error, but fault;
    push 29 ; interrupt number;
    jmp isr_common_stub
isr_30: ; Intel Reserved. Do not use;
    push 0 ; pushes zero, because it isn't an error, but fault;
    push 30 ; interrupt number;
    jmp isr_common_stub
isr_31: ; Intel Reserved. Do not use;
    push 0 ; pushes zero, because it isn't an error, but fault;
    push 31 ; interrupt number;
    jmp isr_common_stub

; IRQ handlers variables
irq_0: ; IRQ0 -  standard used by PIT
    push 0 ; IRQ number;
    push 32 ; interrupt number;
    jmp irq_common_stub

irq_1: ; IRQ1 - standard used by Keyboard.
    push 1 ; IRQ number;
    push 33 ; interrupt number;
    jmp irq_common_stub
irq_2: ; IRQ2 - Cascade (used internally by the two PICs. never raised)
    push 2 ; IRQ number;
    push 34 ; interrupt number;
    jmp irq_common_stub  
irq_3: ; IRQ3 - standard used by COM2 if enabled
    push 3 ; IRQ number;
    push 35 ; interrupt number;
    jmp irq_common_stub 
irq_4: ; IRQ4 - standard used by COM1 if enabled
    push 4 ; IRQ number;
    push 36 ; interrupt number;
    jmp irq_common_stub
irq_5: ; IRQ5 - standard used by LPT2 if enabled
    push 5 ; IRQ number;
    push 37 ; interrupt number;
    jmp irq_common_stub
irq_6: ; IRQ6 - standard used by Floppy Disk
    push 6 ; IRQ number;
    push 38 ; interrupt number;
    jmp irq_common_stub
irq_7: ; IRQ7 - standard used by LPT1/Unreliable "spurious" interrupt (usually)
    push 7 ; IRQ number;
    push 39 ; interrupt number;
    jmp irq_common_stub
irq_8: ; IRQ8 - standard used by CMOS real-time clock if enabled
    push 8 ; IRQ number;
    push 40 ; interrupt number;
    jmp irq_common_stub
irq_9: ; IRQ9 - Free for peripherals/legacy SCSI/NIC
    push 9 ; IRQ number;
    push 41 ; interrupt number;
    jmp irq_common_stub
irq_10: ; IRQ10 - Free for peripherals/SCSI/NIC
    push 10 ; IRQ number;
    push 42 ; interrupt number;
    jmp irq_common_stub
irq_11: ; IRQ11 - Free for peripherals/SCSI/NIC
    push 11 ; IRQ number;
    push 43 ; interrupt number;
    jmp irq_common_stub
irq_12: ; IRQ12 - PS2 Mouse
    push 12 ; IRQ number;
    push 44 ; interrupt number;
    jmp irq_common_stub
irq_13: ; IRQ13 - FPU/Coprocessor/Inter-processor
    push 13 ; IRQ number;
    push 45 ; interrupt number;
    jmp irq_common_stub
irq_14: ; IRQ14 - Primary ATA Hard Disk
    push 14 ; IRQ number;
    push 46 ; interrupt number;
    jmp irq_common_stub
irq_15: ; IRQ15 - Secondary ATA Hard Disk
    push 15 ; IRQ number;
    push 47 ; interrupt number;
    jmp irq_common_stub


isr_common_stub:
    pusha  ; save cpu state;
    mov ax, ds ; lower 16 bits of eax = ds;
    push eax ; save the data segment descriptor;
    mov ax, 0x10 ; kernel data segment descriptor;
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push esp ; registers_t *r
    cld ; C code following the system V ABi requires DF flag to be clean on function entry;
    call exception_handler ; call C ISR handler;
    pop eax ; restore saved CPU state;
    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popa
    add esp, 8 ; Cleans up the pushed error code and pushed ISR number;
    iret ; pops the last 5 registers at once: CS, EIP, EFLAGS and ESP;

irq_common_stub:
    pusha ; save CPU state;
    mov ax, ds ; lower 16 bits of EAX = ds
    push eax ; save the data segment;
    mov ax, 0x10 ; kernel data segment;
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push esp ; registers_t *r
    cld ; cleans DF flag;
    call irq_handler
    pop ebx ; restore saved cpu state;
    pop ebx
    mov ds, bx
    mov es, bx
    mov fs, bx
    mov gs, bx
    popa
    add esp, 8
    iret