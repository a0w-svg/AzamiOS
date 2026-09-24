/* ============================================================================
 * AzamiOS — x86_64 Position-Independent Linker Script
 * File: scripts/user-pic.ld
 *
 * Shared by three build products (Phase 5a of the libc hardening work):
 *   - a PIE test executable (ld -pie --dynamic-linker=/lib/ld-azami.so -e _start)
 *   - a test shared library (ld -shared)
 *   - ld-azami.so itself (ld -shared)
 *
 * All three are ET_DYN — the kernel's elf_load_exec_internal()
 * (kernel/sched/elf.c) already loads ET_DYN images with ASLR through the
 * same generic load_elf_segments() path it uses for a static ET_EXEC, so
 * nothing on the kernel side needs to change for any of this to load.
 *
 * Linked at base 0 like a normal `ld -shared`/`-pie` output (confirmed by
 * inspecting this cross-toolchain's own default -shared output — no custom
 * PHDRS command needed, ld's built-in ET_DYN segment/section generation
 * already does the right thing for .dynamic/.dynsym/.hash/.rela.dyn/.interp).
 * Starting at SIZEOF_HEADERS (not a bare 0) is the same idiom
 * scripts/user.ld uses for the same reason: it makes ld's auto-generated
 * first PT_LOAD segment back-extend to cover file offset 0, so the ELF
 * header and program header table are part of the mapped image.
 *
 * userland/ldso/ldso.c relies on one fact this script guarantees: the
 * program header table always sits at file offset (and therefore vaddr,
 * since the covering segment starts at design-time vaddr 0) 64 — exactly
 * sizeof(Elf64_Ehdr) — which is what lets it recover a *loaded* image's
 * load bias as `AT_PHDR - 64` without needing a PT_PHDR segment (this
 * toolchain's ld doesn't emit one for a script-less -shared/-pie link
 * either) or auxv fields beyond what the kernel already provides.
 */
ENTRY(_start)

SECTIONS
{
    . = SIZEOF_HEADERS;

    .interp   : { *(.interp) }
    .hash     : { *(.hash) }
    .dynsym   : { *(.dynsym) }
    .dynstr   : { *(.dynstr) }
    .rela.dyn : { *(.rela.dyn) }
    .rela.plt : { *(.rela.plt) }

    .text : {
        *(.text._start)
        *(.text .text.*)
    }

    .rodata : ALIGN(0x1000) {
        *(.rodata .rodata.*)
    }

    .dynamic : ALIGN(0x1000) {
        *(.dynamic)
    }

    .got     : { *(.got) }
    .got.plt : { *(.got.plt) }

    /* GCC-emitted global/static constructor pointers, run by
     * userland/ldso/ldso.c's run_ctors_once() after relocation — see that
     * file. Writable: RELATIVE relocations fix each entry's absolute
     * address up in place before it's ever called, and this loader doesn't
     * implement PT_GNU_RELRO to re-lock the page afterward. */
    .init_array : ALIGN(8) {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(.init_array .init_array.*))
        PROVIDE_HIDDEN(__init_array_end = .);
    }
    .fini_array : ALIGN(8) {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(.fini_array .fini_array.*))
        PROVIDE_HIDDEN(__fini_array_end = .);
    }

    /* Thread-local storage template: userland/libc/tls.c's __init_tls() (a
     * statically linked binary) or userland/ldso/ldso.c's
     * setup_static_tls() (a dynamically linked one) copies .tdata and
     * zero-fills out to .tbss's extent into each thread's own block at
     * process/thread start — this is only ever the source image, never
     * touched again at runtime. ld auto-generates the PT_TLS segment
     * covering these two from their SHF_TLS section flag regardless of
     * where in the script they're placed, as long as they're contiguous. */
    .tdata : { *(.tdata .tdata.*) }
    .tbss : { *(.tbss .tbss.*) }

    .data : {
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
