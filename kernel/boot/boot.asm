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
    mov esp, kstack
    push ebx
    push eax
    cli 
    call x86_arch_init
    jmp $
    hlt

section .bss
align 16
    resb 32768
kstack:
    resb 32768
    align 4096
    global heap
heap:
    resb 1 << 23