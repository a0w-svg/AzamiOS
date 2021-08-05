global gdt_flush ; Allows  the C code to call gdt_flush().
extern gtd_pointer
gdt_flush:
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