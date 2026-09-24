; =============================================================================
; AzamiOS — kexec PTE-rewrite trampoline
; File: arch/x86_64/cpu/kexec_trampoline.asm
;
; This code never runs from where the linker actually places it. kexec_
; execute() (kernel/kexec.c) copies the raw bytes between kexec_trampoline
; and kexec_trampoline_end into a scratch physical page mapped at a throwaway
; kernel virtual address well away from both this kernel's own image and the
; HHDM, then calls it through a function pointer at that address.
;
; Why it has to move at all: this .asm file is linked into the running
; kernel's own .text section, i.e. somewhere inside the very
; [_text_start, _kernel_end) virtual range kexec_execute() is about to
; rewrite the leaf page-table entries for. Executing it in place would mean
; the rewrite loop overwrites the mapping for the instructions it is
; currently fetching, mid-loop, on live hardware — undefined at best. Running
; it from a page whose own mapping is never touched by the rewrite sidesteps
; that entirely.
;
; Because it runs from a different address than it was linked at, this code
; must be genuinely position-independent: no absolute references to any
; symbol, no RIP-relative loads of anything outside itself, no stack use
; (so no dependency on RSP holding anything sane), just register-to-register
; and register-indirect operations on whatever it was handed in registers.
;
; Calling convention: ordinary System V AMD64 (this is called through a
; plain C function pointer cast from kexec_execute(), not a custom ABI):
;   rdi = pointer to an array of kexec_poke_t { u64 *pte_ptr; u64 new_value; }
;         (16 bytes each), already HHDM-resident and fully built before this
;         is ever called — see kexec_execute() in kernel/kexec.c.
;   rsi = number of entries in that array
;   rdx = virtual address of the new kernel image's entry point (az_boot_
;         entry in the new image — same link-time address as this kernel's
;         own az_boot_entry, since kexec only targets a compatible build)
;
; It never returns: the CALL that reaches it pushes a return address onto
; whatever stack was live at the time, but that address is inside the old
; kernel's own image and is simply abandoned — nothing here ever executes
; RET. Once every poke is applied, the TLB is fully flushed and control
; jumps straight to the new entry point.
;
; Why a plain CR3 reload is not enough: every kernel mapping in this OS
; carries the GLOBAL bit (VMM_F_GLOBAL — see VMM_KERNEL_RX/RW in arch/x86_64/
; mm/vmm.h), specifically so an ordinary process context switch's CR3 write
; does not have to keep re-walking the kernel's own page tables. That same
; property makes an ordinary "mov cr3, cr3" self-reload *not* flush any of
; those entries (that is the architectural point of the global bit), so any
; kernel VA this core had already cached before the rewrite — which, having
; just spent the whole boot running as the old kernel, is most of the hot
; path — would keep resolving through the stale TLB entry to the *old*
; kernel's physical frame even after its PTE now says otherwise, producing
; exactly the kind of incoherent mixed old/new-frame reads and writes that a
; missed TLB invalidation always produces. Toggling CR4.PGE off and back on
; performs a full flush, global entries included (Intel SDM / AMD APM: a
; CR4.PGE 1→0 transition invalidates all TLB entries, not just non-global
; ones), and the trampoline's own page is a fresh mapping the CPU never
; cached anything stale for, so nothing here needs to survive that flush.
; =============================================================================

bits 64
section .text

global kexec_trampoline
global kexec_trampoline_end

kexec_trampoline:
    ; Interrupts must already be off by the time kexec_execute() calls this
    ; (every other CPU is parked with its own interrupts off, and the caller
    ; disables them here too before the call — see kernel/kexec.c). Repeating
    ; cli costs nothing and removes any doubt.
    cli

.poke_loop:
    test    rsi, rsi
    jz      .flush_and_jump
    mov     rax, [rdi]          ; kexec_poke_t.pte_ptr  (HHDM pointer to the live leaf PTE)
    mov     rcx, [rdi + 8]      ; kexec_poke_t.new_value (new phys | flags)
    mov     [rax], rcx          ; the actual, one-instruction "point this VA at the new kernel"
    add     rdi, 16
    dec     rsi
    jmp     .poke_loop

.flush_and_jump:
    ; Full TLB flush, global entries included — see the file banner for why
    ; a plain CR3 reload is not sufficient here. Clearing CR4.PGE (bit 7)
    ; flushes everything as a side effect of the 1->0 transition; setting it
    ; back merely re-enables the global-page TLB optimisation for whatever
    ; runs next (the new kernel), it does not need to flush anything itself
    ; since nothing new is cached in between here and the jump below.
    mov     rax, cr4
    mov     rcx, rax
    btr     rax, 7          ; clear PGE
    mov     cr4, rax
    mov     cr4, rcx        ; restore PGE (full flush already happened above)

    ; Point of no return: control now belongs to the new kernel image. It
    ; starts exactly where any fresh Limine boot would — az_boot_entry — and
    ; sets up its own stack before touching RSP for anything, so nothing
    ; about the incoming stack pointer here matters.
    jmp     rdx

kexec_trampoline_end:
