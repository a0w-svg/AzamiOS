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

; enter_usermode(uint32_t user_entry, uint32_t user_stack_top)
;   user_entry     [esp+4]  -> ecx
;   user_stack_top [esp+8]  -> edx
enter_usermode:
    cli                        ; disable interrupts while building iret frame

    mov ecx, [esp+4]           ; ecx = user entry point
    mov edx, [esp+8]           ; edx = user stack top

    ; switch data segments to ring-3 selectors (GDT index 4 = 0x20, + RPL 3 = 0x23)
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Build the iret frame on the current (kernel) stack:
    ;   [esp+0]  EIP    – user entry point
    ;   [esp+4]  CS     – 0x1B  (GDT index 3, RPL 3)
    ;   [esp+8]  EFLAGS – IF=1
    ;   [esp+12] ESP    – user stack top     <-- was wrongly using kernel esp
    ;   [esp+16] SS     – 0x23  (GDT index 4, RPL 3)
    push 0x23                  ; SS  – user data segment, Ring 3
    push edx                   ; ESP – user_stack_top (the argument we were passed)

    pushf
    pop eax
    or  eax, 0x200             ; IF=1: re-enable interrupts once in ring 3
    push eax                   ; EFLAGS

    push 0x1B                  ; CS  – user code segment, Ring 3
    push ecx                   ; EIP – user_entry
    iret

