#include <dlfcn.h>
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
    printf("dynamic probe complete: %d failed\n", failures);
    return failures ? 1 : 0;
}
