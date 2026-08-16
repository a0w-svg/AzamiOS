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
    mov rcx, 0x00007FFFFFFFFFFF
    cmp rax, rcx
    ja .fail                 ; Above user space boundary

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
    ; If fault occurred with SMAP active, ensure EFLAGS.AC is cleared
    cmp byte [rel g_smap_enabled], 0
    je .fixup_done
    clac
.fixup_done:
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
    mov rcx, 0x00007FFFFFFFFFFF
    cmp rax, rcx
    ja .fail

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
    cmp byte [rel g_smap_enabled], 0
    je .fixup_done
    clac
.fixup_done:
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
