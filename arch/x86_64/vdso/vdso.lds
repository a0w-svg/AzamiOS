/* ============================================================================
 * AzamiOS — vDSO link layout (linux-vdso.so.1)
 *
 * One PT_LOAD at vaddr 0 holding everything, as on Linux: the kernel maps the
 * image verbatim at a page-aligned base, so p_vaddr == p_offset for every
 * section and the image must be fully position-independent with no dynamic
 * relocations (the Makefile verifies that). The vvar page sits two pages
 * below the image; vclock.c reaches it relative to __ehdr_start.
 * ============================================================================ */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

SECTIONS
{
    . = SIZEOF_HEADERS;

    .hash          : { *(.hash) }                   :text
    .gnu.hash      : { *(.gnu.hash) }
    .dynsym        : { *(.dynsym) }
    .dynstr        : { *(.dynstr) }
    .gnu.version   : { *(.gnu.version) }
    .gnu.version_d : { *(.gnu.version_d) }
    .gnu.version_r : { *(.gnu.version_r) }

    .dynamic       : { *(.dynamic) }                :text :dynamic

    .rodata        : { *(.rodata*) }                :text

    .note          : { *(.note.*) }                 :text :note

    .eh_frame_hdr  : { *(.eh_frame_hdr) }           :text :eh_frame_hdr
    .eh_frame      : { KEEP (*(.eh_frame)) }        :text

    /* Nothing may be writable: the page is mapped r-x into every process. */
    .data          : { *(.data*) *(.bss*) *(.got*) }

    . = ALIGN(16);
    .text          : { *(.text*) }                  :text =0x90909090

    /DISCARD/ : {
        *(.comment)
        *(.note.GNU-stack)
        *(.note.gnu.property)
    }
}

PHDRS
{
    text         PT_LOAD      FLAGS(5) FILEHDR PHDRS;   /* PF_R|PF_X */
    dynamic      PT_DYNAMIC   FLAGS(4);
    note         PT_NOTE      FLAGS(4);
    eh_frame_hdr PT_GNU_EH_FRAME;
}
