; =============================================================================
; AzamiOS — Boot Entry Point (Limine Protocol, x86_64)
; File: arch/x86_64/boot/entry.asm
;
; When Limine hands control to us:
;   • The CPU is already in 64-bit long mode.
;   • A4-level (PML4) page table is active with at minimum the kernel ELF
;     mapped at KERNEL_VIRT_BASE and an HHDM at 0xFFFF800000000000.
;   • Interrupts are disabled (IF=0).
;   • RSP points to a temporary Limine-provided stack (often < 64 KB).
;   • RBX / R15 contain the Limine info pointer (we ignore it; we use the
;     request objects that Limine filled in before calling us).
;
; Our job here:
;   1. Switch to our own per-BSP kernel stack (16 KB, aligned to 16 bytes).
;   2. Zero the BSS segment (.bss_start .. _bss_end symbols from linker).
;   3. Call kernel_main() in C.
;   4. If kernel_main() somehow returns, halt with interrupts off forever.
;
; ABI note: kernel_main() uses the System V AMD64 calling convention.
;           No arguments are passed (void kernel_main(void)).
; =============================================================================

bits 64
section .text

extern kernel_main          ; defined in kernel/main.c
extern _bss_start           ; from linker script
extern _bss_end             ; from linker script

global az_boot_entry

; =============================================================================
; az_boot_entry — Limine calls here (no arguments, no alignment requirement)
; =============================================================================
az_boot_entry:
    ; ── 1. Disable interrupts (should already be off, but be explicit) ────────
    cli

    ; ── 2. Load our own kernel stack ─────────────────────────────────────────
    ; The stack grows downward; we point RSP at the TOP of the boot_stack array.
    lea rsp, [rel boot_stack_top]

    ; Align RSP to 16 bytes to satisfy System V AMD64 ABI before any CALL.
    and rsp, -16

    ; ── 3. Zero the BSS segment ──────────────────────────────────────────────
    ; We must clear .bss ourselves because Limine does not guarantee it.
    ; Use rep stosb: RDI = destination, RCX = count, AL = fill byte.
    mov rdi, _bss_start
    mov rcx, _bss_end
    sub rcx, rdi            ; byte count = _bss_end - _bss_start
    xor eax, eax            ; fill with 0x00
    cld
    rep stosb

    ; ── 4. Enable SSE / SSE2 (required by the x86_64 System V ABI) ──────────
    ; Compiler-generated code may use XMM registers for copies and arithmetic.
    ; Without these CR0/CR4 bits, any SSE instruction raises #UD.
    ;
    ; CR0: clear EM (bit 2 = software FPU emulation) and TS (bit 3 = task switch).
    ;      set   MP (bit 1 = monitor coprocessor — TS-#NM cooperation).
    mov rax, cr0
    and rax, ~(1 << 2)      ; clear EM
    and rax, ~(1 << 3)      ; clear TS
    or  rax,  (1 << 1)      ; set   MP
    mov cr0, rax

    ; CR4: set OSFXSR (bit 9) and OSXMMEXCPT (bit 10).
    mov rax, cr4
    or  rax, (1 << 9)       ; OSFXSR   — OS supports FXSAVE/FXRSTOR
    or  rax, (1 << 10)      ; OSXMMEXCPT — OS handles unmasked SSE exceptions
    mov cr4, rax

    ; ── 5. Jump into C ───────────────────────────────────────────────────────
    ; No arguments — kernel_main reads boot info through the Limine request
    ; objects that the bootloader filled before calling us.
    call kernel_main

    ; ── 6. Hang if kernel_main ever returns ──────────────────────────────────
.hang:
    cli
    hlt
    jmp .hang

; =============================================================================
; Boot stack — 16 KB, 16-byte aligned, in BSS so it doesn't bloat the ELF.
; =============================================================================
section .bss
align 16
boot_stack:
    resb 16384              ; 16 KB
boot_stack_top:
