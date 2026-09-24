/* ============================================================================
 * AzamiOS — ld-azami.so: AzamiOS-native dynamic linker
 * File: userland/ldso/ldso.c
 *
 * Started as the Phase 5a milestone (prove the PT_INTERP/ET_DYN/auxv
 * mechanism end-to-end with eager relocation only). This is the follow-on
 * pass that makes it a real loader:
 *   - every PT_LOAD segment gets its own real R/W/X mapping (mmap the whole
 *     span PROT_NONE, then mmap MAP_FIXED + mprotect each segment to its
 *     actual p_flags), not one combined RWX blob;
 *   - DT_NEEDED is followed transitively (a dependency's own dependencies
 *     are loaded too), with name-based dedup so a diamond dependency isn't
 *     mapped twice;
 *   - combined static TLS: every object present at process start (main +
 *     everything eagerly pulled in by DT_NEEDED) that has a PT_TLS segment
 *     is folded into one variant-II static TLS block, %fs is pointed at it
 *     directly by this file (not by userland/libc/tls.c — see that file's
 *     updated __init_tls(), which detects this and steps aside), and
 *     R_X86_64_TPOFF64 relocations are applied against it. General-dynamic
 *     model TLS (R_X86_64_DTPMOD64/DTPOFF64, which would need a runtime
 *     __tls_get_addr()) is deliberately not supported — every AzamiOS
 *     object is expected to be built with -ftls-model=initial-exec;
 *   - DT_INIT / DT_INIT_ARRAY constructors run, dependency-first;
 *   - unresolved *required* (non-weak) symbols abort the process at
 *     startup instead of silently leaving a null pointer live;
 *   - __ldso_dlopen/__ldso_dlsym/__ldso_dlclose/__ldso_dlerror are real,
 *     usable after startup — see userland/libc/ldso_bridge.c and
 *     userland/libc/dlfcn.c for how libc's dlopen()/dlsym()/dlclose()
 *     reach them (a hand-resolved function-pointer lookup against this
 *     object's own .dynsym, not a normal DT_NEEDED link — see those files).
 *   - __ldso_tls_nmodules/__ldso_tls_module expose the combined static TLS
 *     layout this file computed, so a thread later created by
 *     pthread_create() can replicate every module, not just the main
 *     executable's own.
 *
 * Known, deliberate limitations (documented rather than silently wrong):
 *   - symbol resolution is a linear scan over every loaded object's .dynsym
 *     (fine at this scale; a real .hash/.gnu.hash walk is a perf-only
 *     improvement this file doesn't need yet);
 *   - dlopen() of an object that itself has a PT_TLS segment fails cleanly
 *     (dlerror() explains why) rather than corrupting the fixed-size static
 *     TLS block computed at process start — the classic "surplus static
 *     TLS" limits every minimal loader has;
 *   - dlclose() decrements only the target object's own refcount, not its
 *     dependencies' (a bounded leak, not a dangling pointer);
 *   - this file is not thread-safe against concurrent dlopen()/dlclose()
 *     from two threads — no lock here, none in the libc wrappers either;
 *   - DT_FINI/DT_FINI_ARRAY (shared-library destructors) are not run; a
 *     process exit() just tears the whole address space down.
 *
 * Still deliberately not linked against libc.a (see the original design
 * note this replaces): every global here is `static` except the small,
 * intentional set of functions exported for libc to call back into (which
 * are never called *from* this file's own static code, so they introduce
 * no self-relocation requirement — see those functions' comments), and
 * nothing here is a statically-initialized absolute-pointer constant.
 * That sidesteps "ld.so must bootstrap its own relocations using code that
 * doesn't need relocating yet" without hand-written pre-relocation asm —
 * the kernel never applies relocations to the interpreter image itself, and
 * nothing here does either.
 *
 * The one piece of information this file needs that auxv doesn't hand it
 * directly — the *main executable's* own load bias (AT_BASE is the
 * interpreter's own bias, i.e. this file's; the main image's isn't in auxv
 * at all) — comes from scripts/user-pic.ld's guarantee that every
 * AzamiOS-native ET_DYN image links at design-time base 0 with its program
 * header table at file offset (and therefore vaddr) exactly 64, the fixed
 * size of an ELF64 header. AT_PHDR gives the *runtime* address of that same
 * table, so `AT_PHDR - 64` is the bias — no PT_PHDR segment needed (this
 * cross-toolchain's ld doesn't emit one for a script-less -shared/-pie link
 * either, confirmed empirically before this was written).
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
#define ELF64_ST_BIND(i) ((i) >> 4)

#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2

#define R_X86_64_64        1
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8
#define R_X86_64_TPOFF64   18

#define DT_NULL          0
#define DT_NEEDED        1
#define DT_PLTRELSZ      2
#define DT_HASH          4
#define DT_STRTAB        5
#define DT_SYMTAB        6
#define DT_RELA          7
#define DT_RELASZ        8
#define DT_INIT          12
#define DT_FINI          13
#define DT_JMPREL        23
#define DT_INIT_ARRAY    25
#define DT_FINI_ARRAY    26
#define DT_INIT_ARRAYSZ  27
#define DT_FINI_ARRAYSZ  28

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_TLS     7

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

#define AT_NULL  0
#define AT_PHDR  3
#define AT_PHNUM 5
#define AT_BASE  7
#define AT_ENTRY 9

#define ELF_MAGIC   0x464C457FU
#define ELFCLASS64  2

#define PAGE_SIZE 0x1000ULL

#ifndef ARCH_SET_FS
#define ARCH_SET_FS 0x1002
#endif

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

static void ld_memcpy(void *d, const void *s, u64 n)
{
    u8 *dd = (u8 *)d; const u8 *ss = (const u8 *)s;
    while (n--) *dd++ = *ss++;
}

static u64 align_down(u64 v, u64 a) { return v & ~(a - 1); }
static u64 align_up(u64 v, u64 a)   { return (v + a - 1) & ~(a - 1); }

static int  ld_open(const char *path)            { return (int)syscall2(SYS_open, (long)path, 0 /* O_RDONLY */); }
static long ld_read(int fd, void *buf, u64 n)     { return syscall3(SYS_read, fd, (long)buf, (long)n); }
static long ld_lseek(int fd, s64 off, int whence) { return syscall3(SYS_lseek, fd, off, whence); }
static void ld_close(int fd)                      { syscall1(SYS_close, fd); }
static void ld_write2(const char *s)              { syscall3(SYS_write, 2, (long)s, (long)ld_strlen(s)); }

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

