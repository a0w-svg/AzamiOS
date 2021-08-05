%macro isr_err_stub 1
isr_stub_%+%1:
    cli
    push byte 0
    push byte %1
    jmp  isr_common_stub
%endmacro

%macro isr_no_err_stub 1
isr_stub_%+%1:
    cli 
    push byte %1
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
isr_err_stub    17
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

global isr_stub_table
isr_stub_table:
%assign i 0
%rep 32
    dd isr_stub_%+i
%assign i i+1
%endrep

global idt_flush
idt_flush:
    mov eax, [esp+4]
    lidt[eax]
    ret

extern exception_handler
isr_common_stub:
    pusha  ; pushes edi,esi,ebp, esp, ebx, ecx, eax;
    mov ax, ds ; lower 16 bits of eax = ds;
    push eax ; save the data segment descriptior;
    mov ax, 0x10 ; load the kernel data segment descriptior;
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    call exception_handler
    pop eax ; reload the original data segment descriptior;
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa ; pops edi, esi, ebp, esp, ecx, eax;
    add esp, 8 ; cleans up the pushed error code and pushed ISR number;
    sti 
    iret ; pops CS, EIP, EFLAGS, SS and EIP;
