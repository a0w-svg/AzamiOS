/* A real position-independent ELF interpreter entry. Verify the kernel's
 * initial stack/auxv, then terminate with 41. The main image exits with 42,
 * so accidentally bypassing this interpreter is observable. */
.text
.global _start
_start:
    mov $99, %edi
    test $15, %rsp
    jnz done
    cmpq $2, (%rsp)
    jne done
    lea 8(%rsp), %rsi
argv:
    lodsq
    test %rax, %rax
    jnz argv
envp:
    lodsq
    test %rax, %rax
    jnz envp
    xor %r8d, %r8d
auxv:
    lodsq
    mov %rax, %rcx
    lodsq
    test %rcx, %rcx
    jz checked
    cmp $7, %rcx                 /* AT_BASE */
    jne entry
    test %rax, %rax
    jz done
    or $1, %r8d
entry:
    cmp $9, %rcx                 /* AT_ENTRY */
    jne phdr
    cmp $0x400200, %rax
    jne done
    or $2, %r8d
phdr:
    cmp $3, %rcx                 /* AT_PHDR */
    jne auxv
    cmp $0x400040, %rax
    jne done
    or $4, %r8d
    jmp auxv
checked:
    cmp $7, %r8d
    jne done
    mov $32, %edi               /* FD_CLOEXEC descriptor must be gone */
    mov $1, %esi                /* F_GETFD */
    mov $72, %eax               /* fcntl */
    syscall
    mov $99, %edi
    cmp $-9, %rax
    jne done
    mov $33, %edi               /* Ordinary descriptor must survive */
    mov $72, %eax
    syscall
    mov $99, %edi
    test %rax, %rax
    jnz done
    mov $41, %edi
done:
    mov $60, %eax
    syscall
    ud2
.section .note.GNU-stack,"",@progbits
