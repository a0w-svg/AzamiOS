/* ============================================================================
 * AzamiOS — ld-azami.so: minimal AzamiOS-native dynamic linker
 * File: userland/ldso/ldso.c
 *
 * Phase 5a of the libc hardening plan: prove the PT_INTERP/ET_DYN/auxv
 * mechanism end-to-end with a real (if deliberately minimal) loader —
 * eager relocation only, no dlopen()/dlsym()/dlclose() yet (that's a
 * follow-on milestone, not committed here).
 *
 * Deliberately not linked against libc.a at all: this file has no libc
 * dependency (only the raw syscallN() inline-asm trampolines from
 * userland/libc/include/sys/syscall.h — plain header content, not a link
 * dependency) and, more importantly, no self-relocation dependency either.
 * Every global here is `static` (never referenced from another object, so
 * GCC's -fpic codegen resolves it via direct %rip-relative addressing, not
 * a GOT slot that would itself need a relocation applied before it's safe
 * to read) and nothing here is an initialized pointer/absolute-address
 * constant (the one thing that *would* need an R_X86_64_RELATIVE fixup
 * applied to this file's own memory before it runs). That sidesteps the
 * classic "ld.so must bootstrap its own relocations using only code that
 * doesn't need relocating yet" problem entirely for this file, rather than
 * solving it with hand-written pre-relocation assembly — confirmed with
 * objdump -r showing zero relocations against this object's own sections
 * (see the Makefile rule's build-time check).
 *
 * The one piece of information this file needs that auxv doesn't hand it
 * directly — the *main executable's* own load bias (AT_BASE is the
 * interpreter's bias, i.e. this file's own; the main image's isn't in
 * auxv at all) — comes from scripts/user-pic.ld's guarantee that every
 * AzamiOS-native ET_DYN image links at design-time base 0 with its
 * program header table at file offset (and therefore vaddr) exactly 64,
 * the fixed size of an ELF64 header. AT_PHDR gives the *runtime* address
 * of that same table, so `AT_PHDR - 64` is the bias — no PT_PHDR segment
 * needed (this cross-toolchain's ld doesn't emit one for a script-less
 * -shared/-pie link either, confirmed empirically before writing this).
 * ============================================================================ */

#include <stdint.h>
#include "../libc/include/sys/syscall.h"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;
typedef int64_t  s64;

/* ── ELF64 structures ────────────────────────────────────────────────────
 * Field-for-field identical to kernel/sched/elf.h's elf64_ehdr_t/phdr_t/
 * dyn_t/auxv_t (same on-disk/in-memory format) plus the Sym/Rela shapes
 * that header doesn't need and therefore doesn't declare. Hand-duplicated
 * here rather than #include-d: this file can't pull in kernel headers
 * (different freestanding target, kernel-only dependencies) and
 * deliberately doesn't share anything with libc.a beyond the syscall
 * trampolines (see the top-of-file comment). */
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

typedef struct {
    s64 d_tag;
    union { u64 d_val; u64 d_ptr; } d_un;
} __attribute__((packed)) elf64_dyn_t;

typedef struct {
    u64 a_type;
    union { u64 a_val; void *a_ptr; } a_un;
} __attribute__((packed)) elf64_auxv_t;

typedef struct {
    u32 st_name;
    u8  st_info;
    u8  st_other;
    u16 st_shndx;
    u64 st_value;
    u64 st_size;
} __attribute__((packed)) elf64_sym_t;

typedef struct {
    u64 r_offset;
    u64 r_info;
    s64 r_addend;
} __attribute__((packed)) elf64_rela_t;

#define ELF64_R_SYM(i)  ((u32)((i) >> 32))
#define ELF64_R_TYPE(i) ((u32)(i))

#define R_X86_64_64        1
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8

#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_JMPREL   23

#define PT_LOAD    1
#define PT_DYNAMIC 2

#define AT_NULL  0
#define AT_PHDR  3
#define AT_PHNUM 5
#define AT_BASE  7
#define AT_ENTRY 9

#define ELF_MAGIC 0x464C457FU

/* ── Freestanding helpers (no libc) ─────────────────────────────────────── */

static u64 ld_strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }

static int ld_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void ld_memset(void *d, int c, u64 n)
{
    u8 *dd = (u8 *)d;
    while (n--) *dd++ = (u8)c;
}

static int  ld_open(const char *path)          { return (int)syscall2(SYS_open, (long)path, 0 /* O_RDONLY */); }
static long ld_read(int fd, void *buf, u64 n)   { return syscall3(SYS_read, fd, (long)buf, (long)n); }
static long ld_lseek(int fd, s64 off, int whence) { return syscall3(SYS_lseek, fd, off, whence); }
static void ld_close(int fd)                    { syscall1(SYS_close, fd); }
static void *ld_mmap_anon_rwx(u64 len)
{
    /* PROT_READ|PROT_WRITE|PROT_EXEC (0x7), MAP_PRIVATE|MAP_ANONYMOUS (0x22),
     * fd=-1, offset=0. RWX rather than per-segment permissions because every
     * PT_LOAD segment lands in one combined mapping here (see
     * load_dependency()'s comment) — a dependency's .text has to be
     * executable somewhere in that mapping, so the whole thing is. Real
     * per-segment R/W/X is exactly the kind of correctness dlopen()
     * support (the next milestone) would need to get right. */
    return (void *)syscall6(SYS_mmap, 0, (long)len, 0x7, 0x22, -1, 0);
}
static void ld_write2(const char *s)
{
    syscall3(SYS_write, 2, (long)s, (long)ld_strlen(s));
}

/* ── auxv ────────────────────────────────────────────────────────────────── */

static elf64_auxv_t *find_auxv(char **envp)
{
    char **p = envp;
    while (*p) p++;
    p++;
    return (elf64_auxv_t *)(void *)p;
}

static u64 auxv_get(elf64_auxv_t *av, u64 type)
{
    for (; av->a_type != AT_NULL; av++)
        if (av->a_type == type) return av->a_un.a_val;
    return 0;
}

/* ── One loaded object (the main executable, or one DT_NEEDED dependency) ── */

#define MAX_OBJS 8
typedef struct {
    u64          load_bias;
    elf64_sym_t *symtab;
    const char  *strtab;
    u32          nsyms;
} obj_t;

static obj_t g_objs[MAX_OBJS];
static int   g_nobjs = 0;

#define MAX_NEEDED 4
typedef struct {
    elf64_sym_t  *symtab;
    const char   *strtab;
    u32           nsyms;
    elf64_rela_t *rela;
    u64           relasz;
    elf64_rela_t *jmprel;
    u64           pltrelsz;
    const char   *needed[MAX_NEEDED];
    int           nneeded;
} dyn_info_t;

static void parse_dynamic(elf64_dyn_t *dyn, u64 bias, dyn_info_t *out)
{
    ld_memset(out, 0, sizeof(*out));

    /* DT_STRTAB first: DT_NEEDED entries are string-table *offsets*, so the
     * table has to be known before the second pass can resolve them. */
    for (elf64_dyn_t *d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_STRTAB) out->strtab = (const char *)(bias + d->d_un.d_ptr);
    }

    for (elf64_dyn_t *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB:   out->symtab = (elf64_sym_t *)(bias + d->d_un.d_ptr); break;
        case DT_RELA:     out->rela = (elf64_rela_t *)(bias + d->d_un.d_ptr); break;
        case DT_RELASZ:   out->relasz = d->d_un.d_val; break;
        case DT_JMPREL:   out->jmprel = (elf64_rela_t *)(bias + d->d_un.d_ptr); break;
        case DT_PLTRELSZ: out->pltrelsz = d->d_un.d_val; break;
        case DT_HASH: {
            /* SysV .hash layout: { nbucket, nchain, bucket[nbucket], chain[nchain] }.
             * nchain == the dynamic symbol table's entry count — the only
             * piece of information this loader actually needs .hash for
             * (it walks .dynsym linearly rather than hashing, which is
             * fine at this scale). */
            u32 *hash = (u32 *)(bias + d->d_un.d_ptr);
            out->nsyms = hash[1];
            break;
        }
        case DT_NEEDED:
            if (out->nneeded < MAX_NEEDED && out->strtab)
                out->needed[out->nneeded++] = out->strtab + d->d_un.d_val;
            break;
        default: break;
        }
    }
}

static elf64_dyn_t *find_dynamic(elf64_phdr_t *phdrs, u32 phnum, u64 bias)
{
    for (u32 i = 0; i < phnum; i++)
        if (phdrs[i].p_type == PT_DYNAMIC) return (elf64_dyn_t *)(bias + phdrs[i].p_vaddr);
    return 0;
}

/* Opens, one-shot-maps, and dynamic-parses a DT_NEEDED dependency by name.
 * Not a general loader: every PT_LOAD segment is copied into one
 * contiguous anonymous RW mapping sized to the highest vaddr+memsz,
 * instead of mapping each segment separately with its own permissions —
 * simpler, and sufficient to prove the mechanism for this milestone (a
 * general loader honoring per-segment R/W/X is exactly the kind of thing
 * dlopen() support, the next milestone, would need to get right). */
