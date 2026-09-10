; arch/x86_64/lib/uaccess.asm
; ============================================================================
; AzamiOS — User Space Memory Access Implementation (Assembly)
; ============================================================================

global copy_from_user
global copy_to_user
global search_extable

extern __extable_start
extern __extable_end
extern g_smap_enabled

section .text

; ----------------------------------------------------------------------------
; size_t copy_from_user(void *dst, const void *src, size_t size)
; RDI = dst, RSI = src, RDX = size
; Returns remaining uncopied bytes in RAX (0 on success)
; ----------------------------------------------------------------------------
copy_from_user:
    test rdx, rdx
    jz .success

    ; Check if src + size wraps around or goes above user space limit (0x00007FFFFFFFFFFF)
    mov rax, rsi
    add rax, rdx
    jc .fail                 ; Overflow wrap-around
    ; BUG-O fix: exclusive upper bound is 0x0000800000000000; use jae (above-or-equal)
    ; so a single byte at 0x00007FFFFFFFFFFF is also rejected correctly.
    mov rcx, 0x0000800000000000
    cmp rax, rcx
    jae .fail                ; At or above user space boundary

    mov rcx, rdx

    ; If SMAP is enabled, set EFLAGS.AC via stac to allow ring 0 read from user page
    cmp byte [rel g_smap_enabled], 0
    je .do_copy
    stac

.do_copy:
.copy_insn:
    rep movsb

    ; If SMAP is enabled, clear EFLAGS.AC via clac to re-lock kernel mode
    cmp byte [rel g_smap_enabled], 0
    je .done
    clac

.done:
    mov rax, rcx
    ret

.success:
    xor eax, eax
    ret

.fail:
    mov rax, rdx
    ret

.fault_fixup:
    ; Close the SMAP window before anything else, so an NMI/#MC landing between
    ; the fault and this fixup cannot run with AC=1. CLAC is #UD on a CPU
    ; without SMAP, though, so it stays behind the same guard as the STAC that
    ; opened the window — an unconditional CLAC here turned a recoverable user
    ; fault into a kernel panic on any pre-Broadwell part.
    cmp byte [rel g_smap_enabled], 0
    je .ff_done
    clac
.ff_done:
    mov rax, rcx
    ret

; ----------------------------------------------------------------------------
; size_t copy_to_user(void *dst, const void *src, size_t size)
; RDI = dst, RSI = src, RDX = size
; Returns remaining uncopied bytes in RAX (0 on success)
; ----------------------------------------------------------------------------
copy_to_user:
    test rdx, rdx
    jz .success

    ; Check if dst + size wraps around or goes above user space limit
    mov rax, rdi
    add rax, rdx
    jc .fail
    ; BUG-O fix: exclusive upper bound is 0x0000800000000000; use jae (above-or-equal).
    mov rcx, 0x0000800000000000
    cmp rax, rcx
    jae .fail

    mov rcx, rdx

    ; If SMAP is enabled, set EFLAGS.AC via stac
    cmp byte [rel g_smap_enabled], 0
    je .do_copy
    stac

.do_copy:
.copy_insn:
    rep movsb

    ; If SMAP is enabled, clear EFLAGS.AC via clac
    cmp byte [rel g_smap_enabled], 0
    je .done
    clac

.done:
    mov rax, rcx
    ret

.success:
    xor eax, eax
    ret

.fail:
    mov rax, rdx
    ret

.fault_fixup:
    ; Same reasoning as copy_from_user.fault_fixup: guard the CLAC, it is #UD
    ; when the CPU has no SMAP.
    cmp byte [rel g_smap_enabled], 0
    je .ff_done
    clac
.ff_done:
    mov rax, rcx
    ret

; ----------------------------------------------------------------------------
; u64 search_extable(u64 ip)
; RDI = ip
; Returns fixup address, or 0 if not found
; ----------------------------------------------------------------------------
search_extable:
    mov rax, __extable_start
    mov rcx, __extable_end
.loop:
    cmp rax, rcx
    jae .not_found
    
    mov rdx, [rax]       ; Read 'insn' (first 8 bytes of extable_entry_t)
    cmp rdx, rdi
    je .found
    
    add rax, 16          ; sizeof(extable_entry_t) is 16 bytes
    jmp .loop
    
.found:
    mov rax, [rax + 8]   ; Read 'fixup' (second 8 bytes of extable_entry_t)
    ret
    
.not_found:
    xor rax, rax
    ret

; ----------------------------------------------------------------------------
; Exception Table (.extable)
; ----------------------------------------------------------------------------
section .extable
align 8
    ; struct extable_entry_t { u64 insn; u64 fixup; }
    
    ; Entry for copy_from_user
    dq copy_from_user.copy_insn
    dq copy_from_user.fault_fixup

    ; Entry for copy_to_user
    dq copy_to_user.copy_insn
    dq copy_to_user.fault_fixup
