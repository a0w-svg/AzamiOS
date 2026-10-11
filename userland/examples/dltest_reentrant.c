/* ============================================================================
 * AzamiOS — Dynamic Linking Reentrant Constructor Fixture
 * File: userland/examples/dltest_reentrant.c
 * ============================================================================ */

#include <dlfcn.h>
#include <stddef.h>

extern void dltest_event_record(int id);

#define EVENT_REENTRANT_START 401
#define EVENT_REENTRANT_DONE  402

static int s_reentrant_val = 0;

__attribute__((constructor))
void dltest_reentrant_ctor(void)
{
    dltest_event_record(EVENT_REENTRANT_START);

    /* Reentrant dlopen call from inside a constructor */
    void *h = dlopen("/lib/dltest_lib.so", RTLD_NOW);
    if (h) {
        int (*add)(int, int) = (int (*)(int, int))dlsym(h, "dltest_add");
        if (add) {
            s_reentrant_val = add(20, 22); /* 42 */
        }
        dlclose(h);
    }

    dltest_event_record(EVENT_REENTRANT_DONE);
}

int dltest_reentrant_get(void)
{
    return s_reentrant_val;
}