static int load_dependency(const char *name, dyn_info_t *out_info, u64 *out_bias)
{
    char path[128];
    const char *prefix = "/lib/";
    u64 i = 0;
    while (prefix[i]) { path[i] = prefix[i]; i++; }
    u64 j = 0;
    while (name[j] && i < sizeof(path) - 1) path[i++] = name[j++];
    path[i] = '\0';

    int fd = ld_open(path);
    if (fd < 0) { ld_write2("ld-azami.so: cannot open dependency\n"); return -1; }

    elf64_ehdr_t ehdr;
    if (ld_read(fd, &ehdr, sizeof(ehdr)) != (long)sizeof(ehdr) || ehdr.e_ident_magic != ELF_MAGIC) {
        ld_close(fd);
        return -1;
    }

    #define LD_MAX_PHDRS 16
    elf64_phdr_t phdrs[LD_MAX_PHDRS];
    u16 nphdrs = (ehdr.e_phnum < LD_MAX_PHDRS) ? ehdr.e_phnum : LD_MAX_PHDRS;
    ld_lseek(fd, (s64)ehdr.e_phoff, 0 /* SEEK_SET */);
    if (ld_read(fd, phdrs, (u64)nphdrs * sizeof(elf64_phdr_t)) != (long)((u64)nphdrs * sizeof(elf64_phdr_t))) {
        ld_close(fd);
        return -1;
    }

    u64 max_end = 0;
    for (u16 p = 0; p < nphdrs; p++) {
        if (phdrs[p].p_type != PT_LOAD) continue;
        u64 end = phdrs[p].p_vaddr + phdrs[p].p_memsz;
        if (end > max_end) max_end = end;
    }
    u64 map_len = (max_end + 0xFFF) & ~0xFFFULL;
    if (map_len == 0) { ld_close(fd); return -1; }

    void *base = ld_mmap_anon_rwx(map_len);
    if ((s64)(uintptr_t)base < 0 || !base) { ld_close(fd); return -1; }
    ld_memset(base, 0, map_len);

    for (u16 p = 0; p < nphdrs; p++) {
        if (phdrs[p].p_type != PT_LOAD || phdrs[p].p_filesz == 0) continue;
        ld_lseek(fd, (s64)phdrs[p].p_offset, 0);
        ld_read(fd, (u8 *)base + phdrs[p].p_vaddr, phdrs[p].p_filesz);
    }
    ld_close(fd);

    u64 bias = (u64)(uintptr_t)base;
    elf64_dyn_t *dyn = find_dynamic(phdrs, nphdrs, bias);
    if (dyn) parse_dynamic(dyn, bias, out_info);
    else     ld_memset(out_info, 0, sizeof(*out_info));

    *out_bias = bias;
    return 0;
}

/* ── Symbol resolution ───────────────────────────────────────────────────── */

static u64 resolve_symbol(const char *name)
{
    for (int i = 0; i < g_nobjs; i++) {
        obj_t *o = &g_objs[i];
        if (!o->symtab || !o->strtab) continue;
        for (u32 s = 0; s < o->nsyms; s++) {
            elf64_sym_t *sym = &o->symtab[s];
            if (sym->st_name == 0 || sym->st_shndx == 0) continue; /* null entry / undefined ref */
            if (ld_strcmp(o->strtab + sym->st_name, name) == 0)
                return o->load_bias + sym->st_value;
        }
    }
    return 0;
}

/* ── Relocation ──────────────────────────────────────────────────────────── */

static void apply_relative(dyn_info_t *info, u64 bias)
{
    if (!info->rela) return;
    u64 n = info->relasz / sizeof(elf64_rela_t);
    for (u64 i = 0; i < n; i++) {
        elf64_rela_t *r = &info->rela[i];
        if (ELF64_R_TYPE(r->r_info) == R_X86_64_RELATIVE) {
            *(u64 *)(bias + r->r_offset) = bias + (u64)r->r_addend;
        }
    }
}

static void apply_symbolic_table(elf64_rela_t *tbl, u64 count, dyn_info_t *info, u64 bias)
{
    for (u64 i = 0; i < count; i++) {
        elf64_rela_t *r = &tbl[i];
        u32 type = ELF64_R_TYPE(r->r_info);
        if (type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT && type != R_X86_64_64)
            continue;
        if (!info->symtab || !info->strtab) continue;

        u32 symidx = ELF64_R_SYM(r->r_info);
        elf64_sym_t *sym = &info->symtab[symidx];
        const char *name = info->strtab + sym->st_name;

        u64 resolved = resolve_symbol(name);
        if (!resolved) {
            ld_write2("ld-azami.so: unresolved symbol\n");
            continue; /* leave the slot as-is rather than crash the linker itself */
        }
        u64 addend = (type == R_X86_64_64) ? (u64)r->r_addend : 0;
        *(u64 *)(bias + r->r_offset) = resolved + addend;
    }
}

