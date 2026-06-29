ENTRY(boot64_entry)
PHDRS
{
   text PT_LOAD FLAGS(5);
   data PT_LOAD FLAGS(6);
   note PT_NOTE FLAGS(4);
}
SECTIONS
{
   . = 0x400000;
   .note : { *(.note.Xen) *(.note.*) } :note :text
   .text :
   {
      code = .; _code = .; __code = .;
      *(.text*)
      . = ALIGN(4096);
   } :text
   .rodata :
   {
      rodata = .; _rodata = .; __rodata = .;
      *(.rodata*)
      *(.lrodata*)
      . = ALIGN(4096);
   } :text
   .data :
   {
      data = .; _data = .; __data = .;
      *(.data*)
      *(.ldata*)
      . = ALIGN(4096);
   } :data
   .bss :
   {
      bss = .; _bss = .; __bss = .;
      *(.bss*)
      *(.lbss*)
      *(COMMON)
      . = ALIGN(4096);
   } :data
   end = .; _end = .; __end = .;
   /DISCARD/ : { *(.note.GNU-stack) *(.eh_frame) }
}
