/* ============================================================================
 * AzamiOS — Kernel Linker Script
 * File: kernel.ld
 *
 * Virtual memory layout (x86_64 higher-half):
 *
 *   0xFFFFFFFF80000000  ← KERNEL_VIRT_BASE  (-2 GB)
 *     .text   (rx)
 *     .rodata (r)
 *     .data   (rw)
 *     .bss    (rw)
 *
 * The Limine bootloader loads the kernel ELF at the physical address
 * specified by the PT_LOAD program headers, and maps it into the virtual
 * address range declared here. The HHDM provides a separate, flat map of
 * all physical memory at 0xFFFF800000000000.
 *
 * The linker script is consumed by x86_64-elf-ld; it uses the 'limine'
 * ELF style (no PT_NOTE needed — Limine uses a request section instead).
 * ============================================================================ */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

ENTRY(az_boot_entry)

KERNEL_BASE = 0xFFFFFFFF80000000;

PHDRS {
    text    PT_LOAD FLAGS(5);   /* R-X */
    rodata  PT_LOAD FLAGS(4);   /* R-- */
    data    PT_LOAD FLAGS(6);   /* RW- */
    dynamic PT_DYNAMIC;
}

SECTIONS {
    /* ── Text (executable + read-only) ───────────────────────────────────── */
    . = KERNEL_BASE + 0x100000;  /* 1 MB offset inside the kernel VA range */

    .text : {
        _text_start = .;
        KEEP(*(.limine_requests_start_marker))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end_marker))
        *(.text .text.*)
        _text_end = .;
    } :text

    . = ALIGN(4096);

    /* ── Read-only data ───────────────────────────────────────────────────── */
    .rodata : {
        _rodata_start = .;
        *(.rodata .rodata.*)
        _rodata_end = .;
    } :rodata

    . = ALIGN(4096);

    /* ── Exception Table ──────────────────────────────────────────────────── */
    .extable : {
        __extable_start = .;
        *(.extable)
        __extable_end = .;
    } :rodata

    . = ALIGN(4096);

    /* ── Initialised data ─────────────────────────────────────────────────── */
    .data : {
        _data_start = .;
        *(.data .data.*)
        _data_end = .;
    } :data

    . = ALIGN(4096);

    /* ── Uninitialised data (zero-filled by entry.asm) ────────────────────── */
    .bss : {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
    } :data

    . = ALIGN(4096);
    _kernel_end = .;

    /* ── Discard sections the kernel doesn't need ─────────────────────────── */
    /DISCARD/ : {
        *(.note.GNU-stack)
        *(.eh_frame .eh_frame_hdr)
        *(.comment)
    }
}
