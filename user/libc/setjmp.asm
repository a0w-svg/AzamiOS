; setjmp.asm — AzamiOS libc: non-local jumps (x86 32-bit)
[BITS 32]

global setjmp
global longjmp

section .text

; int setjmp(jmp_buf env)
; jmp_buf layout: [0]=EBX, [1]=ESI, [2]=EDI, [3]=EBP, [4]=ESP, [5]=EIP
setjmp:
    mov edx, [esp + 4]      ; edx = env
    mov [edx + 0], ebx
    mov [edx + 4], esi
    mov [edx + 8], edi
    mov [edx + 12], ebp
    
    lea eax, [esp + 4]      ; caller's ESP before call
    mov [edx + 16], eax
    
    mov eax, [esp]          ; return address (EIP)
    mov [edx + 20], eax
    
    xor eax, eax            ; return 0
    ret

; void longjmp(jmp_buf env, int val)
longjmp:
    mov edx, [esp + 4]      ; edx = env
    mov eax, [esp + 8]      ; eax = val
    test eax, eax
    jnz .val_ok
    inc eax                 ; val cannot be 0, return 1 if 0 passed
.val_ok:
    mov ebx, [edx + 0]
    mov esi, [edx + 4]
    mov edi, [edx + 8]
    mov ebp, [edx + 12]
    mov esp, [edx + 16]
    jmp [edx + 20]          ; jump to saved EIP