/* ── One loaded object (the main executable, or a DT_NEEDED dependency,
 * eager or dlopen()'d) ──────────────────────────────────────────────────── */

#define MAX_OBJS       64
#define LD_MAX_PHDRS   16
#define MAX_NEEDED     16
#define MAX_TLS_MODULES 16

typedef struct {
    int  in_use;
    int  pinned;    /* loaded before the first dlopen() call: dlclose() may
                      * never actually unmap it, no matter what its
                      * refcount does (see __ldso_dlclose()). */
    int  refcount;
    int  ctors_ran;
    char name[64];  /* the DT_NEEDED / dlopen() name this was found under —
                      * used only for dedup, not a filesystem path. */

    u64   load_bias;
    void *map_base;  /* NULL for the main image (kernel-owned; never munmap
                       * it) and, degenerately, for an object dlopen() failed
                       * to map — real objects always have a non-NULL base. */
    u64   map_len;

    elf64_phdr_t phdr_storage[LD_MAX_PHDRS];
    u32          phnum;

    elf64_sym_t *symtab;
    const char  *strtab;
    u32          nsyms;

    elf64_rela_t *rela;
    u64           rela_count;
    elf64_rela_t *jmprel;
    u64           jmprel_count;

    u64  dt_init;
    u64 *dt_init_array;
    u64  dt_init_arraysz;

    const char *needed_names[MAX_NEEDED];
    int         nneeded;
    int         dep_idx[MAX_NEEDED];
    int         ndeps;

    int has_tls;
    u64 tls_src;     /* runtime absolute address of this object's own .tdata */
    u64 tls_filesz;
    u64 tls_memsz;
    u64 tls_align;
    s64 tls_tpoff;   /* filled by setup_static_tls(); valid only if has_tls */
} obj_t;

static obj_t g_objs[MAX_OBJS];

static int obj_alloc(void)
{
    for (int i = 0; i < MAX_OBJS; i++)
        if (!g_objs[i].in_use) return i;
    return -1;
}

static int find_loaded_by_name(const char *name)
{
    for (int i = 0; i < MAX_OBJS; i++)
        if (g_objs[i].in_use && g_objs[i].name[0] && ld_strcmp(g_objs[i].name, name) == 0)
            return i;
    return -1;
}

static void obj_force_unmap(int idx)
{
    obj_t *o = &g_objs[idx];
    if (o->map_base) syscall2(SYS_munmap, (long)(intptr_t)o->map_base, (long)o->map_len);
    ld_memset(o, 0, sizeof(*o));
}

/* ── PT_DYNAMIC / PT_TLS parsing ─────────────────────────────────────────── */

static elf64_dyn_t *find_dynamic_in(elf64_phdr_t *phdrs, u32 phnum, u64 bias)
{
    for (u32 i = 0; i < phnum; i++)
        if (phdrs[i].p_type == PT_DYNAMIC) return (elf64_dyn_t *)(uintptr_t)(bias + phdrs[i].p_vaddr);
    return 0;
}

