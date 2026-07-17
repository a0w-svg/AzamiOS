; =========================================================================================
; Azami OS — Hardware Abstraction Layer (HAL) GDT & TSS Assembly Flush Stub
; File: /kernel/hal/gdt/gdt_asm.asm
; Architecture: x86_64 (NASM syntax, System V AMD64 ABI)
;
; Description:
;   Executes low-level processor instructions (`LGDT`, `LRETQ`, `LTR`) required to reload
;   the Global Descriptor Table register, flush the CPU instruction prefetch queue and
;   segment caches, and bind the Task State Segment (`TSS`) to the hardware Task Register (`TR`).
; =========================================================================================

bits 64
section .text

global hal_gdt_flush

; =========================================================================================
; void hal_gdt_flush(hal_gdt_ptr_t *ptr)
;
; Arguments:
;   RDI = Pointer to the `hal_gdt_ptr_t` structure (contains 16-bit limit and 64-bit base address)
;
; Educational Step-by-Step Walkthrough:
;   1. Loading GDTR (`lgdt [rdi]`):
;      Tells the CPU where our 56-byte GDT array is located in physical/linear RAM.
;
;   2. Reloading Code Segment CS (`lretq`):
;      In 64-bit Long Mode (`x86_64`), you cannot execute `mov cs, ax` directly (the CPU will
;      raise `#GP` General Protection fault). Instead, you must perform a far jump or a far return.
;      The `lretq` instruction pops two 64-bit values off the stack:
;        - First pop -> target instruction pointer (`RIP`)
;        - Second pop -> target code segment selector (`CS`)
;      We push our target code segment (`0x08`), push the label `.reload_cs`, and execute `lretq`.
;      This reloads `%cs` with `0x08` and flushes any stale instruction lookahead buffers!
;
;   3. Reloading Data Segments (`DS, ES, FS, GS, SS`):
;      After `lretq`, we reload data segment registers with selector `0x10` (Kernel 64-Bit Data).
;
;   4. Loading Task Register TR (`ltr ax`):
;      Our TSS system descriptor is positioned at Slot 5 (`Selector 0x28`). Executing `ltr ax`
;      causes the CPU to read Slot 5 (`0x28`) and Slot 6 (`0x30`) to establish the 64-bit TSS
;      base address inside the hardware `TR` register.
; =========================================================================================
hal_gdt_flush:
    ; Step 1: Load the GDTR register from the structure pointed to by RDI
    lgdt [rdi]

    ; Step 2: Prepare stack frame for `lretq` (Far Return 64-Bit) to reload Code Segment (CS)
    push 0x08               ; Push target Code Segment Selector (`Selector 0x08`, Ring 0 64-bit code)
    lea rax, [rel .reload_cs] ; Load 64-bit address of the `.reload_cs` label into RAX
    push rax                ; Push target Instruction Pointer (`RIP`) onto the stack

    ; Execute Far Return (REX.W + LRET): pops RAX -> RIP, pops 0x08 -> CS, and jumps to `.reload_cs`
    db 0x48, 0xCB           ; REX.W prefix (0x48) + LRET far return (0xCB) for 64-bit Long Mode

.reload_cs:
    ; Step 3: Reload all Data Segment registers and Stack Segment with selector `0x10`
    mov ax, 0x10            ; Selector 0x10 = Kernel 64-Bit Data Segment (`Slot 2`)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Step 4: Load the Task Register (`TR`) with our 16-byte TSS descriptor selector `0x28`
    mov ax, 0x28            ; Selector 0x28 = System TSS Descriptor (`Slot 5`)
    ltr ax                  ; Load Task Register (TR)

    ret
