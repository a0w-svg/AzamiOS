; =============================================================================
; AzamiOS — Instruction probes for the hwaccel dispatcher
; File: arch/x86_64/cpu/hwprobe.asm
;
; CPUID advertising an instruction is a claim, not a guarantee: emulators and
; some hypervisors set feature bits for instructions they then #UD on, and a
; #UD taken from a kernel fast path is an unrecoverable boot failure rather
; than a graceful fallback. Every candidate instruction below sits behind an
; .extable fixup, exactly as xsave_probe_asm does, so hwaccel_init() can run
; each one once against a scratch line and demote itself when one faults.
; =============================================================================
bits 64

global hwaccel_probe_asm

section .text

; int hwaccel_probe_asm(void *scratch, u32 which);
; RDI = 64-byte aligned scratch area of at least 128 bytes
; ESI = PROBE_* selector (see hwaccel.c)
; Returns 0 if the instruction executed, 1 if it faulted.
hwaccel_probe_asm:
    cmp  esi, 0                  ; PROBE_CLZERO
    je   .clzero
    cmp  esi, 3                  ; PROBE_TPAUSE
    je   .tpause
    cmp  esi, 4                  ; PROBE_RDPMC
    je   .rdpmc
    mov  eax, 1                  ; unknown selector — report unusable
    ret

.clzero:
    ; CLZERO zeroes the line containing RAX. The caller's scratch is a full
    ; line wide, so nothing outside it can be touched.
    mov  rax, rdi
.probe_clzero:
    clzero
    jmp  .ok

.tpause:
    ; A deadline of 0 is unconditionally in the past, so TPAUSE returns at once
    ; (with CF set) instead of parking the boot CPU for a real interval.
    xor  edx, edx
    xor  eax, eax
    xor  ecx, ecx                ; ECX bit 0 = 0 -> request C0.2
.probe_tpause:
    tpause ecx
    jmp  .ok

.rdpmc:
    ; Counter 0, which pmu_init() has just zeroed. Reading an implemented but
    ; unprogrammed counter is architecturally fine and yields 0; what is being
    ; tested is whether RDPMC executes at all. CPUID enumerating a PMU is not
    ; enough — QEMU's TCG raises #UD from RDPMC unconditionally, whatever leaf
    ; 0xA claimed — so the only honest test is to run the instruction.
    xor  ecx, ecx
.probe_rdpmc:
    rdpmc
    jmp  .ok

.ok:
    xor  eax, eax
    ret
.probe_fault:
    mov  eax, 1
    ret

; =============================================================================
; Exception table entries. search_extable() matches a faulting kernel RIP
; against the first quadword of each 16-byte pair and resumes at the second.
; =============================================================================
section .extable
align 8
    dq hwaccel_probe_asm.probe_clzero
    dq hwaccel_probe_asm.probe_fault

    dq hwaccel_probe_asm.probe_tpause
    dq hwaccel_probe_asm.probe_fault

    dq hwaccel_probe_asm.probe_rdpmc
    dq hwaccel_probe_asm.probe_fault
