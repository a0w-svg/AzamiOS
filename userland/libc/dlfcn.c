/* ============================================================================
 * AzamiOS Userspace — dlopen()/dlsym()/dlclose()/dlerror()
 * File: userland/libc/dlfcn.c
 *
 * Thin wrappers over ld-azami.so's real __ldso_dlopen/__ldso_dlsym/
 * __ldso_dlclose/__ldso_dlerror exports, reached by hand-resolving each
 * symbol out of ld.so's own loaded image (userland/libc/ldso_bridge.c's
 * __libc_ldso_lookup(), keyed off the AT_BASE auxv entry — see that file's
 * header comment for exactly why this can't be an ordinary DT_NEEDED-style
 * link). Each dl*() call here re-resolves its target function pointer rather
 * than caching one at startup: these are not hot-path calls, and it keeps
 * this file correct even in the hypothetical of ld.so's own image moving
 * (it never does today, but nothing here relies on it not to).
 *
 * When there is no ld.so present at all (a purely static binary — AT_BASE
 * == 0, see ldso_bridge.c), every __libc_ldso_lookup() call below returns
 * NULL and every dl*() function here fails cleanly: dlopen() returns NULL,
 * dlclose() returns -1, dlsym() returns NULL, dlerror() reports why. None of
 * this crashes — "no dynamic linker" is an ordinary, expected case for any
 * binary linked purely against libc.a with no PT_INTERP.
 * ============================================================================ */
#include "include/dlfcn.h"
#include <stddef.h>

/* userland/libc/ldso_bridge.c */
extern void *__libc_ldso_lookup(const char *name);

static const char *g_dlerror_msg;

static void set_local_dlerror(const char *msg)
{
    g_dlerror_msg = msg;
}

void *dlopen(const char *path, int flags)
{
    void *(*fn)(const char *, int) =
        (void *(*)(const char *, int))__libc_ldso_lookup("__ldso_dlopen");
    if (!fn) {
        set_local_dlerror("dlopen: no dynamic linker present (statically linked)");
        return NULL;
    }
    void *h = fn(path, flags);
    if (!h) {
        /* ld.so's own dlerror() has a more specific message — surface it
         * through our dlerror() too, by fetching it right now (its buffer
         * is one-shot: __ldso_dlerror() clears the pending flag once read,
         * so this is the only correct place to pull it). */
        const char *(*ldso_dlerror)(void) =
            (const char *(*)(void))__libc_ldso_lookup("__ldso_dlerror");
        const char *msg = ldso_dlerror ? ldso_dlerror() : NULL;
        set_local_dlerror(msg ? msg : "dlopen: failed");
    }
    return h;
}

void *dlsym(void *handle, const char *name)
{
    void *(*fn)(void *, const char *) =
        (void *(*)(void *, const char *))__libc_ldso_lookup("__ldso_dlsym");
    if (!fn) {
        set_local_dlerror("dlsym: no dynamic linker present (statically linked)");
        return NULL;
    }
    void *sym = fn(handle, name);
    if (!sym) {
        const char *(*ldso_dlerror)(void) =
            (const char *(*)(void))__libc_ldso_lookup("__ldso_dlerror");
        const char *msg = ldso_dlerror ? ldso_dlerror() : NULL;
        set_local_dlerror(msg ? msg : "dlsym: symbol not found");
    }
    return sym;
}

int dlclose(void *handle)
{
    int (*fn)(void *) = (int (*)(void *))__libc_ldso_lookup("__ldso_dlclose");
    if (!fn) {
        set_local_dlerror("dlclose: no dynamic linker present (statically linked)");
        return -1;
    }
    int ret = fn(handle);
    if (ret != 0) {
        const char *(*ldso_dlerror)(void) =
            (const char *(*)(void))__libc_ldso_lookup("__ldso_dlerror");
        const char *msg = ldso_dlerror ? ldso_dlerror() : NULL;
        set_local_dlerror(msg ? msg : "dlclose: failed");
    }
    return ret;
}

char *dlerror(void)
{
    /* POSIX: returns NULL if called twice in a row with no intervening
     * failure — the same one-shot contract ld.so's own __ldso_dlerror()
     * already implements; we mirror it locally for the "no ld.so present"
     * messages set directly in this file. */
    const char *msg = g_dlerror_msg;
    g_dlerror_msg = NULL;
    return (char *)msg;
}
