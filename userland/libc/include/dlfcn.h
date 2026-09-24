/* ============================================================================
 * AzamiOS Userspace — Dynamic Linking Loader Interface (dlfcn.h)
 * File: userland/libc/include/dlfcn.h
 *
 * dlopen()/dlsym()/dlclose()/dlerror() reach ld-azami.so's real
 * __ldso_dlopen/__ldso_dlsym/__ldso_dlclose/__ldso_dlerror exports via a
 * hand-resolved lookup against the dynamic linker's own .dynsym — see
 * userland/libc/ldso_bridge.c (the lookup mechanism) and
 * userland/libc/dlfcn.c (this header's implementation).
 *
 * RTLD_LAZY vs RTLD_NOW is NOT a real distinction on AzamiOS: ld-azami.so
 * always resolves every relocation eagerly at dlopen() time (see
 * userland/ldso/ldso.c's top-of-file comment — there is no PLT lazy-binding
 * stub mechanism implemented). Both macros are accepted for source
 * compatibility only. Likewise RTLD_GLOBAL/RTLD_LOCAL: ld.so's symbol
 * resolution is always a global scan over every loaded object (see
 * resolve_symbol_full() in ldso.c), so there is no meaningful "local"
 * scoping to honor. Passing either has no observable effect.
 * ============================================================================ */
#pragma once

#define RTLD_LAZY   0x0001  /* accepted, but ld-azami.so always binds eagerly */
#define RTLD_NOW    0x0002  /* the actual (only) behavior; both flags do the same thing */
#define RTLD_GLOBAL 0x0100  /* accepted, but symbol scope is always global */
#define RTLD_LOCAL  0x0000  /* accepted, but has no effect (see above) */

#define RTLD_DEFAULT ((void *)0)  /* dlsym(): search every loaded object */

#ifdef __cplusplus
extern "C" {
#endif

void *dlopen(const char *path, int flags);
void *dlsym(void *handle, const char *name);
int   dlclose(void *handle);
char *dlerror(void);

#ifdef __cplusplus
}
#endif
