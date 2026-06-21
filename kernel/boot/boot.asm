MBOOT_ALIGN_FLAG equ 1 << 0
MBOOT_MEMINFO_FLAG equ 1 << 1
MBOOT_FLAGS equ MBOOT_ALIGN_FLAG | MBOOT_MEMINFO_FLAG
MBOOT_MAGIC equ 0x1BADB002
MBOOT_CHECKSUM equ -(MBOOT_MAGIC + MBOOT_FLAGS)

section .multiboot
align 4
mboot:
    dd MBOOT_MAGIC
    dd MBOOT_FLAGS
    dd MBOOT_CHECKSUM


section .text
global boot
extern x86_arch_init

boot:
   
    mov esp, stack_top

    
    push ebx
    push eax

    
    cli 
    call x86_arch_init

    
.halt_loop:
    cli
    hlt
    jmp .halt_loop

section .bss
align 16
stack_bottom:
    resb 32768      ; 32 KB 
stack_top: