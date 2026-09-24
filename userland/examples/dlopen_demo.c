/* ============================================================================
 * AzamiOS — dlopen()/dlsym()/dlclose() end-to-end demo
 * File: userland/examples/dlopen_demo.c
 *
 * Unlike userland/examples/dltest_app.c (which proves ld-azami.so's eager
 * DT_NEEDED loading at process start), this app proves the *runtime*
 * dlopen() path: it is itself dynamically linked against libc.so (a real
 * DT_NEEDED dependency, resolved by ld-azami.so before main() ever runs —
 * see userland/libc/crt0_dyn.asm), and at runtime calls libc's dlopen()
 * (userland/libc/dlfcn.c), which reaches ld-azami.so's real
 * __ldso_dlopen() (userland/ldso/ldso.c) through the hand-resolved lookup
 * in userland/libc/ldso_bridge.c — a load-and-relocate of
 * /lib/dltest_lib.so that happens only now, well after this process's own
 * startup relocations are long done.
 * ============================================================================ */
#include <stdio.h>
#include <dlfcn.h>

typedef int (*dltest_add_fn)(int, int);

int main(void)
{
    printf("dlopen_demo: starting\n");

    void *handle = dlopen("/lib/dltest_lib.so", RTLD_NOW);
    if (!handle) {
        printf("dlopen_demo: FAIL: dlopen() returned NULL: %s\n", dlerror());
        return 1;
    }
    printf("dlopen_demo: dlopen(\"/lib/dltest_lib.so\") succeeded, handle=%p\n", handle);

    dltest_add_fn add = (dltest_add_fn)dlsym(handle, "dltest_add");
    if (!add) {
        printf("dlopen_demo: FAIL: dlsym(\"dltest_add\") returned NULL: %s\n", dlerror());
        dlclose(handle);
        return 1;
    }

    int r = add(19, 23);
    printf("dlopen_demo: dltest_add(19,23) = %d\n", r);

    int rc = dlclose(handle);
    if (rc != 0) {
        printf("dlopen_demo: FAIL: dlclose() returned %d: %s\n", rc, dlerror());
        return 1;
    }

    if (r != 42) {
        printf("dlopen_demo: FAIL: expected 42, got %d\n", r);
        return 1;
    }

    printf("dlopen_demo: PASS\n");
    return 0;
}
