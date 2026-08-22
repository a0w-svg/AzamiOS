/* ============================================================================
 * AzamiOS — 64-bit ELF Executable Loader Header (Linux/POSIX Compatible)
 * File: kernel/sched/elf.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "sched.h"

/* ── ELF Magic & Identification ──────────────────────────────────────────── */
#define ELF_MAGIC       0x464C457F /* "\x7FELF" in little-endian */
#define ELFMAG0         0x7F
#define ELFMAG1         'E'
#define ELFMAG2         'L'
#define ELFMAG3         'F'

#define EI_MAG0         0
#define EI_MAG1         1
#define EI_MAG2         2
#define EI_MAG3         3
#define EI_CLASS        4
#define EI_DATA         5
#define EI_VERSION      6
#define EI_OSABI        7
#define EI_ABIVERSION   8
#define EI_PAD          9
#define EI_NIDENT       16

#define ELFCLASSNONE    0
#define ELFCLASS32      1
#define ELFCLASS64      2

#define ELFDATANONE     0
#define ELFDATA2LSB     1 /* Little endian */
#define ELFDATA2MSB     2 /* Big endian */

#define EV_CURRENT      1

#define ELFOSABI_NONE   0
#define ELFOSABI_SYSV   0
#define ELFOSABI_LINUX  3

/* ── ELF Object File Types ────────────────────────────────────────────────── */
#define ET_NONE         0
#define ET_REL          1
#define ET_EXEC         2 /* Static / Fixed executable */
#define ET_DYN          3 /* Position Independent Executable (PIE) / Shared Object */
#define ET_CORE         4

/* ── Target Architecture ──────────────────────────────────────────────────── */
#define EM_NONE         0
#define EM_386          3
#define EM_X86_64       62

/* ── Program Header Types (p_type) ────────────────────────────────────────── */
#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_SHLIB        5
#define PT_PHDR         6
#define PT_TLS          7
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_STACK    0x6474e551
#define PT_GNU_RELRO    0x6474e552

/* ── Program Header Flags (p_flags) ───────────────────────────────────────── */
#define PF_X            0x1 /* Executable */
#define PF_W            0x2 /* Writable */
#define PF_R            0x4 /* Readable */

/* ── Linux Auxiliary Vector Types (auxv) ──────────────────────────────────── */
#define AT_NULL         0  /* End of vector */
#define AT_IGNORE       1  /* Entry should be ignored */
#define AT_EXECFD       2  /* File descriptor of program */
#define AT_PHDR         3  /* Program headers for program */
#define AT_PHENT        4  /* Size of program header entry */
#define AT_PHNUM        5  /* Number of program headers */
#define AT_PAGESZ       6  /* System page size */
#define AT_BASE         7  /* Base address of interpreter */
#define AT_FLAGS        8  /* Flags */
#define AT_ENTRY        9  /* Entry point of program */
#define AT_NOTELF       10 /* Program is not ELF */
#define AT_UID          11 /* Real UID */
#define AT_EUID         12 /* Effective UID */
#define AT_GID          13 /* Real GID */
#define AT_EGID         14 /* Effective GID */
#define AT_PLATFORM     15 /* String identifying platform ("x86_64") */
#define AT_HWCAP        16 /* CPU capabilities */
#define AT_CLKTCK       17 /* Frequency at which times() ticks (100) */
#define AT_SECURE       23 /* Boolean for secure mode */
#define AT_BASE_PLATFORM 24
#define AT_RANDOM       25 /* 16 random bytes on stack */
#define AT_HWCAP2       26
#define AT_EXECFN       31 /* Filename of executable */
#define AT_SYSINFO_EHDR 33 /* VDSO header */

/* ── 64-bit ELF Header ────────────────────────────────────────────────────── */
typedef struct {
    u32 e_ident_magic;
    u8  e_ident_class;
    u8  e_ident_data;
    u8  e_ident_version;
    u8  e_ident_osabi;
    u8  e_ident_abiversion;
    u8  e_ident_pad[7];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

/* ── 64-bit Program Header ────────────────────────────────────────────────── */
typedef struct {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
} __attribute__((packed)) elf64_phdr_t;

/* ── 64-bit Section Header ────────────────────────────────────────────────── */
typedef struct {
    u32 sh_name;
    u32 sh_type;
    u64 sh_flags;
    u64 sh_addr;
    u64 sh_offset;
    u64 sh_size;
    u32 sh_link;
    u32 sh_info;
    u64 sh_addralign;
    u64 sh_entsize;
} __attribute__((packed)) elf64_shdr_t;

/* ── 64-bit Dynamic Entry ─────────────────────────────────────────────────── */
typedef struct {
    s64 d_tag;
    union {
        u64 d_val;
        u64 d_ptr;
    } d_un;
} __attribute__((packed)) elf64_dyn_t;

/* ── 64-bit Auxiliary Vector Entry ────────────────────────────────────────── */
typedef struct {
    u64 a_type;
    union {
        u64 a_val;
        void *a_ptr;
    } a_un;
} __attribute__((packed)) elf64_auxv_t;

/**
 * elf_load_exec(proc, path, argv, envp, out_entry, out_space, out_rsp) —
 * Load an ELF binary or shebang script from VFS into process memory space with System V AMD64 stack.
 */
int elf_load_exec(process_t *proc, const char *path, const char *const argv[], const char *const envp[],
                  uintptr_t *out_entry, phys_addr_t *out_space, u64 *out_rsp);

/**
 * sched_spawn_user(path) — Create process and thread from ELF file path with default arguments.
 */
process_t *sched_spawn_user(const char *path);

/**
 * sched_spawn_user_args(path, argv, envp) — Create process and thread with custom arguments.
 */
process_t *sched_spawn_user_args(const char *path, const char *const argv[], const char *const envp[]);
