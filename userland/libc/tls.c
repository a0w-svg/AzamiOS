/* ============================================================================
 * AzamiOS Userspace — TLS bootstrap (main thread and pthread_create threads)
 * File: userland/libc/tls.c
 *
 * Sets up the x86-64 psABI "variant II" static TLS block for a thread and
 * points %fs at it via arch_prctl(ARCH_SET_FS, ...), before any code —
 * including __libc_init() — can touch a __thread variable such as errno.
 *
 * The main thread (__init_tls(), called from crt0 before __libc_init())
 * locates this executable's own PT_TLS program header by walking the auxv
 * the kernel already pushes onto the initial stack for every exec
 * (AT_PHDR/AT_PHENT/AT_PHNUM — see kernel/sched/elf.c's setup_user_stack())
 * rather than trusting a linker-script symbol to exactly match whatever
 * alignment padding the linker actually chose for PT_TLS. This mirrors what
 * glibc's __libc_setup_tls() and musl's __init_tls() do for the same
 * reason: the auxv values are the exact numbers the linker computed the
 * R_X86_64_TPOFF32 relocations for __thread variables against, so reading
 * them back at runtime can never disagree with the link.
 *
 * A pthread_create()'d thread (__init_thread_tls(), called from
 * thread_startup_trampoline() in pthread.c, running as the new thread
 * itself — arch_prctl only ever affects the calling thread) reuses the
 * PT_TLS template __init_tls() already found: every thread in a process
 * runs the same executable image, so there is nothing left to discover.
 * It allocates its block on the heap instead of the static buffer __init_tls
 * uses, since malloc() is safe to call by the time a thread is created
 * (well after __libc_init()).
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

/* Real .tdata/.tbss content in this libc is small (a handful of __thread
 * scalars and fixed-size arrays), so a fixed static buffer is generous
 * headroom for the main thread without needing malloc — which is not yet
 * safe to call this early, before __libc_init() has run. */
#define MAIN_TLS_BUF_SIZE 16384
static unsigned char g_main_tls_buf[MAIN_TLS_BUF_SIZE] __attribute__((aligned(64)));

/* PT_TLS template cached by __init_tls(), reused by __init_thread_tls() for
 * every later pthread_create()'d thread. */
static uint64_t g_tls_vaddr;   /* source to copy .tdata's initializer from */
static uint64_t g_tls_filesz;  /* bytes to copy (the .tdata part) */
static uint64_t g_tls_size;    /* total block size, aligned (.tdata+.tbss) */
static uint64_t g_tls_align;
static int      g_tls_present;

static unsigned long tls_align_up(unsigned long v, unsigned long a)
{
    if (a == 0) a = 1;
    return (v + a - 1) & ~(a - 1);
}

/* Copies the .tdata image into @block, zero-fills the .tbss tail out to
 * g_tls_size, writes the variant-II self-pointer at the end, and points
 * %fs at it — for whichever thread is currently running. */
static void install_tls_block(unsigned char *block)
{
    unsigned char *tdata_src = (unsigned char *)(uintptr_t)g_tls_vaddr;
    for (uint64_t i = 0; i < g_tls_filesz; i++)
        block[i] = tdata_src[i];
    for (uint64_t i = g_tls_filesz; i < g_tls_size; i++)
        block[i] = 0;

    void *tp = (void *)(block + g_tls_size);
    *(void **)tp = tp; /* variant II: %fs:0 holds a pointer to itself */

    syscall2(SYS_arch_prctl, ARCH_SET_FS, (long)(intptr_t)tp);
}

/* __init_tls() — called from crt0 with envp, before __libc_init(). Finds
 * PT_TLS in this process's own program headers, builds the variant-II TLS
 * block for the main thread, and points %fs at it. Deliberately touches no
 * __thread state (including errno) itself: %fs is not live yet when this
 * runs. Silently does nothing if the binary has no PT_TLS (no __thread
 * variables) or the auxv is missing/malformed — callers then simply must
 * not use __thread storage, same as before this file existed. */
void __init_tls(char **envp)
{
    char **p = envp;
    while (*p) p++;
    p++; /* skip envp's NULL terminator to reach auxv */
    elf64_auxv_t *auxv = (elf64_auxv_t *)(void *)p;

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

    g_tls_vaddr  = tls_phdr->p_vaddr;
    g_tls_filesz = tls_phdr->p_filesz;
    g_tls_size   = tls_size;
    g_tls_align  = align;
    g_tls_present = 1;

    install_tls_block((unsigned char *)(uintptr_t)base);
}

/* __init_thread_tls() — called by a newly created thread, from the start of
 * thread_startup_trampoline() in pthread.c, before it runs the caller's
 * start_routine. Reuses the PT_TLS template __init_tls() found for the main
 * thread. A no-op if the binary has no __thread variables, or if the
 * allocation fails (the thread then runs without a valid %fs — any
 * __thread access it makes will read/write through whatever %fs happened
 * to be left at, which is a real limitation of this minimal implementation,
 * not a crash-safety issue pthread_create() can detect in advance). */
void __init_thread_tls(void)
{
    if (!g_tls_present)
        return;

    unsigned long need = (unsigned long)g_tls_size + (unsigned long)g_tls_align + sizeof(void *);
    unsigned char *mem = (unsigned char *)malloc(need);
    if (!mem)
        return;

    unsigned long base = tls_align_up((unsigned long)(uintptr_t)mem, (unsigned long)g_tls_align);
    install_tls_block((unsigned char *)(uintptr_t)base);
}
