/* ============================================================================
 * AzamiOS — 64-bit ELF Executable Loader Header
 * File: kernel/sched/elf.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "sched.h"

#define ELF_MAGIC 0x464C457F /* "\x7FELF" in little-endian */
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define EM_X86_64   62
#define PT_LOAD     1

#define PF_X        1
#define PF_W        2
#define PF_R        4

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

/**
 * elf_load_exec(proc, path, argv, envp, out_entry, out_space, out_rsp) —
 * Load an ELF binary from VFS into process memory space with System V AMD64 stack.
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
