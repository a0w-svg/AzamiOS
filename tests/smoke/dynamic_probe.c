#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

/* On-disk ELF64 layout; also usable with the freestanding native libc. */
struct elf_header {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct program_header {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

extern int smoke_start_value(void);
static int failures;

static void check(int ok, const char *name)
{
    printf("  %s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}

static int elf_magic(unsigned long address)
{
    return address && memcmp((const void *)address, "\177ELF", 4) == 0;
}

struct probe_phdr_state {
    int count;
    int seen_phdrs;
};

static int probe_phdr_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    struct probe_phdr_state *st = (struct probe_phdr_state *)data;
    st->count++;
    if (info->dlpi_phdr && info->dlpi_phnum > 0) {
        st->seen_phdrs = 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    puts("-- dynamic ELF / auxv / dlopen --");
    check(argc == 2 && strcmp(argv[1], "--smoke") == 0, "entry argc/argv");
    const char *token = getenv("AZAMI_SMOKE_TOKEN");
    check(token && strcmp(token, "loader-environment") == 0, "entry environment");
    check(getauxval(AT_PAGESZ) == 4096, "AT_PAGESZ");
    errno = 0;
    check(getauxval(AT_SECURE) == 0 && errno == 0, "AT_SECURE present and zero");
    unsigned long minsig = getauxval(51); /* AT_MINSIGSTKSZ */
    check(minsig >= 1776, "AT_MINSIGSTKSZ >= 1776");
    const char *bplat = (const char *)getauxval(24); /* AT_BASE_PLATFORM */
    check(bplat && strcmp(bplat, "x86_64") == 0, "AT_BASE_PLATFORM is x86_64");
    unsigned long random = getauxval(AT_RANDOM);
    check(random != 0, "AT_RANDOM present");
    if (random) {
        unsigned char bits = 0;
        for (int i = 0; i < 16; i++) bits |= ((unsigned char *)random)[i];
        check(bits != 0, "AT_RANDOM readable 16-byte entropy");
    }
    check(elf_magic(getauxval(AT_SYSINFO_EHDR)), "AT_SYSINFO_EHDR ELF image");
    check(elf_magic(getauxval(AT_BASE)), "AT_BASE interpreter ELF image");
    const struct program_header *ph = (const void *)getauxval(AT_PHDR);
    unsigned long phnum = getauxval(AT_PHNUM);
    int valid_ph = ph && phnum > 0 && phnum < 128 &&
                   getauxval(AT_PHENT) == sizeof(*ph);
    check(valid_ph, "AT_PHDR / AT_PHNUM / AT_PHENT");
    if (valid_ph) {
        unsigned long bias = 0;
        int interp_found = 0, entry_found = 0;
        for (unsigned long i = 0; i < phnum; i++) {
            if (ph[i].type == 6) /* PT_PHDR */
                bias = (unsigned long)ph - ph[i].vaddr;
            if (ph[i].type == 1 && ph[i].offset == 0) {
                const struct elf_header *eh = (const void *)
                    ((unsigned long)ph - sizeof(struct elf_header));
                if (elf_magic((unsigned long)eh) && eh->phoff == sizeof(*eh))
                    bias = (unsigned long)eh - ph[i].vaddr;
            }
        }
        unsigned long entry = getauxval(AT_ENTRY);
        for (unsigned long i = 0; i < phnum; i++) {
            if (ph[i].type == 3) interp_found = 1; /* PT_INTERP */
            if (ph[i].type == 1 && (ph[i].flags & 1) &&
                entry >= bias + ph[i].vaddr &&
                entry - bias - ph[i].vaddr < ph[i].memsz)
                entry_found = 1;
        }
        check(interp_found, "PT_INTERP in mapped program headers");
        check(entry_found, "AT_ENTRY inside executable PT_LOAD");
    }
    errno = 0;
    check(getauxval(0xdeadbeefUL) == 0 && errno == ENOENT,
          "absent auxv entry returns ENOENT");
    check(smoke_start_value() == 42, "DT_NEEDED / constructor / relocation / TLS");

    dlerror();
    check(dlopen("/lib/missing-smoke-library.so", RTLD_NOW) == NULL,
          "dlopen missing library fails");
    check(dlerror() != NULL, "dlopen error reported");
    check(dlerror() == NULL, "dlerror clears after retrieval");

    void *handle = dlopen("/lib/libsmoke_plugin.so", RTLD_NOW | RTLD_LOCAL);
    check(handle != NULL, "dlopen plugin");
    if (handle) {
        dlerror();
        int (*value)(int) = (int (*)(int))dlsym(handle, "smoke_plugin_value");
        const char *error = dlerror();
        check(value != NULL && error == NULL, "dlsym exported function");
        if (value) check(value(7) == 49, "plugin dependency / constructor / call");
        dlerror();
        check(dlsym(handle, "missing_smoke_symbol") == NULL,
              "dlsym missing symbol fails");
        check(dlerror() != NULL, "dlsym error reported");
        check(dlclose(handle) == 0, "dlclose plugin");
    }

    /* Symbol versioning tests */
    void *(*probe_dlvsym)(void *, const char *, const char *) = 0;
#ifdef __AZAMI_LIBC__
    probe_dlvsym = dlvsym;
#else
    probe_dlvsym = (void *(*)(void *, const char *, const char *))dlsym(RTLD_DEFAULT, "dlvsym");
#endif

    void *h_ver = dlopen("/lib/libsmoke_versioned.so", RTLD_NOW | RTLD_LOCAL);
    check(h_ver != NULL, "dlopen versioned library");
    if (h_ver) {
        dlerror();
        int (*foo_def)(void) = (int (*)(void))dlsym(h_ver, "smoke_versioned_foo");
        const char *err = dlerror();
        check(foo_def != NULL && err == NULL, "dlsym unversioned finds default version");
        if (foo_def) check(foo_def() == 222, "dlsym unversioned calls default @@VER_2.0 (222)");

        if (probe_dlvsym) {
            dlerror();
            int (*foo_v1)(void) = (int (*)(void))probe_dlvsym(h_ver, "smoke_versioned_foo", "VER_1.0");
            err = dlerror();
            check(foo_v1 != NULL && err == NULL, "dlvsym VER_1.0 finds hidden version");
            if (foo_v1) check(foo_v1() == 111, "dlvsym VER_1.0 calls foo@VER_1.0 (111)");

            dlerror();
            int (*foo_v2)(void) = (int (*)(void))probe_dlvsym(h_ver, "smoke_versioned_foo", "VER_2.0");
            err = dlerror();
            check(foo_v2 != NULL && err == NULL, "dlvsym VER_2.0 finds default version");
            if (foo_v2) check(foo_v2() == 222, "dlvsym VER_2.0 calls foo@@VER_2.0 (222)");

            dlerror();
            check(probe_dlvsym(h_ver, "smoke_versioned_foo", "VER_NONEXISTENT") == NULL,
                  "dlvsym nonexistent version fails");
            check(dlerror() != NULL, "dlvsym error reported for nonexistent version");
        }
        check(dlclose(h_ver) == 0, "dlclose versioned library");
    }

    void *h_c1 = dlopen("/lib/libsmoke_consumer_v1.so", RTLD_NOW | RTLD_LOCAL);
    check(h_c1 != NULL, "dlopen consumer_v1 library");
    if (h_c1) {
        int (*call_v1)(void) = (int (*)(void))dlsym(h_c1, "smoke_call_v1");
        if (call_v1) {
#ifdef __AZAMI_LIBC__
            check(call_v1() == 111, "relocation binding to foo@VER_1.0 (111)");
#else
            check(call_v1() == 111 || call_v1() == 222, "relocation binding to foo (musl fallback)");
#endif
        }
        check(dlclose(h_c1) == 0, "dlclose consumer_v1 library");
    }

    void *h_c2 = dlopen("/lib/libsmoke_consumer_v2.so", RTLD_NOW | RTLD_LOCAL);
    check(h_c2 != NULL, "dlopen consumer_v2 library");
    if (h_c2) {
        int (*call_v2)(void) = (int (*)(void))dlsym(h_c2, "smoke_call_v2");
        check(call_v2 != NULL, "dlsym smoke_call_v2");
        if (call_v2) check(call_v2() == 222, "relocation binding to foo@@VER_2.0 (222)");
        check(dlclose(h_c2) == 0, "dlclose consumer_v2 library");
    }

    /* dladdr probe */
    Dl_info dlinfo;
    memset(&dlinfo, 0, sizeof(dlinfo));
    int dladdr_ok = dladdr((const void *)smoke_start_value, &dlinfo);
    check(dladdr_ok != 0 && dlinfo.dli_sname != NULL && strcmp(dlinfo.dli_sname, "smoke_start_value") == 0,
          "dladdr finds smoke_start_value symbol");

    /* dl_iterate_phdr probe */
    struct probe_phdr_state ph_st = {0, 0};
    int iter_rc = dl_iterate_phdr(probe_phdr_cb, &ph_st);
    check(iter_rc == 0 && ph_st.count >= 1 && ph_st.seen_phdrs,
          "dl_iterate_phdr enumerates loaded objects");

    printf("dynamic probe complete: %d failed\n", failures);
    return failures ? 1 : 0;
}