static void find_tls_in(obj_t *o, elf64_phdr_t *phdrs, u32 phnum)
{
    for (u32 i = 0; i < phnum; i++) {
        if (phdrs[i].p_type != PT_TLS || phdrs[i].p_memsz == 0) continue;
        o->has_tls    = 1;
        o->tls_src    = o->load_bias + phdrs[i].p_vaddr;
        o->tls_filesz = phdrs[i].p_filesz;
        o->tls_memsz  = phdrs[i].p_memsz;
        o->tls_align  = phdrs[i].p_align ? phdrs[i].p_align : 8;
        if (o->tls_align < 8) o->tls_align = 8;
        return; /* ELF permits at most one PT_TLS per object */
    }
}

static void parse_dynamic(obj_t *o, elf64_dyn_t *dyn)
{
    u64 bias = o->load_bias;
    const char *strtab = 0;

    /* DT_STRTAB first: DT_NEEDED entries are string-table *offsets*. */
    for (elf64_dyn_t *d = dyn; d->d_tag != DT_NULL; d++)
        if (d->d_tag == DT_STRTAB) strtab = (const char *)(uintptr_t)(bias + d->d_un.d_ptr);
    o->strtab = strtab;

    for (elf64_dyn_t *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB:       o->symtab = (elf64_sym_t *)(uintptr_t)(bias + d->d_un.d_ptr); break;
        case DT_RELA:          o->rela = (elf64_rela_t *)(uintptr_t)(bias + d->d_un.d_ptr); break;
        case DT_RELASZ:        o->rela_count = d->d_un.d_val / sizeof(elf64_rela_t); break;
        case DT_JMPREL:         o->jmprel = (elf64_rela_t *)(uintptr_t)(bias + d->d_un.d_ptr); break;
        case DT_PLTRELSZ:       o->jmprel_count = d->d_un.d_val / sizeof(elf64_rela_t); break;
        case DT_INIT:           o->dt_init = bias + d->d_un.d_val; break;
        case DT_INIT_ARRAY:     o->dt_init_array = (u64 *)(uintptr_t)(bias + d->d_un.d_ptr); break;
        case DT_INIT_ARRAYSZ:   o->dt_init_arraysz = d->d_un.d_val; break;
        case DT_HASH: {
            /* SysV .hash layout: { nbucket, nchain, ... }. nchain is the
             * dynamic symbol table's entry count — the only thing this
             * loader needs .hash for (see the top-of-file note on why a
             * real hash walk isn't implemented). */
            u32 *hash = (u32 *)(uintptr_t)(bias + d->d_un.d_ptr);
            o->nsyms = hash[1];
            break;
        }
        case DT_NEEDED:
            if (o->nneeded < MAX_NEEDED && strtab)
                o->needed_names[o->nneeded++] = strtab + d->d_un.d_val;
            break;
        default: break;
        }
    }
}

/* ── Loading a single object (real per-segment R/W/X, not one RWX blob) ───── */

