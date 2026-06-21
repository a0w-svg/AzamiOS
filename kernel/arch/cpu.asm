global gdt_flush ; Allows  the C code to call gdt_flush().
gdt_flush:
    mov eax, [esp+4]
    lgdt [eax]         ; load the new GDT pointer.

    mov ax, 0x10       ; 0x10 is the offset in the GDT to our data segment.
    mov ds, ax         ; loads all data segment selectors.
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:flush ; returns to the code segment.

flush:
    ret

global enter_usermode

enter_usermode:
    cli ; disable interrupts
    ; user data segment vector is 4 * 8 = 0x20. Add 3 for Ring 3 = 0x23
    mov ecx, [esp+4]
    mov edx, [esp+8]
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; stack configuration for iret
    mov eax, esp
    push 0x23 ; SS - Stack Segment Ring 3
    push eax  ; ESP - Using the same stack

    pushf
    pop eax
    or eax, 0x200 ; set bit 9 (IF) to enable interrupts in jmp_usermode
    push eax ; return modified flags onto stack

    push 0x1B
    push ecx
    iret

