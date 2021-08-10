global gdt_flush ; Allows  the C code to call gdt_flush().
extern gdt_pointer
gdt_flush:
    lgdt [gdt_pointer]         ; load the new GDT pointer.

    mov ax, 0x10       ; 0x10 is the offset in the GDT to our data segment.
    mov ds, ax         ; loads all data segment selectors.
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:flush ; returns to the code segment.

flush:
    ret

global tss_flush ; allows our C code to call tss_flush()
tss_flush:
    mov ax, 0x2B ; Load the index of our TSS structure - The index is
                 ; 0x28, as it is the 5 selector and each is 8 bytes long
    ltr ax ; load 0x2B into task state register.
    ret ; return to the code segment.