; =========================================================================================
; Azami OS — Hardware Abstraction Layer (HAL) Low-Level CPU Assembly Stubs
; File: /kernel/hal/cpu/cpu_asm.asm
; Architecture: x86_64 (NASM syntax, System V AMD64 ABI)
;
; Description:
;   Provides assembly-level execution of instructions requiring direct hardware register
;   access (`CPUID`, `RDMSR`, `WRMSR`, `MOV CRn`). Every function adheres strictly to the
;   System V AMD64 Calling Convention:
;     Arguments: RDI, RSI, RDX, RCX, R8, R9
;     Return Value: RAX (64-bit integer or pointer)
;     Callee-Saved Registers: RBX, RSP, RBP, R12, R13, R14, R15
; =========================================================================================

bits 64
section .text

global hal_cpu_cpuid
global hal_cpu_rdmsr
global hal_cpu_wrmsr
global hal_cpu_get_cr0
global hal_cpu_set_cr0
global hal_cpu_get_cr2
global hal_cpu_get_cr3
global hal_cpu_set_cr3
global hal_cpu_get_cr4
global hal_cpu_set_cr4

; =========================================================================================
; void hal_cpu_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
;
; Arguments (System V AMD64 ABI):
;   RDI = leaf         (Input to EAX)
;   RSI = subleaf      (Input to ECX)
;   RDX = *eax pointer (Destination for EAX output)
;   RCX = *ebx pointer (Destination for EBX output)
;   R8  = *ecx pointer (Destination for ECX output)
;   R9  = *edx pointer (Destination for EDX output)
;
; Educational Note on Callee-Saved Register RBX:
;   In System V AMD64 ABI, RBX is designated as a CALLEE-SAVED register. Because the `cpuid`
;   instruction overwrites EBX (and zero-extends to RBX), failing to preserve RBX before
;   executing `cpuid` will corrupt any local variables the C caller stored in RBX!
;   Therefore, we explicitly `push rbx` at entry and `pop rbx` before returning.
; =========================================================================================
hal_cpu_cpuid:
    push rbp
    mov rbp, rsp
    push rbx                ; Preserve callee-saved RBX register before executing cpuid

    ; We need RDX and RCX pointers after cpuid, but cpuid overwrites EDX and ECX!
    ; Let's save the destination pointers onto the stack so cpuid doesn't destroy them.
    push rdx                ; Save *eax pointer
    push rcx                ; Save *ebx pointer
    push r8                 ; Save *ecx pointer
    push r9                 ; Save *edx pointer

    ; Prepare input registers for cpuid instruction
    mov eax, edi            ; EAX = leaf (Argument 1)
    mov ecx, esi            ; ECX = subleaf (Argument 2)

    ; Execute CPUID: queries processor capabilities, returning data in EAX, EBX, ECX, EDX
    cpuid

    ; Pop the saved destination pointers back into scratch registers R8, R9, R10, R11
    pop r11                 ; R11 = *edx pointer (originally pushed from R9)
    pop r10                 ; R10 = *ecx pointer (originally pushed from R8)
    pop r9                  ; R9  = *ebx pointer (originally pushed from RCX)
    pop r8                  ; R8  = *eax pointer (originally pushed from RDX)

    ; Store results into the caller's output pointers (checking if pointer is non-null)
    test r8, r8
    jz .skip_eax
    mov [r8], eax           ; *eax = output EAX
.skip_eax:

    test r9, r9
    jz .skip_ebx
    mov [r9], ebx           ; *ebx = output EBX
.skip_ebx:

    test r10, r10
    jz .skip_ecx
    mov [r10], ecx          ; *ecx = output ECX
.skip_ecx:

    test r11, r11
    jz .skip_edx
    mov [r11], edx          ; *edx = output EDX
.skip_edx:

    pop rbx                 ; Restore callee-saved RBX register
    mov rsp, rbp
    pop rbp
    ret

; =========================================================================================
; uint64_t hal_cpu_rdmsr(uint32_t msr)
;
; Arguments:
;   RDI = MSR address (Argument 1)
;
; Returns:
;   RAX = 64-bit value read from the specified Model-Specific Register
;
; Educational Note on RDMSR:
;   The `rdmsr` instruction reads the MSR specified in ECX into EDX:EAX (`EDX` holds
;   bits [32..63] and `EAX` holds bits [0..31]). To return a clean 64-bit integer
;   in RAX as required by C, we shift RDX left by 32 bits and OR it into RAX.
; =========================================================================================
hal_cpu_rdmsr:
    mov ecx, edi            ; ECX = target MSR index
    rdmsr                   ; Reads MSR -> EDX (high 32 bits), EAX (low 32 bits)

    shl rdx, 32             ; Shift EDX up into bits [32..63] of RDX
    or rax, rdx             ; Combine EDX:EAX into a single 64-bit value inside RAX
    ret

; =========================================================================================
; void hal_cpu_wrmsr(uint32_t msr, uint64_t value)
;
; Arguments:
;   RDI = MSR address (Argument 1)
;   RSI = 64-bit value to write (Argument 2)
;
; Educational Note on WRMSR:
;   The `wrmsr` instruction writes the 64-bit value formed by EDX:EAX into the MSR
;   specified by ECX. We extract the lower 32 bits of RSI into EAX and the upper 32 bits
;   into EDX prior to execution.
; =========================================================================================
hal_cpu_wrmsr:
    mov ecx, edi            ; ECX = target MSR index
    mov rax, rsi            ; RAX = value (low 32 bits automatically in EAX)
    mov rdx, rsi
    shr rdx, 32             ; RDX = value >> 32 (high 32 bits)
    wrmsr                   ; Writes EDX:EAX into MSR[ECX]
    ret

; =========================================================================================
; uint64_t hal_cpu_get_cr0(void)
; Reads and returns Control Register 0 (Protection, Paging, Cache controls).
; =========================================================================================
hal_cpu_get_cr0:
    mov rax, cr0
    ret

; =========================================================================================
; void hal_cpu_set_cr0(uint64_t val)
; Updates Control Register 0 with the value in RDI.
; =========================================================================================
hal_cpu_set_cr0:
    mov cr0, rdi
    ret

; =========================================================================================
; uint64_t hal_cpu_get_cr2(void)
; Reads and returns Control Register 2 (Page Fault Linear Address).
; When `#PF` (vector 14) fires, CR2 contains the exact virtual address causing the fault.
; =========================================================================================
hal_cpu_get_cr2:
    mov rax, cr2
    ret

; =========================================================================================
; uint64_t hal_cpu_get_cr3(void)
; Reads Control Register 3 (Physical Base Address of PML4 / Page Directory).
; =========================================================================================
hal_cpu_get_cr3:
    mov rax, cr3
    ret

; =========================================================================================
; void hal_cpu_set_cr3(uint64_t val)
; Updates Control Register 3 with the value in RDI.
; Note: Writing to CR3 automatically flushes non-global entries in the Translation Lookaside
; Buffer (TLB), ensuring page table modifications take immediate effect across memory accesses!
; =========================================================================================
hal_cpu_set_cr3:
    mov cr3, rdi
    ret

; =========================================================================================
; uint64_t hal_cpu_get_cr4(void)
; Reads Control Register 4 (PAE, PGE, OSFXSR, OSXMMEXCPT flags).
; =========================================================================================
hal_cpu_get_cr4:
    mov rax, cr4
    ret

; =========================================================================================
; void hal_cpu_set_cr4(uint64_t val)
; Updates Control Register 4 with the value in RDI.
; =========================================================================================
hal_cpu_set_cr4:
    mov cr4, rdi
    ret
