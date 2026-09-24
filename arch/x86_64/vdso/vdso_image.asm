; ============================================================================
; AzamiOS — embedded vDSO image
; File: arch/x86_64/vdso/vdso_image.asm
;
; Pulls the stripped linux-vdso.so.1 produced by the Makefile's vDSO rules
; into the kernel's read-only data. vdso_init() copies it into page-aligned
; frames at boot, so no particular alignment is needed here.
; ============================================================================

section .rodata

global vdso_image_start
global vdso_image_end

align 16
vdso_image_start:
    incbin "build/vdso/vdso.so"
vdso_image_end:

section .note.GNU-stack noalloc noexec nowrite progbits
