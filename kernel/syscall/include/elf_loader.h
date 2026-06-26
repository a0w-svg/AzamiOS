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

/* ELF data encoding */
#define ELFDATA2LSB     1   /* little-endian */

/* e_type */
#define ET_EXEC         2   /* executable file */

/* e_machine */
#define EM_386          3   /* Intel 80386 */

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

/* ─── ELF32 structures ────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  e_ident[EI_NIDENT]; /* Magic number + identification */
    uint16_t e_type;             /* Object file type              */
    uint16_t e_machine;          /* Architecture                  */
    uint32_t e_version;          /* Object file version           */
    uint32_t e_entry;            /* Entry point virtual address   */
    uint32_t e_phoff;            /* Program header table offset   */
    uint32_t e_shoff;            /* Section header table offset   */
    uint32_t e_flags;            /* Processor-specific flags      */
    uint16_t e_ehsize;           /* ELF header size in bytes      */
    uint16_t e_phentsize;        /* Program header entry size     */
    uint16_t e_phnum;            /* Number of program headers     */
    uint16_t e_shentsize;        /* Section header entry size     */
    uint16_t e_shnum;            /* Number of section headers     */
    uint16_t e_shstrndx;         /* Section name string table idx */
} Elf32_Ehdr_t;

typedef struct {
    uint32_t p_type;    /* Segment type            */
    uint32_t p_offset;  /* Offset in file          */
    uint32_t p_vaddr;   /* Virtual address         */
    uint32_t p_paddr;   /* Physical address        */
    uint32_t p_filesz;  /* Segment size in file    */
    uint32_t p_memsz;   /* Segment size in memory  */
    uint32_t p_flags;   /* Segment flags (PF_*)    */
    uint32_t p_align;   /* Segment alignment       */
} Elf32_Phdr_t;

typedef struct {
    uint32_t sh_name;       /* Section name (index into .shstrtab) */
    uint32_t sh_type;       /* Section type                        */
    uint32_t sh_flags;      /* Section flags                       */
    uint32_t sh_addr;       /* Virtual address in memory           */
    uint32_t sh_offset;     /* Offset in file                      */
    uint32_t sh_size;       /* Section size in bytes               */
    uint32_t sh_link;       /* Link to another section             */
    uint32_t sh_info;       /* Additional section information      */
    uint32_t sh_addralign;  /* Section alignment                   */
    uint32_t sh_entsize;    /* Entry size if section holds table   */
} Elf32_Shdr_t;

/* ─── Return codes ────────────────────────────────────────────────────────── */
#define ELF_LOAD_OK             0
#define ELF_LOAD_ERR_NOT_ELF    1   /* bad magic                  */
#define ELF_LOAD_ERR_NOT_32     2   /* not ELF32                  */
#define ELF_LOAD_ERR_NOT_LSB    3   /* not little-endian          */
#define ELF_LOAD_ERR_NOT_EXEC   4   /* not an executable          */
#define ELF_LOAD_ERR_WRONG_ARCH 5   /* not i386                   */
#define ELF_LOAD_ERR_BAD_VER    6   /* unsupported ELF version    */
#define ELF_LOAD_ERR_NO_PHDR    7   /* no program headers         */
#define ELF_LOAD_ERR_NO_MEM     8   /* out of physical memory     */
#define ELF_LOAD_ERR_READ       9   /* file read failed           */

/* ─── Public API ──────────────────────────────────────────────────────────── */

/**
 * load_elf32 - validate and load an ELF32 executable into memory.
 *
 * @param file_node  VFS node of the file to load.
 * @param entry_out  On success, receives the program entry-point virtual addr.
 * @return           ELF_LOAD_OK on success, or an ELF_LOAD_ERR_* code.
 *
 * The function:
 *  1. Reads and validates the ELF32 header.
 *  2. Iterates over PT_LOAD program headers.
 *  3. Allocates physical frames and maps them into virtual memory (user-mode,
 *     writable) at the requested virtual address.
 *  4. Copies the file image into the mapped memory.
 *  5. Zero-fills any extra .bss bytes (p_memsz > p_filesz).
 */
int load_elf32(fs_node_t *file_node, uint32_t *entry_out);

#endif /* ELF_LOADER_H */
