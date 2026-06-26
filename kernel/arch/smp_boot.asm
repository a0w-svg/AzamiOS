; smp_boot.asm - Real-mode AP SIPI Trampoline relocated to 0x8000
[bits 16]
global smp_boot_start
global smp_boot_end

smp_boot_start:
    cli
    cld
    xor ax, ax
    mov ds, ax

    lgdt [0x8040]

    mov eax, cr0
    or al, 1
    mov cr0, eax

    ; Far jump to 32-bit protected mode at physical address 0x801B
    db 0x66, 0xEA
    dd 0x801B
    dw 0x0008

[bits 32]
ap_protected:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Load dedicated core stack from 0x8050
    mov esp, [0x8050]

    ; Invoke C kernel handler ap_entry stored at 0x8054
    mov eax, [0x8054]
    call eax

.halt:
    cli
    hlt
    jmp .halt

align 16
smp_boot_end:
