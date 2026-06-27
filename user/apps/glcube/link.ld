ENTRY(_start)

SECTIONS
{
    . = 0x10000000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(COMMON) *(.bss*) }
    . = ALIGN(4096);
    __end = .;
    __heap_start = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