static int obj_load_file(const char *path, const char *name_for_dedup)
{
    int existing = find_loaded_by_name(name_for_dedup);
    if (existing >= 0) { g_objs[existing].refcount++; return existing; }

    int idx = obj_alloc();
    if (idx < 0) { ld_write2("ld-azami.so: too many loaded objects\n"); return -1; }
    obj_t *o = &g_objs[idx];
    ld_memset(o, 0, sizeof(*o));

    int fd = ld_open(path);
    if (fd < 0) return -1;

    elf64_ehdr_t ehdr;
    if (ld_read(fd, &ehdr, sizeof(ehdr)) != (long)sizeof(ehdr) ||
        ehdr.e_ident_magic != ELF_MAGIC || ehdr.e_ident_class != ELFCLASS64) {
        ld_close(fd);
        return -1;
    }

    u16 nphdrs = (ehdr.e_phnum < LD_MAX_PHDRS) ? ehdr.e_phnum : LD_MAX_PHDRS;
    ld_lseek(fd, (s64)ehdr.e_phoff, 0 /* SEEK_SET */);
    if (ld_read(fd, o->phdr_storage, (u64)nphdrs * sizeof(elf64_phdr_t)) !=
        (long)((u64)nphdrs * sizeof(elf64_phdr_t))) {
        ld_close(fd);
        return -1;
    }
    o->phnum = nphdrs;

    u64 max_end = 0;
    for (u16 p = 0; p < nphdrs; p++) {
        if (o->phdr_storage[p].p_type != PT_LOAD) continue;
        u64 end = align_up(o->phdr_storage[p].p_vaddr + o->phdr_storage[p].p_memsz, PAGE_SIZE);
        if (end > max_end) max_end = end;
    }
    if (max_end == 0) { ld_close(fd); return -1; }

    /* Reserve the whole span at once (design-time vaddr 0 base, per every
     * AzamiOS-native ET_DYN image's guarantee — see the top-of-file note),
     * then punch real per-segment mappings into it below. Mirrors what the
     * kernel's own load_elf_segments() does for the main image
     * (kernel/sched/elf.c) instead of one combined RWX mapping. */
    void *base = (void *)syscall6(SYS_mmap, 0, (long)max_end, 0 /* PROT_NONE */,
                                   0x22 /* MAP_PRIVATE|MAP_ANONYMOUS */, -1, 0);
    if ((s64)(intptr_t)base < 0 || !base) { ld_close(fd); return -1; }

    for (u16 p = 0; p < nphdrs; p++) {
        elf64_phdr_t *ph = &o->phdr_storage[p];
        if (ph->p_type != PT_LOAD) continue;

        u64 seg_start = align_down(ph->p_vaddr, PAGE_SIZE);
        u64 seg_end   = align_up(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);
        void *seg_addr = (u8 *)base + seg_start;
        u64   seg_len  = seg_end - seg_start;

        long mret = syscall6(SYS_mmap, (long)(intptr_t)seg_addr, (long)seg_len,
                              0x3 /* PROT_READ|PROT_WRITE — writable for the copy below;
                                   * narrowed to the real permissions after */,
                              0x22 | 0x10 /* MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED */, -1, 0);
        if (mret < 0) {
            syscall2(SYS_munmap, (long)(intptr_t)base, (long)max_end);
            ld_close(fd);
            return -1;
        }

        if (ph->p_filesz) {
            ld_lseek(fd, (s64)ph->p_offset, 0);
            ld_read(fd, (u8 *)base + ph->p_vaddr, ph->p_filesz);
        }
        /* .bss tail is already zero: fresh anonymous pages. */

        int prot = 0;
        if (ph->p_flags & PF_R) prot |= 0x1;
        if (ph->p_flags & PF_W) prot |= 0x2;
        if (ph->p_flags & PF_X) prot |= 0x4;
        syscall3(SYS_mprotect, (long)(intptr_t)seg_addr, (long)seg_len, prot);
    }
    ld_close(fd);

    o->load_bias = (u64)(uintptr_t)base;
    o->map_base  = base;
    o->map_len   = max_end;
    o->in_use    = 1;
    o->refcount  = 1;
    {
        u64 n = 0;
        while (name_for_dedup[n] && n < sizeof(o->name) - 1) { o->name[n] = name_for_dedup[n]; n++; }
        o->name[n] = '\0';
    }

    find_tls_in(o, o->phdr_storage, o->phnum);
    elf64_dyn_t *dyn = find_dynamic_in(o->phdr_storage, o->phnum, o->load_bias);
    if (dyn) parse_dynamic(o, dyn);

    return idx;
}

/* Builds @path for a DT_NEEDED/dlopen() name: absolute names are used as-is,
 * anything else is searched under /lib/ — the one location the build's
 * install layout ever places a shared object at. */
static void build_lib_path(char *out, u64 out_len, const char *name)
{
    const char *prefix = (name[0] == '/') ? "" : "/lib/";
    u64 k = 0;
    while (prefix[k] && k < out_len - 1) { out[k] = prefix[k]; k++; }
    u64 j = 0;
    while (name[j] && k < out_len - 1) out[k++] = name[j++];
    out[k] = '\0';
}

/* Loads every not-yet-loaded DT_NEEDED dependency of g_objs[idx], and theirs
 * in turn — transitive, with the name-based dedup in obj_load_file()/
 * find_loaded_by_name() making a diamond dependency graph and cycles safe. */
static int load_needed_recursive(int idx)
{
    obj_t *o = &g_objs[idx];
    int nneeded = o->nneeded;
    const char *names[MAX_NEEDED];
    for (int i = 0; i < nneeded; i++) names[i] = o->needed_names[i];

    for (int i = 0; i < nneeded; i++) {
        int already = find_loaded_by_name(names[i]);
        int dep_idx;
        if (already >= 0) {
            g_objs[already].refcount++;
            dep_idx = already;
        } else {
            char path[128];
            build_lib_path(path, sizeof(path), names[i]);
            dep_idx = obj_load_file(path, names[i]);
            if (dep_idx < 0) {
                ld_write2("ld-azami.so: cannot load dependency '");
                ld_write2(names[i]);
                ld_write2("'\n");
                return -1;
            }
            if (load_needed_recursive(dep_idx) < 0) return -1;
        }

        if (o->ndeps < MAX_NEEDED) o->dep_idx[o->ndeps++] = dep_idx;
    }
    return 0;
}

/* ── Combined static TLS (main + everything eager) ──────────────────────── */

typedef struct {
    u64 vaddr, filesz, memsz, align;
    s64 tpoff;
} tls_mod_t;

static struct {
    u32       nmodules;
    tls_mod_t mods[MAX_TLS_MODULES];
} g_tls_info;

/* Folds every loaded object's PT_TLS into one variant-II static TLS block
 * and points %fs at it — see userland/libc/tls.c's __init_tls(), which
 * detects that %fs is already non-zero on entry and steps aside rather than
 * building its own single-module block on top of this one. A no-op (leaves
 * %fs at 0, exactly like a static binary with no __thread variables at all)
 * if nothing loaded has a PT_TLS segment. */
static void setup_static_tls(void)
{
    obj_t *tls_objs[MAX_TLS_MODULES];
    int n = 0;
    for (int i = 0; i < MAX_OBJS && n < MAX_TLS_MODULES; i++)
        if (g_objs[i].in_use && g_objs[i].has_tls) tls_objs[n++] = &g_objs[i];
    if (n == 0) return;

    u64 total = 0;
    for (int i = 0; i < n; i++)
        total = align_up(total, tls_objs[i]->tls_align) + align_up(tls_objs[i]->tls_memsz, tls_objs[i]->tls_align);
    total = align_up(total, 16);

    u64 block_len = align_up(total + sizeof(void *), PAGE_SIZE);
    void *block = (void *)syscall6(SYS_mmap, 0, (long)block_len, 0x3 /* R|W */,
                                    0x22 /* PRIVATE|ANON */, -1, 0);
    if ((s64)(intptr_t)block < 0 || !block) {
        ld_write2("ld-azami.so: fatal: cannot allocate static TLS block\n");
        syscall1(SYS_exit, 127);
    }

    void *tp = (u8 *)block + total;

    u64 used = 0;
    for (int i = 0; i < n; i++) {
        obj_t *m = tls_objs[i];
        used = align_up(used, m->tls_align);
        u8 *dst = (u8 *)block + used;
        ld_memcpy(dst, (const void *)(uintptr_t)m->tls_src, m->tls_filesz);
        ld_memset(dst + m->tls_filesz, 0, m->tls_memsz - m->tls_filesz);

        m->tls_tpoff = (s64)(uintptr_t)dst - (s64)(uintptr_t)tp;

        g_tls_info.mods[i].vaddr  = m->tls_src;
        g_tls_info.mods[i].filesz = m->tls_filesz;
        g_tls_info.mods[i].memsz  = m->tls_memsz;
        g_tls_info.mods[i].align  = m->tls_align;
        g_tls_info.mods[i].tpoff  = m->tls_tpoff;

        used += align_up(m->tls_memsz, m->tls_align);
    }
    g_tls_info.nmodules = (u32)n;

    *(void **)tp = tp; /* variant II: %fs:0 holds a pointer to itself */
    syscall2(SYS_arch_prctl, ARCH_SET_FS, (long)(intptr_t)tp);
}

/* ── Symbol resolution ───────────────────────────────────────────────────── */

static elf64_sym_t *resolve_symbol_full(const char *name, obj_t **out_obj)
{
    for (int i = 0; i < MAX_OBJS; i++) {
        obj_t *o = &g_objs[i];
        if (!o->in_use || !o->symtab || !o->strtab) continue;
        for (u32 s = 0; s < o->nsyms; s++) {
            elf64_sym_t *sym = &o->symtab[s];
            if (sym->st_name == 0 || sym->st_shndx == 0) continue; /* null entry / undefined ref */
            if (ld_strcmp(o->strtab + sym->st_name, name) == 0) {
                *out_obj = o;
                return sym;
            }
        }
    }
    return 0;
}

/* ── Relocation ──────────────────────────────────────────────────────────── */

static void apply_relative(obj_t *o)
{
    if (!o->rela) return;
    for (u64 i = 0; i < o->rela_count; i++) {
        elf64_rela_t *r = &o->rela[i];
        if (ELF64_R_TYPE(r->r_info) == R_X86_64_RELATIVE)
            *(u64 *)(uintptr_t)(o->load_bias + r->r_offset) = o->load_bias + (u64)r->r_addend;
    }
}

static int g_dlopen_error; /* set when a non-fatal (dlopen-time) required-symbol lookup fails */

static void apply_symbolic_table(obj_t *o, elf64_rela_t *tbl, u64 count, int fatal)
{
    for (u64 i = 0; i < count; i++) {
        elf64_rela_t *r = &tbl[i];
        u32 type = ELF64_R_TYPE(r->r_info);
        if (type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT &&
            type != R_X86_64_64 && type != R_X86_64_TPOFF64)
            continue; /* RELATIVE already handled; GD-model DTPMOD64/DTPOFF64 unsupported (see top-of-file note) */

        u32 symidx = ELF64_R_SYM(r->r_info);
        u64 *slot = (u64 *)(uintptr_t)(o->load_bias + r->r_offset);

        if (type == R_X86_64_TPOFF64 && symidx == 0) {
            /* Local TLS access within this object: many linkers reduce this
             * to symbol index 0 with the offset already folded into the
             * addend, since it's resolved to "this module" at static-link
             * time already. */
            if (!o->has_tls) {
                ld_write2("ld-azami.so: fatal: TPOFF64(sym=0) in an object with no PT_TLS\n");
                syscall1(SYS_exit, 127);
            }
            *slot = (u64)(o->tls_tpoff + r->r_addend);
            continue;
        }

        if (!o->symtab || !o->strtab) continue;
        elf64_sym_t *local_sym = &o->symtab[symidx];
        const char *name = o->strtab + local_sym->st_name;

        obj_t *def_obj = 0;
        elf64_sym_t *def_sym = resolve_symbol_full(name, &def_obj);

        if (!def_sym) {
            if (ELF64_ST_BIND(local_sym->st_info) == STB_WEAK) continue; /* weak: leave the slot as 0 */
            ld_write2("ld-azami.so: unresolved symbol '");
            ld_write2(name);
            ld_write2("'\n");
            if (fatal) syscall1(SYS_exit, 127);
            g_dlopen_error = 1;
            continue;
        }

        switch (type) {
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
            *slot = def_obj->load_bias + def_sym->st_value;
            break;
        case R_X86_64_64:
            *slot = def_obj->load_bias + def_sym->st_value + (u64)r->r_addend;
            break;
        case R_X86_64_TPOFF64:
            if (!def_obj->has_tls) {
                ld_write2("ld-azami.so: TPOFF64 against non-TLS symbol '");
                ld_write2(name);
                ld_write2("'\n");
                if (fatal) syscall1(SYS_exit, 127);
                g_dlopen_error = 1;
                break;
            }
            *slot = (u64)(def_obj->tls_tpoff + (s64)def_sym->st_value + r->r_addend);
            break;
        default: break;
        }
    }
}

static void apply_symbolic(obj_t *o, int fatal)
{
    if (o->rela)   apply_symbolic_table(o, o->rela, o->rela_count, fatal);
    if (o->jmprel) apply_symbolic_table(o, o->jmprel, o->jmprel_count, fatal);
}

/* ── Constructors (DT_INIT / DT_INIT_ARRAY) ──────────────────────────────── */

static void run_ctors_once(obj_t *o)
{
    if (o->ctors_ran) return;
    o->ctors_ran = 1;

    if (o->dt_init) ((void (*)(void))(uintptr_t)o->dt_init)();

    if (o->dt_init_array) {
        u64 n = o->dt_init_arraysz / sizeof(u64);
        for (u64 i = 0; i < n; i++) {
            u64 fnaddr = o->dt_init_array[i];
            /* Already absolute: RELATIVE relocations (applied before this
             * runs) fix up .init_array's own entries in place. */
            if (fnaddr == 0 || fnaddr == (u64)-1) continue;
            ((void (*)(void))(uintptr_t)fnaddr)();
        }
    }
}

/* Dependency-first, @top_idx last — @top_idx is main at startup, or the
 * object dlopen() just finished relocating. run_ctors_once()'s ctors_ran
 * flag makes this safe to call again later without re-running anything. */
static void run_all_ctors(int top_idx)
{
    for (int i = 0; i < MAX_OBJS; i++)
        if (g_objs[i].in_use && i != top_idx) run_ctors_once(&g_objs[i]);
    if (top_idx >= 0 && g_objs[top_idx].in_use) run_ctors_once(&g_objs[top_idx]);
}

/* ── dlerror() state ──────────────────────────────────────────────────────── */

static char g_dlerror_buf[160];
static int  g_dlerror_pending;

static void set_dlerror(const char *msg)
{
    u64 i = 0;
    while (msg[i] && i < sizeof(g_dlerror_buf) - 1) { g_dlerror_buf[i] = msg[i]; i++; }
    g_dlerror_buf[i] = '\0';
    g_dlerror_pending = 1;
}

/* ── Public runtime ABI for libc's dlopen()/dlsym()/dlclose()/dlerror() ─────
 *
 * These are the only non-static symbols in this file besides ld_main()
 * itself. libc never link-time-depends on ld-azami.so (see
 * userland/libc/ldso_bridge.c): it locates these by hand — reading this
 * process's own AT_BASE, parsing ld-azami.so's own PT_DYNAMIC the same way
 * this file parses any other object's, and looking a name up in its
 * .dynsym — then calls through the resulting function pointer. That keeps
 * this file's "nothing here needs a relocation applied to itself" invariant
 * intact: nothing in this file calls these, so exporting them costs nothing
 * for ld.so's own bootstrap safety. */

void *__ldso_dlopen(const char *path, int flags)
{
    (void)flags; /* RTLD_LAZY/NOW/GLOBAL/LOCAL are all treated the same: eager, global scope */
    if (!path) { set_dlerror("dlopen: NULL path"); return 0; }

    int existing = find_loaded_by_name(path);
    if (existing >= 0) { g_objs[existing].refcount++; return (void *)&g_objs[existing]; }

    char full[128];
    build_lib_path(full, sizeof(full), path);

    int idx = obj_load_file(full, path);
    if (idx < 0) { set_dlerror("dlopen: cannot open or map object"); return 0; }

    if (g_objs[idx].has_tls) {
        set_dlerror("dlopen: object has a PT_TLS segment, unsupported for a runtime load");
        obj_force_unmap(idx);
        return 0;
    }
    if (load_needed_recursive(idx) < 0) {
        set_dlerror("dlopen: a dependency failed to load");
        obj_force_unmap(idx);
        return 0;
    }
    for (int i = 0; i < g_objs[idx].ndeps; i++) {
        if (g_objs[g_objs[idx].dep_idx[i]].has_tls) {
            set_dlerror("dlopen: a dependency has a PT_TLS segment, unsupported for a runtime load");
            obj_force_unmap(idx);
            return 0;
        }
    }

    g_dlopen_error = 0;
    apply_relative(&g_objs[idx]);
    for (int i = 0; i < g_objs[idx].ndeps; i++) apply_relative(&g_objs[g_objs[idx].dep_idx[i]]);
    apply_symbolic(&g_objs[idx], 0 /* not fatal: fail the dlopen(), don't kill the process */);
    for (int i = 0; i < g_objs[idx].ndeps; i++) apply_symbolic(&g_objs[g_objs[idx].dep_idx[i]], 0);

    if (g_dlopen_error) {
        set_dlerror("dlopen: unresolved symbol");
        obj_force_unmap(idx);
        return 0;
    }

    run_all_ctors(idx);
    return (void *)&g_objs[idx];
}

static int handle_valid(void *handle)
{
    obj_t *o = (obj_t *)handle;
    return o >= g_objs && o < g_objs + MAX_OBJS && o->in_use;
}

void *__ldso_dlsym(void *handle, const char *name)
{
    if (!name) { set_dlerror("dlsym: NULL name"); return 0; }

    if (!handle) {
        /* Simplified RTLD_DEFAULT-like behavior: search every loaded object. */
        obj_t *def_obj = 0;
        elf64_sym_t *sym = resolve_symbol_full(name, &def_obj);
        if (!sym) { set_dlerror("dlsym: symbol not found"); return 0; }
        return (void *)(uintptr_t)(def_obj->load_bias + sym->st_value);
    }

    if (!handle_valid(handle)) { set_dlerror("dlsym: invalid handle"); return 0; }
    obj_t *o = (obj_t *)handle;

    if (o->symtab && o->strtab) {
        for (u32 s = 0; s < o->nsyms; s++) {
            elf64_sym_t *sym = &o->symtab[s];
            if (sym->st_name && sym->st_shndx && ld_strcmp(o->strtab + sym->st_name, name) == 0)
                return (void *)(uintptr_t)(o->load_bias + sym->st_value);
        }
    }
    /* One level into its own recorded dependencies — a deliberate
     * simplification, not a fully transitive search (see top-of-file note). */
    for (int i = 0; i < o->ndeps; i++) {
        obj_t *d = &g_objs[o->dep_idx[i]];
        if (!d->symtab || !d->strtab) continue;
        for (u32 s = 0; s < d->nsyms; s++) {
            elf64_sym_t *sym = &d->symtab[s];
            if (sym->st_name && sym->st_shndx && ld_strcmp(d->strtab + sym->st_name, name) == 0)
                return (void *)(uintptr_t)(d->load_bias + sym->st_value);
        }
    }
    set_dlerror("dlsym: symbol not found");
    return 0;
}

int __ldso_dlclose(void *handle)
{
    if (!handle_valid(handle)) { set_dlerror("dlclose: invalid handle"); return -1; }
    obj_t *o = (obj_t *)handle;
    o->refcount--;
    if (o->refcount <= 0 && !o->pinned) obj_force_unmap((int)(o - g_objs));
    return 0;
}

const char *__ldso_dlerror(void)
{
    if (!g_dlerror_pending) return 0;
    g_dlerror_pending = 0;
    return g_dlerror_buf;
}

/* Combined static TLS layout query for userland/libc/tls.c's
 * __init_thread_tls() — exposed as plain scalars rather than a struct
 * pointer so the two independently-compiled translation units never have
 * to agree on a struct layout, only on a calling convention. */
unsigned long __ldso_tls_nmodules(void)
{
    return (unsigned long)g_tls_info.nmodules;
}

int __ldso_tls_module(unsigned long i, unsigned long *out_vaddr, unsigned long *out_filesz,
                       unsigned long *out_memsz, long *out_tpoff)
{
    if (i >= g_tls_info.nmodules) return -1;
    if (out_vaddr)  *out_vaddr  = g_tls_info.mods[i].vaddr;
    if (out_filesz) *out_filesz = g_tls_info.mods[i].filesz;
    if (out_memsz)  *out_memsz  = g_tls_info.mods[i].memsz;
    if (out_tpoff)  *out_tpoff  = g_tls_info.mods[i].tpoff;
    return 0;
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

/* visibility("hidden"): ld_main is called exactly once, by crt0.asm's `call
 * ld_main wrt ..plt`, from *within this same* ld-azami.so image — and this
 * image is the one object nobody ever relocates (not the kernel, which
 * loads it with a plain segment copy, and not this file itself, by design
 * — see the top-of-file comment). A PLT32 relocation against a default-
 * visibility (preemptible) global symbol gets a real lazy-PLT GOT slot,
 * whose un-relocated default content is the classic "push $reloc_idx; jmp
 * PLT0" lazy-resolver-stub address — a link-time (base-0) constant that is
 * only correct if this image's runtime load bias happens to be 0. Real
 * ASLR bias breaks it: crt0.asm's call jumps through that never-patched
 * GOT slot straight into unmapped memory (found by booting: every single
 * dynamically-linked process, including ld-azami.so's own bootstrap for a
 * completely static-relative main executable, faulted at the exact same
 * literal, un-biased address — the giveaway that this was ld-azami.so's
 * own startup crashing, not anything downstream). Hidden visibility tells
 * the static linker ld_main can never be interposed from outside this
 * object, so it resolves the call directly (no PLT/GOT indirection, no
 * relocation needed at all) — restoring the "nothing in this file needs a
 * relocation applied to itself" invariant the rest of this comment block
 * already claimed, but this one call was quietly violating. */
__attribute__((visibility("hidden")))
long ld_main(long argc, char **argv, char **envp, u64 initial_rsp)
{
    (void)argc; (void)argv;

    elf64_auxv_t *av = find_auxv(envp);
    u64 at_phdr  = auxv_get(av, AT_PHDR);
    u64 at_phnum = auxv_get(av, AT_PHNUM);
    u64 at_entry = auxv_get(av, AT_ENTRY);

    if (!at_phdr || !at_entry) {
        ld_write2("ld-azami.so: auxv missing AT_PHDR/AT_ENTRY\n");
        syscall1(SYS_exit, 127);
    }

    /* See this file's top comment: guaranteed by scripts/user-pic.ld. */
    u64 main_bias = at_phdr - 64;
    elf64_phdr_t *main_phdrs_src = (elf64_phdr_t *)(uintptr_t)at_phdr;

    int main_idx = obj_alloc();
    obj_t *mo = &g_objs[main_idx];
    ld_memset(mo, 0, sizeof(*mo));
    mo->in_use    = 1;
    mo->refcount  = 1;
    mo->load_bias = main_bias;
    mo->map_base  = 0; /* kernel-owned; never munmap the main image */

    u32 nphdrs = (u32)at_phnum;
    if (nphdrs > LD_MAX_PHDRS) nphdrs = LD_MAX_PHDRS;
    for (u32 i = 0; i < nphdrs; i++) mo->phdr_storage[i] = main_phdrs_src[i];
    mo->phnum = nphdrs;

    find_tls_in(mo, mo->phdr_storage, mo->phnum);

    elf64_dyn_t *main_dyn = find_dynamic_in(mo->phdr_storage, mo->phnum, main_bias);
    if (!main_dyn) {
        /* No PT_DYNAMIC in the main image: nothing to relocate or link
         * against. Jump straight to its entry point. */
        jump_to_entry(at_entry, initial_rsp);
    }
    parse_dynamic(mo, main_dyn);

    if (load_needed_recursive(main_idx) < 0) {
        ld_write2("ld-azami.so: fatal: a required dependency failed to load\n");
        syscall1(SYS_exit, 127);
    }

    /* Everything loaded up to here was pulled in eagerly at process start;
     * pin it so a later dlclose() (of an object that happens to share a
     * name with one of these, e.g. a redundant dlopen("libc.so")) can never
     * actually unmap something still in active use. */
    for (int i = 0; i < MAX_OBJS; i++) if (g_objs[i].in_use) g_objs[i].pinned = 1;

    setup_static_tls();

    for (int i = 0; i < MAX_OBJS; i++) if (g_objs[i].in_use) apply_relative(&g_objs[i]);
    for (int i = 0; i < MAX_OBJS; i++) if (g_objs[i].in_use) apply_symbolic(&g_objs[i], 1 /* fatal */);

    run_all_ctors(main_idx);

    jump_to_entry(at_entry, initial_rsp);
}
