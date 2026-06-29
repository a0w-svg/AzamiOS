bits 32

section .note.Xen
align 4
    dd 4           ; name size
    dd 4           ; desc size
    dd 18          ; type = XEN_ELFNOTE_PHYS32_ENTRY
    db "Xen", 0
    dd boot64_entry

section .data
align 4
bsp_entered: dd 0

section .text
global boot64_entry
global efi_main
global pml4
global pdpt
global pd0
extern x86_arch_init

efi_main:
boot64_entry:
    cli
    ; Save incoming boot registers immediately
    mov ebp, eax
    mov edi, ebx

    ; Use atomic fetch-and-add to ensure only one core acts as BSP
    mov eax, 1
    lock xadd [bsp_entered], eax
    test eax, eax
    jnz .ap_halt

.is_bsp:
    mov esp, stack_top

    ; Restore boot magic and pointer
    mov eax, ebp
    mov ebx, edi

    ; Default to whatever is in eax (e.g. Multiboot 1 magic)
    mov edi, eax
    mov esi, ebx

    ; Check if PVH hvm_start_info struct is passed in ebx
    test ebx, ebx
    jz .pvh_done
    cmp dword [ebx], 0x336ec578
    jne .pvh_done
    mov edi, 0x336ec578
.pvh_done:

    ; Build page tables for identity mapping first 4GB (2MB huge pages)
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
    or eax, 0x87          ; present + writable + user + huge (bit 7)
    mov [edx + ecx * 8], eax
    mov dword [edx + ecx * 8 + 4], 0
    inc ecx
    cmp ecx, 2048
    jl .map_loop

    ; Enable PAE, OSFXSR, and OSXMMEXCPT (CR4 bits 5, 9, 10)
    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)
    mov cr4, eax

    ; Load CR3 with PML4
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

    jmp 0x08:long_entry

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
    mov rsp, stack_top

    ; Call x86_arch_init(rdi, rsi)
    call x86_arch_init

.hang:
    cli
    hlt
    jmp .hang

section .data
align 8
gdt64:
    dq 0
    dq 0x00209A0000000000  ; 64-bit code
    dq 0x0000920000000000  ; 64-bit data
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
    resb 32768
stack_top:
