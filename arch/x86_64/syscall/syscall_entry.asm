; =============================================================================
; AzamiOS — SYSCALL / SYSRET Entry Stub (x86_64)
; File: arch/x86_64/syscall/syscall_entry.asm
;
; Setup (called once from kernel_main via syscall_abi_init()):
;   We write three MSRs:
;     STAR  [63:48] = SYSRET  CS base (user CS = this + 16, user SS = this + 8)
;     STAR  [47:32] = SYSCALL CS base (kernel CS = this,    kernel SS = this + 8)
;     LSTAR = 64-bit address of az_syscall_entry (this function)
;     SFMASK = bits to clear in RFLAGS on SYSCALL (we clear IF to block interrupts)
;
; Calling convention when SYSCALL fires:
;   On entry from user space:
;     RAX = syscall number
;     RDI = arg1, RSI = arg2, RDX = arg3, R10 = arg4, R8 = arg5, R9 = arg6
;     RCX = user RIP (saved by CPU before SYSCALL)
;     R11 = user RFLAGS (saved by CPU before SYSCALL)
;     RSP = user RSP  ← NOT switched by hardware; we must switch it
;
; Frame we build on the kernel stack (matches pt_regs_t in idt.h):
;   We reuse the same pt_regs_t layout as the exception path so the syscall
;   dispatcher can read arguments from the same struct the IRQ path uses.
;   This means register save/restore in the syscall path is identical in layout.
;
; SYSRET restores:
;   RIP  from RCX  (we place user RIP there)
;   RFLAGS from R11  (we restore original user RFLAGS)
;   RSP is restored by us from the saved pt_regs.rsp before SYSRETQ.
; =============================================================================

bits 64
section .text

extern syscall_dispatch         ; defined in kernel/syscall/syscall.c
extern g_cpu_kernel_stack       ; phys/virt table: per-CPU kernel RSP0 values

global az_syscall_entry
global syscall_abi_init

