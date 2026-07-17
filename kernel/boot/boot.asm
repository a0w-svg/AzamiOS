bits 32

section .note.Xen
align 4
    dd 4           ; name size
    dd 4           ; desc size
    dd 18          ; type = XEN_ELFNOTE_PHYS32_ENTRY
    db "Xen", 0
    dd boot_entry

section .data
align 4
bsp_entered: dd 0

section .text
global _start
global boot_entry
global boot64_entry
global pml4
global pdpt
global pd0
extern x86_arch_init

_start:
boot_entry:
boot64_entry:
    cli
    ; Save incoming bootloader registers immediately (EAX = magic, EBX = bootinfo address)
    mov ebp, eax
    mov edi, ebx

    ; Atomic fetch-and-add to ensure only one CPU core acts as BSP during early boot
    mov eax, 1
    lock xadd [bsp_entered], eax
    test eax, eax
    jnz .ap_halt

.is_bsp:
    ; SAFE EARLY STACK (32-bit): Explicitly allocated and 16-byte aligned
    mov esp, stack_top

    ; EARLY LOGGING: Initialize COM1 serial port (0x3F8) at earliest instruction
    mov dx, 0x3F9
    mov al, 0x00      ; Disable all interrupts
    out dx, al
    mov dx, 0x3FB
    mov al, 0x80      ; Enable DLAB (set baud rate divisor)
    out dx, al
    mov dx, 0x3F8
    mov al, 0x03      ; Divisor 3 (38400 baud)
    out dx, al
    mov dx, 0x3F9
    mov al, 0x00      ; Divisor 0 (hi byte)
    out dx, al
    mov dx, 0x3FB
    mov al, 0x03      ; 8 bits, no parity, one stop bit (8N1)
    out dx, al
    mov dx, 0x3FA
    mov al, 0xC7      ; Enable FIFO, clear, 14-byte threshold
    out dx, al
    mov dx, 0x3FC
    mov al, 0x0B      ; IRQs enabled, RTS/DSR set
    out dx, al

    ; Print early diagnostic status directly via COM1 UART
    mov esi, msg_32bit_ok
    call .early_puts_32

    ; Restore magic and bootinfo pointer for page table logic
    mov eax, ebp
    mov ebx, edi

    ; Default: whatever boot magic and address was passed
    mov edi, eax
    mov esi, ebx

    ; Check if PVH hvm_start_info struct is passed in EBX when magic is not yet set
    test ebx, ebx
    jz .pvh_done
    cmp dword [ebx], 0x336ec578
    jne .pvh_done
    mov edi, 0x336ec578
.pvh_done:

    ; Preserve boot arguments on early stack while modifying page tables and MSRs
    push edi
    push esi

    ; Build identity page tables for first 4GB of physical RAM using 2MB huge pages
    mov eax, pdpt
    or eax, 0x07          ; present + writable + user
    mov [pml4], eax

    mov eax, pd0
    or eax, 0x07
    mov [pdpt], eax
    add eax, 4096
    mov [pdpt + 8], eax
    add eax, 4096
    mov [pdpt + 16], eax
    add eax, 4096
    mov [pdpt + 24], eax

    mov ecx, 0
    mov edx, pd0
.map_loop:
    mov eax, ecx
    shl eax, 21           ; ecx * 2MB
    or eax, 0x83          ; present + writable + huge (bit 7), USER cleared for kernel isolation
    mov [edx + ecx * 8], eax
    mov dword [edx + ecx * 8 + 4], 0
    inc ecx
    cmp ecx, 2048
    jl .map_loop

    ; Enable PAE, OSFXSR, and OSXMMEXCPT (CR4 bits 5, 9, 10)
    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)
    mov cr4, eax

    ; Load CR3 with PML4 address
    mov eax, pml4
    mov cr3, eax

    ; Enable Long Mode (EFER MSR 0xC0000080 bit 8)
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)
    wrmsr

    ; Enable Paging and Protection (CR0 bit 31 and bit 0)
    mov eax, cr0
    or eax, (1 << 31) | (1 << 0)
    mov cr0, eax

    ; Load 64-bit GDT
    lgdt [gdt64_ptr]

    ; Restore boot parameters right before 64-bit far jump
    pop esi
    pop edi

    jmp 0x08:long_entry

.early_puts_32:
    push eax
    push edx
.puts_32_loop:
    mov al, [esi]
    test al, al
    jz .puts_32_done
    mov dx, 0x3FD
.puts_32_wait:
    in al, dx
    test al, 0x20
    jz .puts_32_wait
    mov dx, 0x3F8
    mov al, [esi]
    out dx, al
    inc esi
    jmp .puts_32_loop
.puts_32_done:
    pop edx
    pop eax
    ret

.ap_halt:
    cli
    hlt
    jmp .ap_halt

bits 64
long_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; SAFE EARLY STACK (64-bit): Explicit 64 KB kernel stack
    mov rsp, stack_top
    ; Enforce System V AMD64 ABI strict 16-byte alignment before any CALL instruction
    and rsp, ~0xF

    ; Save EDI (magic) and ESI (bootinfo pointer) into R12 and R13 across helper calls
    mov r12, rdi
    mov r13, rsi

    ; Print early 64-bit long mode status to COM1 serial UART
    mov rsi, msg_64bit_ok
    call .early_puts_64

    ; Restore System V AMD64 arguments (RDI = magic, RSI = bootinfo address)
    mov rdi, r12
    mov rsi, r13

    ; Call x86_arch_init(rdi, rsi)
    ; Note: right before 'call', rsp % 16 == 0. 'call' pushes 8-byte return address,
    ; so on entry to x86_arch_init, rsp % 16 == 8, matching System V AMD64 ABI specification exactly!
    call x86_arch_init

.hang:
    cli
    hlt
    jmp .hang

.early_puts_64:
    push rax
    push rdx
.puts_64_loop:
    mov al, [rsi]
    test al, al
    jz .puts_64_done
    mov dx, 0x3FD
.puts_64_wait:
    in al, dx
    test al, 0x20
    jz .puts_64_wait
    mov dx, 0x3F8
    mov al, [rsi]
    out dx, al
    inc rsi
    jmp .puts_64_loop
.puts_64_done:
    pop rdx
    pop rax
    ret

section .rodata
msg_32bit_ok: db "[BOOT-ASM] Entered 32-bit protected mode. Page table identity mapping init...", 13, 10, 0
msg_64bit_ok: db "[BOOT-ASM] Switched to 64-bit long mode. Stack aligned (System V AMD64 ABI). Calling C kernel...", 13, 10, 0

section .data
align 8
gdt64:
    dq 0
    dq 0x00209A0000000000  ; 64-bit code segment
    dq 0x0000920000000000  ; 64-bit data segment
gdt64_end:

gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dq gdt64

section .bss
align 4096
pml4:
    resb 4096
pdpt:
    resb 4096
pd0:
    resb 16384
align 16
stack_bottom:
    resb 65536             ; Explicit 64 KB kernel stack aligned to 16-byte boundary
stack_top:
