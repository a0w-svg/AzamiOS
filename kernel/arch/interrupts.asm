%macro isr_err_stub 1
global isr_%1
isr_%1:
    push %1
    jmp  isr_common_stub
%endmacro

%macro isr_no_err_stub 1
global isr_%1
isr_%1:
    push  0
    push %1
    jmp isr_common_stub
%endmacro

isr_no_err_stub 0
isr_no_err_stub 1
isr_no_err_stub 2
isr_no_err_stub 3
isr_no_err_stub 4
isr_no_err_stub 5
isr_no_err_stub 6
isr_no_err_stub 7
isr_err_stub    8
isr_no_err_stub 9
isr_err_stub    10
isr_err_stub    11
isr_err_stub    12
isr_err_stub    13
isr_err_stub    14
isr_no_err_stub 15
isr_no_err_stub 16
isr_no_err_stub 17
isr_no_err_stub 18
isr_no_err_stub 19
isr_no_err_stub 20
isr_no_err_stub 21
isr_no_err_stub 22
isr_no_err_stub 23
isr_no_err_stub 24
isr_no_err_stub 25
isr_no_err_stub 26
isr_no_err_stub 27
isr_no_err_stub 28
isr_no_err_stub 29
isr_err_stub    30
isr_no_err_stub 31

extern exception_handler
isr_common_stub:
    pushad  ; pushes edi,esi,ebp, esp, ebx, ecx, eax;
    call exception_handler
    popad
    add esp, 8
    iret
%macro irq_stub 2
global irq_%1
irq_%1:
    cli
    push byte 0
    push byte %2
    jmp irq_common_stub
%endmacro
irq_stub 0, 32
irq_stub 1, 33
irq_stub 2, 34
irq_stub 3, 35
irq_stub 4, 36
irq_stub 5, 37
irq_stub 6, 38
irq_stub 7, 39
irq_stub 8, 40
irq_stub 9, 41
irq_stub 10, 42
irq_stub 11, 43
irq_stub 12, 44
irq_stub 13, 45
irq_stub 14, 46
irq_stub 15, 47


extern irq_handler
irq_common_stub:
    pushad
    push esp
    call irq_handler
    pop esp
    popad
    add esp, 8
    iret