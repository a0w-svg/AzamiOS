#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stdint.h>
/* fs_node_t is defined here; include path is relative to where elf_loader.h
 * sits (kernel/syscall/include/), so step up two levels to reach filesystem. */
#include "../../filesystem/include/vfs.h"

/* ─── ELF magic & identification ─────────────────────────────────────────── */
#define ELF_MAGIC       0x464C457FU   /* little-endian  \x7fELF */

/* e_ident indices */
#define EI_CLASS        4
#define EI_DATA         5
#define EI_VERSION      6
#define EI_OSABI        7
#define EI_NIDENT       16

/* ELF class */
#define ELFCLASS32      1
#define ELFCLASS64      2

/* ELF data encoding */
#define ELFDATA2LSB     1   /* little-endian */

/* e_type */
#define ET_EXEC         2   /* executable file */
#define ET_DYN          3   /* shared object / Linux PIE executable */

/* e_machine */
#define EM_386          3   /* Intel 80386 */
#define EM_X86_64       62  /* AMD x86-64 */

/* e_version / EV_CURRENT */
#define EV_CURRENT      1

/* ─── Program-header p_type ───────────────────────────────────────────────── */
#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_SHLIB        5
#define PT_PHDR         6

/* p_flags */
#define PF_X            0x1   /* execute */
#define PF_W            0x2   /* write   */
#define PF_R            0x4   /* read    */

/* ─── ELF64 structures ────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr_t;

#define ELF_TARGET_CLASS   ELFCLASS64
#define ELF_TARGET_MACHINE EM_X86_64
typedef Elf64_Ehdr_t Elf_Ehdr_t;
typedef Elf64_Phdr_t Elf_Phdr_t;

/* ─── Return codes ────────────────────────────────────────────────────────── */
#define ELF_LOAD_OK             0
#define ELF_LOAD_ERR_NOT_ELF    1   /* bad magic                  */
#define ELF_LOAD_ERR_NOT_32     2   /* not ELF32 / wrong class    */
#define ELF_LOAD_ERR_NOT_LSB    3   /* not little-endian          */
#define ELF_LOAD_ERR_NOT_EXEC   4   /* not an executable          */
#define ELF_LOAD_ERR_WRONG_ARCH 5   /* not i386                   */
#define ELF_LOAD_ERR_BAD_VER    6   /* unsupported ELF version    */
#define ELF_LOAD_ERR_NO_PHDR    7   /* no program headers         */
#define ELF_LOAD_ERR_NO_MEM     8   /* out of physical memory     */
#define ELF_LOAD_ERR_READ       9   /* file read failed           */

/* ─── Public API ──────────────────────────────────────────────────────────── */

/**
 * load_elf - validate and load an ELF64 executable into memory.
 *
 * @param file_node  VFS node of the file to load.
 * @param entry_out  On success, receives the program entry-point virtual addr.
 * @return           ELF_LOAD_OK on success, or an ELF_LOAD_ERR_* code.
 */
int load_elf(fs_node_t *file_node, uintptr_t *entry_out);

#endif /* ELF_LOADER_H */
