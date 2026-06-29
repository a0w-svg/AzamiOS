ENTRY(boot)
SECTIONS
{
   .text 0x100000 :
   {
      code = .; _code = .; __code = .;
      *(.multiboot)
      *(.text*)
      . = ALIGN(4096);
   }
   
   .rodata :
   {
     rodata = .; _rodata = .; __rodata = .;
     *(.rodata*)
     *(.lrodata*)
     . = ALIGN(4096);
   }
   
   .data :
   {
     data = .; _data = .; __data = .;
     *(.data*)
     *(.ldata*)
     . = ALIGN(4096);
   }
   
   .bss :
   {
     bss = .; _bss = .; __bss = .;
     *(.bss*)
     *(.lbss*)
     *(COMMON)
     . = ALIGN(4096);
   }
   
   end = .; _end = .; __end = .;
}