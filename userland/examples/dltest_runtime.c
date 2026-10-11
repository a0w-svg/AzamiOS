/* ============================================================================
 * AzamiOS — Native Dynamic Loader Comprehensive Test Suite
 * File: userland/examples/dltest_runtime.c
 *
 * Exercises runtime dynamic loading behaviors:
 *   1. Repeated dependency loads & refcounts
 *   2. Constructor reentrancy (ctors calling dlopen/dlsym/dlclose)
 *   3. Destructor order (DT_FINI_ARRAY reverse order & reverse dependencies)
 *   4. Transitive symbol scope in dlsym()
 *   5. dladdr() resolution
 *   6. Transactional rollback of newly mapped objects on failed load
 *   7. Thread-local dlerror isolation across threads
 *   8. Concurrent multi-threaded load/unload stress
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

static int g_pass_count = 0;
static int g_fail_count = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        g_pass_count++; \
    } else { \
        printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
        g_fail_count++; \
    } \
} while (0)

/* Interface to dltest_event.so */
typedef void (*event_clear_fn)(void);
typedef int  (*event_count_fn)(void);
typedef int  (*event_get_fn)(int);

static event_clear_fn ev_clear = NULL;
static event_count_fn ev_count = NULL;
static event_get_fn   ev_get   = NULL;

static void test_1_and_2_deps_refcounts_and_dtor_order(void)
{
    printf("\n--- Test 1 & 2: Repeated Loads, Refcounts, & Dependency Dtor Order ---\n");

    ev_clear();

    /* Load top library (which pulls in dep library) */
    void *h1 = dlopen("/lib/dltest_top.so", RTLD_NOW);
    TEST_ASSERT(h1 != NULL, "dlopen(/lib/dltest_top.so) succeeded (h1)");
    if (!h1) return;

    /* Verify ctors ran in dependency-first order: DEP_CTOR (101) then TOP_CTOR (102) */
    TEST_ASSERT(ev_count() == 2, "2 constructor events recorded");
    TEST_ASSERT(ev_get(0) == 101, "Dependency ctor ran first (101)");
    TEST_ASSERT(ev_get(1) == 102, "Top ctor ran second (102)");

    int (*top_calc)(int) = (int (*)(int))dlsym(h1, "dltest_top_calc");
    TEST_ASSERT(top_calc != NULL, "dlsym(h1, dltest_top_calc) resolved");
    if (top_calc) {
        int val = top_calc(5);
        TEST_ASSERT(val == 1005, "dltest_top_calc(5) returned 1005 (dep 1000 + 5)");
    }

    /* Repeated load: must return same handle and increment refcount */
    void *h2 = dlopen("/lib/dltest_top.so", RTLD_NOW);
    TEST_ASSERT(h2 == h1, "Repeated dlopen returns existing handle");
    TEST_ASSERT(ev_count() == 2, "No extra constructor calls on repeated load");

    ev_clear();

    /* First dlclose: refcount 2 -> 1. Object remains mapped, NO destructors run yet */
    int rc1 = dlclose(h1);
    TEST_ASSERT(rc1 == 0, "First dlclose(h1) returned 0");
    TEST_ASSERT(ev_count() == 0, "No destructors run while refcount > 0");

    /* Function still callable through h2 */
    if (top_calc) {
        int val = top_calc(10);
        TEST_ASSERT(val == 1010, "dltest_top_calc(10) still callable via h2");
    }

    /* Second dlclose: refcount 1 -> 0. Destructors run in reverse dependency order */
    int rc2 = dlclose(h2);
    TEST_ASSERT(rc2 == 0, "Second dlclose(h2) returned 0");
    TEST_ASSERT(ev_count() == 2, "2 destructor events recorded on final unload");
    TEST_ASSERT(ev_get(0) == 202, "Top dtor ran first (202)");
    TEST_ASSERT(ev_get(1) == 201, "Dep dtor ran second (201)");
}

static void test_3_dtor_array_order(void)
{
    printf("\n--- Test 3: Destructor Array Reverse Order ---\n");

    ev_clear();

    void *h = dlopen("/lib/dltest_dtor_order.so", RTLD_NOW);
    TEST_ASSERT(h != NULL, "dlopen(/lib/dltest_dtor_order.so) succeeded");
    if (!h) return;

    int rc = dlclose(h);
    TEST_ASSERT(rc == 0, "dlclose(/lib/dltest_dtor_order.so) succeeded");

    /* DT_FINI_ARRAY must execute in reverse order: third (303), second (302), first (301) */
    TEST_ASSERT(ev_count() == 3, "3 destructors recorded");
    TEST_ASSERT(ev_get(0) == 303, "dtor_third executed first (303)");
    TEST_ASSERT(ev_get(1) == 302, "dtor_second executed second (302)");
    TEST_ASSERT(ev_get(2) == 301, "dtor_first executed third (301)");
}