static void apply_symbolic(dyn_info_t *info, u64 bias)
{
    if (info->rela)   apply_symbolic_table(info->rela, info->relasz / sizeof(elf64_rela_t), info, bias);
    if (info->jmprel) apply_symbolic_table(info->jmprel, info->pltrelsz / sizeof(elf64_rela_t), info, bias);
}

/* ── Handoff ─────────────────────────────────────────────────────────────── */

/* Restores RSP to exactly the value crt0.asm captured right after aligning
 * it (argc/argv/envp/auxv are all still sitting there, untouched — this
 * file only ever read that region) and jumps to the main executable's real
 * entry point, so its own crt0 sees exactly what it would have seen had
 * the kernel jumped there directly with no interpreter involved. */
static void jump_to_entry(u64 entry, u64 initial_rsp) __attribute__((noreturn));
static void jump_to_entry(u64 entry, u64 initial_rsp)
{
    __asm__ volatile(
        "mov %0, %%rsp\n\t"
        "jmp *%1\n\t"
        :
        : "r"(initial_rsp), "r"(entry)
        : "memory"
    );
    __builtin_unreachable();
}

long ld_main(long argc, char **argv, char **envp, u64 initial_rsp)
{
    (void)argc; (void)argv;

    elf64_auxv_t *av = find_auxv(envp);
    u64 at_phdr  = auxv_get(av, AT_PHDR);
    u64 at_phnum = auxv_get(av, AT_PHNUM);
    u64 at_entry = auxv_get(av, AT_ENTRY);

    if (!at_phdr || !at_entry) {
        ld_write2("ld-azami.so: auxv missing AT_PHDR/AT_ENTRY\n");
        return 1;
    }

    /* See this file's top comment: guaranteed by scripts/user-pic.ld. */
    u64 main_bias = at_phdr - 64;
    elf64_phdr_t *main_phdrs = (elf64_phdr_t *)(uintptr_t)at_phdr;

    elf64_dyn_t *main_dyn = find_dynamic(main_phdrs, (u32)at_phnum, main_bias);
    if (!main_dyn) {
        /* No PT_DYNAMIC in the main image: nothing to relocate or link
         * against. Jump straight to its entry point. */
        jump_to_entry(at_entry, initial_rsp);
    }

    dyn_info_t main_info;
    parse_dynamic(main_dyn, main_bias, &main_info);

    g_nobjs = 0;
    g_objs[g_nobjs].load_bias = main_bias;
    g_objs[g_nobjs].symtab    = main_info.symtab;
    g_objs[g_nobjs].strtab    = main_info.strtab;
    g_objs[g_nobjs].nsyms     = main_info.nsyms;
    g_nobjs++;

    dyn_info_t dep_info[MAX_NEEDED];
    int ndeps = 0;
    for (int i = 0; i < main_info.nneeded && g_nobjs < MAX_OBJS && ndeps < MAX_NEEDED; i++) {
        u64 dep_bias = 0;
        if (load_dependency(main_info.needed[i], &dep_info[ndeps], &dep_bias) == 0) {
            g_objs[g_nobjs].load_bias = dep_bias;
            g_objs[g_nobjs].symtab    = dep_info[ndeps].symtab;
            g_objs[g_nobjs].strtab    = dep_info[ndeps].strtab;
            g_objs[g_nobjs].nsyms     = dep_info[ndeps].nsyms;
            g_nobjs++;
            ndeps++;
        }
    }

    /* RELATIVE first (self-contained, no symbol lookup) for every object,
     * then GLOB_DAT/JUMP_SLOT/64 (need every object's symbol table known)
     * — so a dependency's own self-relocations are done before the main
     * image's symbolic relocations might read through it. */
    apply_relative(&main_info, main_bias);
    for (int i = 0; i < ndeps; i++) apply_relative(&dep_info[i], g_objs[1 + i].load_bias);

    apply_symbolic(&main_info, main_bias);
    for (int i = 0; i < ndeps; i++) apply_symbolic(&dep_info[i], g_objs[1 + i].load_bias);

    jump_to_entry(at_entry, initial_rsp);
}