; =============================================================================
; syscall_abi_init() — Write STAR/LSTAR/SFMASK MSRs (call once from C).
; =============================================================================
;
; STAR layout (MSR 0xC0000081):
;   bits [63:48] = SYSRET  CS/SS base selector (user   code = base+16, data = base+8)
;   bits [47:32] = SYSCALL CS/SS base selector (kernel code = base+0,  data = base+8)
;
; We set:
;   SYSRET  base = 0x10  → user DS = 0x18, user CS = 0x20  (matches our GDT layout)
;   SYSCALL base = 0x08  → kernel CS = 0x08, kernel DS = 0x10
;
; SFMASK (MSR 0xC0000084):
;   Bits set here are cleared in RFLAGS when SYSCALL executes.
;   0x200 = IF (interrupt flag) — disable interrupts on syscall entry.
;   0x100 = TF (trap flag)      — don't single-step into the kernel.
;   We combine: SFMASK = IF | TF | AC (alignment check).
;
syscall_abi_init:
    push rbx

    ; ── Enable SCE (System Call Extension) in EFER ───────────────────────────
    mov  ecx, 0xC0000080        ; MSR_EFER
    rdmsr
    or   eax, 1                 ; set SCE bit
    wrmsr

    ; ── Write STAR ───────────────────────────────────────────────────────────
    ; bits[63:48] = 0x0013 (SYSRET  base: gives user  SS=0x1B, CS=0x23)
    ; bits[47:32] = 0x0008 (SYSCALL base: gives kernel CS=0x08, SS=0x10)
    ; bits[31:0]  = 0 (reserved)
    ;
    ; Explanation: CPU sets CS = STAR[47:32] on SYSCALL (kernel code).
    ;              CPU sets SS = STAR[47:32]+8 on SYSCALL (kernel data).
    ;              CPU sets CS = STAR[63:48]+16 on SYSRETQ (user code).
    ;              CPU sets SS = STAR[63:48]+8  on SYSRETQ (user data).
    ;
    ; With SYSCALL base = 0x08: kernel CS = 0x08, kernel SS = 0x10 ✓
    ; With SYSRET  base = 0x10: user   SS = 0x18 | RPL3 = 0x1B,
    ;                            user   CS = 0x20 | RPL3 = 0x23 ✓
    mov  ecx, 0xC0000081        ; MSR_STAR
    xor  edx, edx               ; EDX = bits [63:32] of STAR
    mov  eax, 0                 ; EAX = bits [31:0]  of STAR (reserved = 0)
    mov  edx, (0x0010 << 16) | 0x0008  ; [63:48]=0x10, [47:32]=0x08
    wrmsr

    ; ── Write LSTAR (64-bit syscall handler address) ──────────────────────────
    mov  ecx, 0xC0000082        ; MSR_LSTAR
    lea  rax, [rel az_syscall_entry]
    mov  rdx, rax
    shr  rdx, 32                ; EDX = bits [63:32]
    wrmsr

    ; ── Write CSTAR (compat-mode handler: just halt — we don't support 32-bit) ─
    mov  ecx, 0xC0000083        ; MSR_CSTAR
    xor  eax, eax
    xor  edx, edx
    wrmsr

    ; ── Write SFMASK ──────────────────────────────────────────────────────────
    ; Clear IF (bit 9), TF (bit 8), and AC (bit 18) on syscall entry.
    mov  ecx, 0xC0000084        ; MSR_SFMASK
    mov  eax, (1 << 9) | (1 << 8) | (1 << 18)
    xor  edx, edx
    wrmsr

    pop  rbx
    ret

; =============================================================================
; az_syscall_entry — Kernel-side SYSCALL handler
;
; At entry:
;   RAX = syscall number
;   RCX = user RIP (SYSCALL saves it here)
;   R11 = user RFLAGS
;   RSP = user RSP (NOT switched by hardware)
;   IF  = 0  (cleared by SFMASK)
; =============================================================================
az_syscall_entry:
    ; ── 1. Save user RSP and switch to kernel stack ───────────────────────────
    ; SWAPGS: swap GS.base with IA32_KERNEL_GS_BASE so GS now points to the
    ; per-CPU data block (which contains the per-CPU kernel stack pointer).
    swapgs
    mov  [gs:0x08], rsp          ; save user RSP into per-CPU slot (offset 8)
    mov  rsp, [gs:0x00]          ; load kernel RSP0 from per-CPU slot (offset 0)

    ; ── 2. Build pt_regs_t frame on the kernel stack ──────────────────────────
    ; Push in reverse order of the struct so the stack pointer at the end
    ; equals the address of the first field (ds) in pt_regs_t.
    push qword 0x1B              ; ss  (user data RPL3)
    push qword [gs:0x08]         ; rsp (user RSP we saved above)
    push r11                     ; rflags (user RFLAGS from R11)
    push qword 0x23              ; cs  (user code RPL3)
    push rcx                     ; rip (user RIP from RCX)
    push qword 0                 ; err_code (none for syscall)
    push qword 0x80              ; int_no = 128 (syscall pseudo-vector)

    ; Push general-purpose registers.
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Save and reload data segment.
    mov  rax, ds
    push rax
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax

    ; ── 3. Re-enable interrupts (kernel can be preempted during syscall) ───────
    sti

    ; ── 4. Call C dispatcher with pt_regs pointer ─────────────────────────────
    mov  rdi, rsp                ; arg0 = pt_regs *
    cld
    mov  rbx, rsp                ; Save original RSP
    and  rsp, -16                ; Align stack to 16 bytes for System V ABI
    call syscall_dispatch
    mov  rsp, rbx                ; Restore original RSP

    ; ── 5. Restore frame ───────────────────────────────────────────────────────
    cli                          ; disable interrupts before we touch RSP

    pop  rax                     ; restore ds (discard, we handle it below)
    mov  ds, ax
    mov  es, ax

    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rbp
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx                     ; scratch — overwritten below
    pop  rbx
    pop  rax                     ; RAX = syscall return value

    add  rsp, 16                 ; skip int_no and err_code

    ; ── 6. Set up SYSRETQ registers ───────────────────────────────────────────
    pop  rcx                     ; RCX = user RIP  (for SYSRETQ)
    add  rsp, 8                  ; skip CS
    pop  r11                     ; R11 = user RFLAGS (for SYSRETQ)
    pop  rsp                     ; RSP = user RSP

    ; Restore kernel GS base (swapgs: kernel GS → user GS).
    swapgs

    ; SYSRETQ: returns to RCX (user RIP), restores RFLAGS from R11, DPL → ring 3.
    ; Encoded as 48 0F 07 (REX.W + SYSRET) because older assemblers may not
    ; recognise the mnemonic.
    db 0x48, 0x0F, 0x07          ; sysretq
