/* ============================================================================
 * AzamiOS — x86_64 Userspace Linker Script
 * File: user/user.ld
 * Entry point at virtual address 0x400000 (Ring 3 user address space)
 * ============================================================================ */
ENTRY(_start)

SECTIONS
{
    . = 0x0000000000400000;

    .text : {
        *(.text._start)
        *(.text .text.*)
    }

    .rodata : ALIGN(0x1000) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(0x1000) {
        *(.data .data.*)
    }

    .bss : ALIGN(0x1000) {
        *(COMMON)
        *(.bss .bss.*)
    }

    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
        *(.note*)
    }
}
