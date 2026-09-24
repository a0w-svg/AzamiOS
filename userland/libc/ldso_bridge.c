/* ============================================================================
 * AzamiOS Userspace — bridge to ld-azami.so's runtime exports
 * File: userland/libc/ldso_bridge.c
 *
 * dlopen()/dlsym()/dlclose()/dlerror() (userland/libc/dlfcn.c) and TLS setup
 * for a pthread_create()'d thread when running under ld-azami.so
 * (userland/libc/tls.c's __init_thread_tls()) both need to call back into
 * functions the dynamic linker exports (see userland/ldso/ldso.c's "Public
 * runtime ABI" section for exactly what it exports and why). Neither the
 * main executable nor libc.a/libc.so is ever linked against ld-azami.so as
 * a normal DT_NEEDED dependency — it's the interpreter, not a library the
 * main image depends on — so those calls can't go through an ordinary
 * compiler-generated PLT/GOT relocation. Instead, this file finds
 * ld-azami.so's own loaded image (from AT_BASE, the one auxv entry that
 * names it) and looks the requested symbol up in its .dynsym by hand,
 * exactly the way ld-azami.so itself looks a symbol up in any other
 * object — then the caller casts the returned address to the right
 * function-pointer type and calls it directly.
 *
 * AT_BASE is 0 whenever this process has no PT_INTERP (a purely static
 * binary): every lookup here then correctly, silently fails, and callers
 * (dlopen() returning NULL, __init_tls() falling back to its own
 * single-module PT_TLS scan) already treat "no ld.so" as an ordinary,
 * expected case rather than an error.
 * ============================================================================ */
#include <stdint.h>

#define AT_NULL 0
#define AT_BASE 7

#define PT_DYNAMIC 2

#define DT_NULL   0
#define DT_HASH   4
#define DT_STRTAB 5
#define DT_SYMTAB 6

typedef struct {
    uint64_t a_type;
    uint64_t a_val;
} elf64_auxv_t;

/* Only the fields needed to reach e_phoff/e_phnum — this process's own
 * crt0/tls.c already assume ELFCLASS64/little-endian/x86_64, so there's
 * nothing to validate here that isn't already true by construction. */
typedef struct {
    uint8_t  e_ident[16];
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
} __attribute__((packed)) elf64_ehdr_min_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;

typedef struct {
    int64_t  d_tag;
    uint64_t d_un; /* d_val/d_ptr union — same bit layout either way */
} __attribute__((packed)) elf64_dyn_t;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} __attribute__((packed)) elf64_sym_t;

#define BRIDGE_MAX_PHDRS 16

static elf64_auxv_t *g_auxv;

/* Called once, early, from tls.c's __init_tls() (which has already found
 * the auxv pointer for its own purposes) — before __libc_init() or main(),
 * so every later dlopen()/dlsym()/dlclose() call already has this set. */
void __libc_stash_auxv(void *auxv)
{
    g_auxv = (elf64_auxv_t *)auxv;
}

static uint64_t auxv_get(uint64_t type)
{
    if (!g_auxv) return 0;
    for (elf64_auxv_t *a = g_auxv; a->a_type != AT_NULL; a++)
        if (a->a_type == type) return a->a_val;
    return 0;
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void *__libc_ldso_lookup(const char *name)
{
    uint64_t at_base = auxv_get(AT_BASE);
    if (!at_base) return (void *)0; /* no PT_INTERP: statically linked, no ld.so present */

    elf64_ehdr_min_t *ehdr = (elf64_ehdr_min_t *)(uintptr_t)at_base;
    uint16_t phnum = ehdr->e_phnum;
    if (phnum > BRIDGE_MAX_PHDRS) phnum = BRIDGE_MAX_PHDRS;
    elf64_phdr_t *phdrs = (elf64_phdr_t *)(uintptr_t)(at_base + ehdr->e_phoff);

    elf64_dyn_t *dyn = (elf64_dyn_t *)(uintptr_t)0;
    for (uint16_t i = 0; i < phnum; i++) {
        if (phdrs[i].p_type == (uint32_t)PT_DYNAMIC) {
            dyn = (elf64_dyn_t *)(uintptr_t)(at_base + phdrs[i].p_vaddr);
            break;
        }
    }
    if (!dyn) return (void *)0;

    const char   *strtab = 0;
    elf64_sym_t  *symtab = 0;
    uint32_t      nsyms  = 0;
    for (elf64_dyn_t *d = dyn; d->d_tag != DT_NULL; d++)
        if (d->d_tag == DT_STRTAB) strtab = (const char *)(uintptr_t)(at_base + d->d_un);
    for (elf64_dyn_t *d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_SYMTAB) {
            symtab = (elf64_sym_t *)(uintptr_t)(at_base + d->d_un);
        } else if (d->d_tag == DT_HASH) {
            uint32_t *hash = (uint32_t *)(uintptr_t)(at_base + d->d_un);
            nsyms = hash[1];
        }
    }
    if (!strtab || !symtab) return (void *)0;

    for (uint32_t s = 0; s < nsyms; s++) {
        elf64_sym_t *sym = &symtab[s];
        if (sym->st_name == 0 || sym->st_shndx == 0) continue;
        if (str_eq(strtab + sym->st_name, name))
            return (void *)(uintptr_t)(at_base + sym->st_value);
    }
    return (void *)0;
}
