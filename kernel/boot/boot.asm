MBOOT_ALIGN_FLAG equ 1 << 0
MBOOT_MEMINFO_FLAG equ 1 << 1
MBOOT_FLAGS equ MBOOT_ALIGN_FLAG | MBOOT_MEMINFO_FLAG
MBOOT_MAGIC equ 0x1BADB002
MBOOT_CHECKSUM equ - (MBOOT_MAGIC + MBOOT_FLAGS)
global mboot
extern code
extern bss
extern end
mboot:
    dd MBOOT_MAGIC
    dd MBOOT_FLAGS
    dd MBOOT_CHECKSUM
    dd mboot
    dd code
    dd bss
    dd boot

global boot
extern x86_arch_init
boot:
    mov esp, stack_top
    
    call x86_arch_init
    cli 
    hlt
    jmp $

section .bss
align 16
stack_bottom:
resb 32768
stack_top: