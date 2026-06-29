; smp_boot64.asm - Real-mode AP SIPI Trampoline for 64-bit Long Mode relocated to 0x8000
[bits 16]
global smp_boot_start
global smp_boot_end

smp_boot_start:
    cli
    cld
    xor ax, ax
    mov ds, ax

    lgdt [0x8000 + (tramp_gdt_ptr - smp_boot_start)]

    mov eax, cr0
    or al, 1
    mov cr0, eax

    db 0x66, 0xEA
    dd 0x8000 + (ap_protected - smp_boot_start)
    dw 0x0008

[bits 32]
ap_protected:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    mov eax, [0x8108]
    mov cr3, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)
    wrmsr

    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax

    jmp 0x18:(0x8000 + (ap_long - smp_boot_start))

[bits 64]
ap_long:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    lgdt [0x8110]

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, [0x8100]

    mov rax, [0x8120]
    call rax

.halt:
    cli
    hlt
    jmp .halt

align 16
tramp_gdt:
    dq 0x0000000000000000 ; 0x00: Null
    dq 0x00CF9A000000FFFF ; 0x08: 32-bit Code (G=1, D=1, P=1, DPL=0, Type=0xA)
    dq 0x00CF92000000FFFF ; 0x10: 32-bit Data (G=1, D=1, P=1, DPL=0, Type=0x2)
    dq 0x00AF9A000000FFFF ; 0x18: 64-bit Code (G=1, L=1, D=0, P=1, DPL=0, Type=0xA)
    dq 0x00AF92000000FFFF ; 0x20: 64-bit Data (G=1, L=0, D=0, P=1, DPL=0, Type=0x2)
tramp_gdt_end:

tramp_gdt_ptr:
    dw tramp_gdt_end - tramp_gdt - 1
    dd 0x8000 + (tramp_gdt - smp_boot_start)

align 16
smp_boot_end:
