/* ============================================================================
 * AzamiOS Userspace — TLS bootstrap (main thread and pthread_create threads)
 * File: userland/libc/tls.c
 *
 * Sets up the x86-64 psABI "variant II" static TLS block for a thread and
 * points %fs at it via arch_prctl(ARCH_SET_FS, ...), before any code —
 * including __libc_init() — can touch a __thread variable such as errno.
 *
 * Two very different situations reach __init_tls(), told apart by whether
 * %fs is already non-zero on entry (a fresh process from the kernel always
 * starts with fs_base == 0 — see kernel/syscall/syscall.c's execve path):
 *
 *   - Statically linked (no PT_INTERP): __init_tls() does everything
 *     itself, exactly as before this file grew ld.so-awareness — it locates
 *     this executable's own PT_TLS program header by walking the auxv the
 *     kernel already pushes onto the initial stack for every exec
 *     (AT_PHDR/AT_PHENT/AT_PHNUM — see kernel/sched/elf.c's
 *     setup_user_stack()) rather than trusting a linker-script symbol to
 *     exactly match whatever alignment padding the linker actually chose,
 *     the same reason glibc's __libc_setup_tls() and musl's __init_tls()
 *     read auxv back for the same purpose.
 *
 *   - Dynamically linked (PT_INTERP -> ld-azami.so): userland/ldso/ldso.c's
 *     setup_static_tls() has *already* built a combined static TLS block —
 *     covering the main executable and every DT_NEEDED dependency that has
 *     its own PT_TLS (most importantly a future libc.so's own __thread
 *     errno) — and pointed %fs at it, before ever jumping to this
 *     executable's entry point. __init_tls() detects that and, instead of
 *     redoing the work (and getting it wrong: it only knows about *this*
 *     image's own PT_TLS, not any dependency's), fetches the layout ld.so
 *     already computed via userland/libc/ldso_bridge.c's hand-resolved
 *     lookup of ld.so's exported __ldso_tls_nmodules()/__ldso_tls_module()
 *     and caches it — so __init_thread_tls() can still replicate every
 *     module for a later pthread_create()'d thread.
 *
 * Either way, the result cached here is a list of "modules" (1 in the
 * static case, N in the dynamic case), each with a *fixed* tpoff — the
 * signed offset from %fs:0 fixed at layout time and baked into every
 * R_X86_64_TPOFF32/TPOFF64 relocation against it, identical for every
 * thread. install_tls_block() below places each module at
 * (this_thread's_tp + module's_tpoff) — the only thing that differs
 * between threads is where the block itself lives, never the offsets code
 * uses to reach into it.
 *
 * Known limitation, not addressed here: neither pthread_join()/pthread_exit()
 * nor this file frees a thread's TLS block when the thread ends — matching
 * the existing, separately-tracked leak where a thread's stack (allocated in
 * pthread_create()) is never freed either. malloc()'s own thread-safety
 * under concurrent calls from multiple running threads has also not been
 * audited as part of this change.
 * ============================================================================ */
#include <stdint.h>
#include <sys/syscall.h>
#include "include/stdlib.h"

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5

#define PT_TLS    7

#ifndef ARCH_SET_FS
#define ARCH_SET_FS 0x1002
#endif
#ifndef ARCH_GET_FS
#define ARCH_GET_FS 0x1003
#endif

/* Elf64_Phdr, hand-declared: freestanding builds have no <elf.h>. */
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

/* One (type, value) auxv pair, matching how kernel/sched/elf.c pushes them. */
typedef struct {
    uint64_t a_type;
    uint64_t a_val;
} elf64_auxv_t;

/* userland/libc/ldso_bridge.c */
extern void  __libc_stash_auxv(void *auxv);
extern void *__libc_ldso_lookup(const char *name);

/* Real .tdata/.tbss content in this libc is small (a handful of __thread
 * scalars and fixed-size arrays), so a fixed static buffer is generous
 * headroom for the main thread without needing malloc — which is not yet
 * safe to call this early, before __libc_init() has run. Also used as the
 * combined block when running under ld.so, since ld.so's own block (already
 * installed and pointed at by %fs by the time this file ever looks at it)
 * is never touched here — this buffer only matters for the single-module,
 * statically-linked path. */
#define MAIN_TLS_BUF_SIZE 16384
static unsigned char g_main_tls_buf[MAIN_TLS_BUF_SIZE] __attribute__((aligned(64)));

/* One static TLS module: fixed offset from %fs:0, same for every thread. */
#define MAX_TLS_MODULES 16
typedef struct {
    uint64_t vaddr;   /* source to copy this module's .tdata initializer from */
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
    long     tpoff;
} tls_mod_t;

static tls_mod_t     g_tls_mods[MAX_TLS_MODULES];
static unsigned int  g_tls_nmodules;

static unsigned long tls_align_up(unsigned long v, unsigned long a)
{
    if (a == 0) a = 1;
    return (v + a - 1) & ~(a - 1);
}

/* Copies every cached module's .tdata image into its fixed
 * (tp + module.tpoff) offset, zero-fills each module's .tbss tail, writes
 * the variant-II self-pointer at tp, and points %fs at it — for whichever
 * thread is currently running. @tp is the address where the self-pointer
 * word itself lives; every module's tpoff is already relative to it. */
static void install_tls_block(void *tp)
{
    for (unsigned int i = 0; i < g_tls_nmodules; i++) {
        unsigned char *dst = (unsigned char *)tp + g_tls_mods[i].tpoff;
        const unsigned char *src = (const unsigned char *)(uintptr_t)g_tls_mods[i].vaddr;
        for (uint64_t k = 0; k < g_tls_mods[i].filesz; k++) dst[k] = src[k];
        for (uint64_t k = g_tls_mods[i].filesz; k < g_tls_mods[i].memsz; k++) dst[k] = 0;
    }
    *(void **)tp = tp; /* variant II: %fs:0 holds a pointer to itself */
    syscall2(SYS_arch_prctl, ARCH_SET_FS, (long)(intptr_t)tp);
}

/* Total combined block size (everything below %fs:0), derived from the
 * first-packed module's tpoff: setup_static_tls() in ldso.c (and the
 * single-module path below, which mirrors it) always places the first
 * module at block offset 0, i.e. tpoff == -total. */
static uint64_t tls_total_size(void)
{
    if (g_tls_nmodules == 0) return 0;
    return (uint64_t)(-g_tls_mods[0].tpoff);
}

/* __init_tls() — called from crt0 with envp, before __libc_init(). */
void __init_tls(char **envp)
{
    char **p = envp;
    while (*p) p++;
    p++; /* skip envp's NULL terminator to reach auxv */
    elf64_auxv_t *auxv = (elf64_auxv_t *)(void *)p;

    __libc_stash_auxv(auxv); /* so a later dlopen()/dlsym()/dlclose() (dlfcn.c)
                               * and __init_thread_tls() below can reach
                               * ld.so's exports too. */

    /* Already set up by ld-azami.so before it jumped here? A fresh process
     * always starts with fs_base == 0 (kernel/syscall/syscall.c's execve
     * path); ld.so is the only other thing that could have changed it by
     * the time this runs. */
    uint64_t cur_fs = 0;
    syscall2(SYS_arch_prctl, ARCH_GET_FS, (long)(intptr_t)&cur_fs);
    if (cur_fs != 0) {
        unsigned long (*nmod_fn)(void) = (unsigned long (*)(void))__libc_ldso_lookup("__ldso_tls_nmodules");
        int (*mod_fn)(unsigned long, unsigned long *, unsigned long *, unsigned long *, long *) =
            (int (*)(unsigned long, unsigned long *, unsigned long *, unsigned long *, long *))
                __libc_ldso_lookup("__ldso_tls_module");
        if (nmod_fn && mod_fn) {
            unsigned long n = nmod_fn();
            if (n > MAX_TLS_MODULES) n = MAX_TLS_MODULES;
            for (unsigned long i = 0; i < n; i++) {
                unsigned long vaddr = 0, filesz = 0, memsz = 0;
                long tpoff = 0;
                if (mod_fn(i, &vaddr, &filesz, &memsz, &tpoff) != 0) break;
                g_tls_mods[i].vaddr  = vaddr;
                g_tls_mods[i].filesz = filesz;
                g_tls_mods[i].memsz  = memsz;
                g_tls_mods[i].tpoff  = tpoff;
            }
            g_tls_nmodules = (unsigned int)n;
        }
        return; /* %fs is already correct for this (the main) thread. */
    }

    uint64_t phdr_addr = 0, phent = 0, phnum = 0;
    for (elf64_auxv_t *a = auxv; a->a_type != AT_NULL; a++) {
        if (a->a_type == AT_PHDR)       phdr_addr = a->a_val;
        else if (a->a_type == AT_PHENT) phent = a->a_val;
        else if (a->a_type == AT_PHNUM) phnum = a->a_val;
    }
    if (!phdr_addr || !phent || !phnum)
        return;

    elf64_phdr_t *tls_phdr = 0;
    for (uint64_t i = 0; i < phnum; i++) {
        elf64_phdr_t *ph = (elf64_phdr_t *)(uintptr_t)(phdr_addr + i * phent);
        if (ph->p_type == PT_TLS) { tls_phdr = ph; break; }
    }
    if (!tls_phdr || tls_phdr->p_memsz == 0)
        return; /* this binary has no __thread variables */

    uint64_t align = tls_phdr->p_align ? tls_phdr->p_align : 8;
    if (align < 8) align = 8; /* the self-pointer word needs 8-byte alignment */
    uint64_t tls_size = tls_align_up((unsigned long)tls_phdr->p_memsz, (unsigned long)align);

    unsigned long base = tls_align_up((unsigned long)(uintptr_t)g_main_tls_buf, (unsigned long)align);
    if (tls_size + sizeof(void *) > MAIN_TLS_BUF_SIZE - (base - (unsigned long)(uintptr_t)g_main_tls_buf))
        return; /* .tdata/.tbss outgrew the static buffer — bail rather than corrupt memory */

    g_tls_mods[0].vaddr  = tls_phdr->p_vaddr; /* ET_EXEC, base 0: design vaddr == runtime address */
    g_tls_mods[0].filesz = tls_phdr->p_filesz;
    g_tls_mods[0].memsz  = tls_size;
    g_tls_mods[0].align  = align;
    g_tls_mods[0].tpoff  = -(long)tls_size;
    g_tls_nmodules = 1;

    void *tp = (void *)(base + tls_size);
    install_tls_block(tp);
}

/* __init_thread_tls() — called by a newly created thread, from the start of
 * thread_startup_trampoline() in pthread.c, before it runs the caller's
 * start_routine. Reuses the module list __init_tls() already found (whether
 * that came from this binary's own single PT_TLS or from ld.so's combined
 * layout) — every thread in a process runs the same set of loaded images,
 * so there is nothing left to discover. Allocates its block on the heap
 * instead of the static buffer __init_tls() uses, since malloc() is safe to
 * call by the time a thread is created (well after __libc_init()). A no-op
 * if nothing has any __thread variables, or if the allocation fails (the
 * thread then runs without a valid %fs — any __thread access it makes will
 * read/write through whatever %fs happened to be left at, a real limitation
 * of this minimal implementation, not a crash-safety issue pthread_create()
 * can detect in advance). */
void __init_thread_tls(void)
{
    if (g_tls_nmodules == 0)
        return;

    uint64_t total = tls_total_size();
    /* A conservative alignment covering every module: 64 bytes is already
     * more than any TLS variable in this libc (or a real dependency) needs. */
    unsigned long need = (unsigned long)total + 64 + sizeof(void *);
    unsigned char *mem = (unsigned char *)malloc(need);
    if (!mem)
        return;

    unsigned long base = tls_align_up((unsigned long)(uintptr_t)mem, 64);
    void *tp = (void *)(base + total);
    install_tls_block(tp);
}
