; kernel/arch/cpu64.asm - 64-bit architecture CPU utilities
[bits 64]

section .text
global gdt_flush
global enter_usermode
global switch_page_dir

; void gdt_flush(uintptr_t pointer)
; RDI = pointer to gdt_ptr_t
gdt_flush:
    lgdt [rdi]
    
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    push 0x08
    mov rax, .flush
    push rax
    retfq

.flush:
    ret

; void switch_page_dir(void *dir)
; RDI = physical address of page directory
switch_page_dir:
    mov cr3, rdi
    ret

; void enter_usermode(uintptr_t user_entry, uintptr_t user_stack_top)
; RDI = user_entry
; RSI = user_stack_top
enter_usermode:
    cli

    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push 0x23               ; SS
    push rsi                ; RSP
    pushf
    pop rax
    or rax, 0x200           ; IF=1
    push rax                ; RFLAGS
    push 0x1B               ; CS
    push rdi                ; RIP
    iretq
