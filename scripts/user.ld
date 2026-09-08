/* ============================================================================
 * AzamiOS — x86_64 Userspace Linker Script
 * File: user/user.ld
 * Entry point at virtual address 0x400000 + SIZEOF_HEADERS (Ring 3 user
 * address space) — see the SIZEOF_HEADERS note below for why it isn't a
 * round number.
 * ============================================================================ */
ENTRY(_start)

SECTIONS
{
    /* Starting .text at BASE + SIZEOF_HEADERS rather than a bare BASE is a
     * standard idiom (used by, e.g., the Linux kernel's own boot stubs) for
     * a PHDRS-less script like this one: it makes ld's auto-generated first
     * PT_LOAD segment back-extend to cover file offset 0, i.e. the ELF
     * header and program header table become part of that segment's mapped
     * image instead of sitting in an unmapped gap before it. AzamiOS binaries
     * carry no PT_PHDR (this script has no explicit PHDRS command emitting
     * one), so the *only* way a running process can locate its own program
     * headers — needed to find PT_TLS for TLS setup, see
     * userland/libc/tls.c's __init_tls() — is the kernel's
     * load_elf_segments() "phdr_from_load" fallback in kernel/sched/elf.c,
     * and that fallback only finds anything if e_phoff falls inside some
     * PT_LOAD's file range. Confirmed by the AT_PHDR=0 auxv value a binary
     * got before this line existed. The kernel loader reads the real e_entry
     * out of the ELF header, so a non-round entry point here is not a
     * problem for anything that follows. */
    . = 0x0000000000400000 + SIZEOF_HEADERS;

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

    /* Thread-local storage template (variant II, x86-64 psABI). ld
     * auto-synthesizes a PT_TLS program header spanning .tdata+.tbss since
     * no explicit PHDRS command overrides it. userland/libc/tls.c reads that
     * PT_TLS header back out of the loaded image via auxv (AT_PHDR) rather
     * than trusting a linker symbol to match ld's alignment padding, so
     * nothing here needs to export a size symbol. */
    .tdata : ALIGN(0x1000) {
        *(.tdata .tdata.*)
    }

    .tbss : ALIGN(8) {
        *(.tbss .tbss.*)
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
