MBOOT_ALIGN_FLAG equ 1 << 0
MBOOT_MEMINFO_FLAG equ 1 << 1
MBOOT_FLAGS equ MBOOT_ALIGN_FLAG | MBOOT_MEMINFO_FLAG
MBOOT_MAGIC equ 0x1BADB002
MBOOT_CHECKSUM equ - (MBOOT_MAGIC + MBOOT_FLAGS)

section .multiboot
align 4
    dd MBOOT_MAGIC
    dd MBOOT_FLAGS
    dd MBOOT_CHECKSUM

section .bss
align 16
stack_bottom:
resb 16384
stack_top:

section .text
global boot

boot:
    mov esp, stack_top
    extern x86_arch_init
    call x86_arch_init
    cli 
    hlt
    jmp $