static void test_4_constructor_reentrancy(void)
{
    printf("\n--- Test 4: Constructor Reentrancy ---\n");

    ev_clear();

    /* dltest_reentrant.so's ctor calls dlopen(/lib/dltest_lib.so) and dlsym while holding loader_lock */
    void *h = dlopen("/lib/dltest_reentrant.so", RTLD_NOW);
    TEST_ASSERT(h != NULL, "dlopen(/lib/dltest_reentrant.so) succeeded without deadlock");
    if (!h) return;

    TEST_ASSERT(ev_count() == 2, "Reentrant ctor start and finish recorded");
    TEST_ASSERT(ev_get(0) == 401, "Reentrant ctor started (401)");
    TEST_ASSERT(ev_get(1) == 402, "Reentrant ctor completed (402)");

    int (*get_fn)(void) = (int (*)(void))dlsym(h, "dltest_reentrant_get");
    TEST_ASSERT(get_fn != NULL, "dlsym(dltest_reentrant_get) resolved");
    if (get_fn) {
        int r = get_fn();
        TEST_ASSERT(r == 42, "Reentrant nested dlopen calculation returned 42");
    }

    int rc = dlclose(h);
    TEST_ASSERT(rc == 0, "dlclose(/lib/dltest_reentrant.so) succeeded");
}

static void test_5_transitive_scope_and_dladdr(void)
{
    printf("\n--- Test 5: Transitive Symbol Scope & dladdr() ---\n");

    /* Chain: A -> B -> C */
    void *h = dlopen("/lib/dltest_chain_a.so", RTLD_NOW);
    TEST_ASSERT(h != NULL, "dlopen(/lib/dltest_chain_a.so) succeeded");
    if (!h) return;

    /* Direct symbol in A */
    int (*a_fn)(void) = (int (*)(void))dlsym(h, "chain_a_val");
    TEST_ASSERT(a_fn != NULL, "dlsym(chain_a_val) found direct symbol in A");
    if (a_fn) {
        TEST_ASSERT(a_fn() == 555, "chain_a_val() returned 555");
    }

    /* Transitive symbol in C (A does not directly depend on C; A -> B -> C) */
    int (*c_fn)(void) = (int (*)(void))dlsym(h, "chain_c_val");
    TEST_ASSERT(c_fn != NULL, "dlsym(chain_c_val) found transitive symbol across 2 hops (A->B->C)");
    if (c_fn) {
        TEST_ASSERT(c_fn() == 500, "chain_c_val() returned 500");
    }

    /* dladdr on c_fn */
    if (c_fn) {
        Dl_info info;
        memset(&info, 0, sizeof(info));
        int ret = dladdr((const void *)(uintptr_t)c_fn, &info);
        TEST_ASSERT(ret != 0, "dladdr(chain_c_val) returned success");
        TEST_ASSERT(info.dli_sname != NULL && strcmp(info.dli_sname, "chain_c_val") == 0,
                    "info.dli_sname matched 'chain_c_val'");
        TEST_ASSERT(info.dli_saddr == (void *)(uintptr_t)c_fn,
                    "info.dli_saddr matched function pointer");
        TEST_ASSERT(info.dli_fname != NULL && strstr(info.dli_fname, "dltest_chain_c") != NULL,
                    "info.dli_fname contains 'dltest_chain_c'");
        TEST_ASSERT(info.dli_fbase != NULL, "info.dli_fbase is non-NULL");
    }

    /* dladdr on main function */
    {
        Dl_info info_main;
        memset(&info_main, 0, sizeof(info_main));
        int ret_m = dladdr((const void *)(uintptr_t)test_5_transitive_scope_and_dladdr, &info_main);
        TEST_ASSERT(ret_m != 0, "dladdr on executable code returned success");
        TEST_ASSERT(info_main.dli_fbase != NULL, "info_main.dli_fbase is non-NULL");
    }

    dlclose(h);
}

static void test_6_transactional_rollback(void)
{
    printf("\n--- Test 6: Transactional Rollback on Failed Loads ---\n");

    /* Attempt 1: Shared object with missing required symbol */
    void *h_bad1 = dlopen("/lib/dltest_unresolved.so", RTLD_NOW);
    TEST_ASSERT(h_bad1 == NULL, "dlopen(dltest_unresolved.so) failed cleanly");
    char *err1 = dlerror();
    TEST_ASSERT(err1 != NULL && strstr(err1, "unresolved") != NULL,
                "dlerror() reported unresolved symbol error");

    /* Attempt 2: Shared object with PT_TLS segment (unsupported at runtime) */
    void *h_bad2 = dlopen("/lib/dltest_tls_reject.so", RTLD_NOW);
    TEST_ASSERT(h_bad2 == NULL, "dlopen(dltest_tls_reject.so) rejected cleanly");
    char *err2 = dlerror();
    TEST_ASSERT(err2 != NULL && strstr(err2, "PT_TLS") != NULL,
                "dlerror() reported PT_TLS rejection error");

    /* Verification: Next valid load succeeds immediately without slot leakage or corruption */
    void *h_good = dlopen("/lib/dltest_lib.so", RTLD_NOW);
    TEST_ASSERT(h_good != NULL, "Subsequent valid dlopen succeeded after rollbacks");
    if (h_good) {
        int (*add_fn)(int, int) = (int (*)(int, int))dlsym(h_good, "dltest_add");
        TEST_ASSERT(add_fn != NULL && add_fn(2, 3) == 5, "dltest_add(2,3) = 5");
        dlclose(h_good);
    }
}

/* Data for thread-local dlerror test */
static volatile int g_t1_done = 0;
static char g_t1_err_copy[128];

static void *t1_func(void *arg)
{
    (void)arg;
    void *h = dlopen("/nonexistent_library_t1.so", RTLD_NOW);
    (void)h;
    char *err = dlerror();
    if (err) {
        strncpy(g_t1_err_copy, err, sizeof(g_t1_err_copy) - 1);
    } else {
        g_t1_err_copy[0] = '\0';
    }
    g_t1_done = 1;
    return NULL;
}

static void test_7_thread_local_dlerror(void)
{
    printf("\n--- Test 7: Thread-Local dlerror Isolation ---\n");

    pthread_t th;
    g_t1_done = 0;
    g_t1_err_copy[0] = '\0';

    int rc = pthread_create(&th, NULL, t1_func, NULL);
    TEST_ASSERT(rc == 0, "pthread_create for dlerror test succeeded");

    /* Main thread performs a SUCCESSFUL dlopen */
    void *h_main = dlopen("/lib/dltest_lib.so", RTLD_NOW);
    TEST_ASSERT(h_main != NULL, "Main thread dlopen succeeded");
    char *main_err = dlerror();
    TEST_ASSERT(main_err == NULL, "Main thread dlerror is NULL after success");
    if (h_main) dlclose(h_main);

    pthread_join(th, NULL);

    /* Child thread must have captured its own dlerror */
    TEST_ASSERT(g_t1_done == 1, "Child thread finished");
    TEST_ASSERT(strlen(g_t1_err_copy) > 0, "Child thread observed non-NULL dlerror");
    TEST_ASSERT(strstr(g_t1_err_copy, "cannot open") != NULL,
                "Child thread observed 'cannot open' error");
}

/* Concurrent stress test */
#define NUM_STRESS_THREADS 4
#define STRESS_ITERS 20

static volatile int g_stress_errors = 0;

static void *stress_worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < STRESS_ITERS; i++) {
        void *h = dlopen("/lib/dltest_lib.so", RTLD_NOW);
        if (!h) {
            printf("Thread %d iter %d: dlopen failed: %s\n", id, i, dlerror());
            __sync_fetch_and_add(&g_stress_errors, 1);
            continue;
        }
        int (*add)(int, int) = (int (*)(int, int))dlsym(h, "dltest_add");
        if (!add || add(i, id) != (i + id)) {
            printf("Thread %d iter %d: add calculation error\n", id, i);
            __sync_fetch_and_add(&g_stress_errors, 1);
        }
        if (dlclose(h) != 0) {
            printf("Thread %d iter %d: dlclose failed\n", id, i);
            __sync_fetch_and_add(&g_stress_errors, 1);
        }
    }
    return NULL;
}

static void test_8_concurrent_stress(void)
{
    printf("\n--- Test 8: Concurrent Multi-Threaded Load/Unload Stress ---\n");

    pthread_t threads[NUM_STRESS_THREADS];
    g_stress_errors = 0;

    for (int i = 0; i < NUM_STRESS_THREADS; i++) {
        pthread_create(&threads[i], NULL, stress_worker, (void *)(intptr_t)i);
    }
    for (int i = 0; i < NUM_STRESS_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    TEST_ASSERT(g_stress_errors == 0, "All 4 threads completed 20 load/unload cycles without errors (80 total)");
}

int main(void)
{
    printf("=================================================================\n");
    printf("     AzamiOS Native Dynamic Loader Runtime Test Suite\n");
    printf("=================================================================\n");

    /* Setup event sink */
    void *h_event = dlopen("/lib/libdltest_event.so", RTLD_NOW);
    if (!h_event) {
        printf("FATAL: cannot load /lib/libdltest_event.so: %s\n", dlerror());
        return 1;
    }
    ev_clear = (event_clear_fn)dlsym(h_event, "dltest_event_clear");
    ev_count = (event_count_fn)dlsym(h_event, "dltest_event_count");
    ev_get   = (event_get_fn)dlsym(h_event, "dltest_event_get");

    if (!ev_clear || !ev_count || !ev_get) {
        printf("FATAL: cannot resolve event functions from libdltest_event.so\n");
        return 1;
    }

    test_1_and_2_deps_refcounts_and_dtor_order();
    test_3_dtor_array_order();
    test_4_constructor_reentrancy();
    test_5_transitive_scope_and_dladdr();
    test_6_transactional_rollback();
    test_7_thread_local_dlerror();
    test_8_concurrent_stress();

    dlclose(h_event);

    printf("\n=================================================================\n");
    printf("Summary: %d passed, %d failed\n", g_pass_count, g_fail_count);
    if (g_fail_count == 0) {
        printf("ALL TESTS PASSED: 100%% dynamic loader fidelity achieved!\n");
    }
    printf("=================================================================\n");

    return (g_fail_count == 0) ? 0 : 1;
}
