; kernel/arch/cpu.asm - 64-bit architecture CPU utilities
[bits 64]

section .text
global gdt_flush
global enter_usermode
global enter_userspace
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

; void enter_userspace(uintptr_t user_entry, uintptr_t user_stack_top)
; void enter_usermode(uintptr_t user_entry, uintptr_t user_stack_top)
; RDI = user_entry
; RSI = user_stack_top
enter_userspace:
enter_usermode:
    cli
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push 0x23               ; User SS (Index 4 0x20 | RPL 3)
    push rsi                ; User RSP (Userspace Stack Pointer)
    pushf
    pop rax
    or rax, 0x202           ; IF=1 (Interrupts Enabled), bit 1 (reserved) = 1
    push rax                ; RFLAGS
    push 0x2B               ; User CS (Index 5 0x28 | RPL 3)
    push rdi                ; User RIP (Entry point of userspace program)

    iretq
