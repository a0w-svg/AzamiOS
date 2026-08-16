; =============================================================================
; AzamiOS — GDT Flush & TSS Load Assembly Stubs
; File: arch/x86_64/cpu/gdt_flush.asm
;
; These are the two low-level stubs that C code cannot do inline:
;
;   1. gdt_flush(gdt_ptr_t *ptr)
;      Loads the GDTR from *ptr, then performs a 64-bit far return (lretq)
;      to reload the Code Segment register (CS) with selector 0x08.
;      After the far return, all data segment registers (DS, ES, FS, GS, SS)
;      are reloaded with selector 0x10 (kernel data).
;
;   2. tss_flush(u16 selector)
;      Executes LTR to load the Task Register from the given selector.
;      The selector (0x28) points to the 16-byte TSS system descriptor in
;      the GDT. The CPU reads both slot 5 (0x28) and slot 6 (0x30) to
;      reconstruct the full 64-bit TSS base address.
;
; Why a far return instead of a far jump to reload CS?
;   In 64-bit long mode the CPU does not support the far JMP form with a
;   64-bit target. The standard workaround is:
;     push <new_cs>   ; push 64-bit CS selector
;     lea  rax, [rip+offset]   ; next instruction address
;     push rax        ; push 64-bit RIP
;     retfq           ; "far return" pops RIP then CS
;   This atomically reloads CS and flushes the prefetch queue.
;   We encode retfq as REX.W (0x48) + LRET (0xCB) in raw bytes because some
;   older NASM versions do not recognise "o64 retf" / "retfq" as a mnemonic.
; =============================================================================

bits 64
section .text

; =============================================================================
; void gdt_flush(gdt_ptr_t *ptr)   — RDI = pointer to 10-byte GDTR struct
; =============================================================================
global gdt_flush
gdt_flush:
    ; Step 1: Load GDTR with the new GDT address and limit.
    lgdt [rdi]

    ; Step 2: Reload CS via a 64-bit far return.
    ;   Push the new CS selector (0x08 = kernel code) then the return address,
    ;   then execute a REX.W LRET (0x48 0xCB = retfq).
    push 0x08                        ; new CS selector
    lea  rax, [rel .reload_cs]       ; address of next label
    push rax
    db   0x48, 0xCB                  ; REX.W + LRET (retfq) — far return 64-bit

.reload_cs:
    ; Step 3: Reload data segment registers with kernel data selector 0x10.
    ;   In 64-bit long mode DS/ES/FS/GS/SS are mostly ignored for address
    ;   calculation, but the CPU still checks the descriptor cache on transitions.
    ;   Loading them here ensures they hold a valid, present, writable descriptor.
    mov  ax, 0x10                    ; SEL_KERNEL_DATA
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    ; Do not touch GS base (used for per-CPU info)
    xor  ax, ax
    mov  fs, ax

    ret

; =============================================================================
; void tss_flush(u16 selector)   — DI = TSS selector (0x28)
; =============================================================================
global tss_flush
tss_flush:
    ; LTR loads the Task Register from the GDT slot pointed to by the selector.
    ; The CPU reads slot 5 (0x28) and slot 6 (0x30) together to get the full
    ; 64-bit TSS base address. After LTR, the TSS is "busy" (bit 1 of the
    ; access byte is set by hardware) and cannot be re-loaded until cleared.
    ltr  di
    ret
