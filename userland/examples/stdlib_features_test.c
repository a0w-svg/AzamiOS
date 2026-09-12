/* ============================================================================
 * AzamiOS — Standard Library Extended Features Regression Test
 * File: userland/examples/stdlib_features_test.c
 * ============================================================================ */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <malloc.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("[PASS] %s\n", msg); \
    else { printf("[FAIL] %s (errno=%d)\n", msg, errno); g_fail++; } \
} while (0)

static void test_radix64(void)
{
    printf("--- Radix-64 (l64a / a64l) ---\n");
    CHECK(strcmp(l64a(0), "") == 0, "l64a(0) is empty string");
    CHECK(a64l("") == 0L, "a64l(\"\") is 0");

    long test_vals[] = { 1L, 63L, 64L, 123456L, 0x12345678L, -1L, 0x7FFFFFFFL };
    int all_ok = 1;
    for (size_t i = 0; i < sizeof(test_vals) / sizeof(test_vals[0]); i++) {
        long v = test_vals[i];
        char *encoded = l64a(v);
        long decoded = a64l(encoded);
        if ((int32_t)v != (int32_t)decoded) {
            all_ok = 0;
            printf("  mismatch for 0x%lx: encoded '%s', decoded 0x%lx\n", v, encoded, decoded);
        }
    }
    CHECK(all_ok, "l64a/a64l roundtrip preserved 32-bit values");
}

static void test_user_interaction(void)
{
    printf("--- rpmatch ---\n");
    CHECK(rpmatch("yes") == 1, "rpmatch('yes') == 1");
    CHECK(rpmatch("Y") == 1, "rpmatch('Y') == 1");
    CHECK(rpmatch("no") == 0, "rpmatch('no') == 0");
    CHECK(rpmatch("N") == 0, "rpmatch('N') == 0");
    CHECK(rpmatch("maybe") == -1, "rpmatch('maybe') == -1");
    CHECK(rpmatch("") == -1, "rpmatch('') == -1");
    CHECK(rpmatch(NULL) == -1, "rpmatch(NULL) == -1");
}

static void test_multibyte(void)
{
    printf("--- mblen ---\n");
    CHECK(mblen(NULL, 0) == 0, "mblen(NULL, 0) == 0 (stateless)");
    CHECK(mblen("", 1) == 0, "mblen('', 1) == 0 (null terminator)");

    /* 1-byte ASCII */
    CHECK(mblen("A", 1) == 1, "mblen('A', 1) == 1");

    /* 2-byte UTF-8: £ (\xc2\xa3) */
    const char *two_byte = "\xc2\xa3";
    CHECK(mblen(two_byte, 2) == 2, "mblen(2-byte utf-8, 2) == 2");
    CHECK(mblen(two_byte, 1) == -1, "mblen(2-byte utf-8, 1) == -1 (truncated)");

    /* 3-byte UTF-8: € (\xe2\x82\xac) */
    const char *three_byte = "\xe2\x82\xac";
    CHECK(mblen(three_byte, 3) == 3, "mblen(3-byte utf-8, 3) == 3");
    CHECK(mblen(three_byte, 2) == -1, "mblen(3-byte utf-8, 2) == -1 (truncated)");

    /* 4-byte UTF-8: 🌍 (\xf0\x9f\x8c\x8d) */
    const char *four_byte = "\xf0\x9f\x8c\x8d";
    CHECK(mblen(four_byte, 4) == 4, "mblen(4-byte utf-8, 4) == 4");
    CHECK(mblen(four_byte, 3) == -1, "mblen(4-byte utf-8, 3) == -1 (truncated)");

    /* Invalid leading byte */
    CHECK(mblen("\xff", 1) == -1, "mblen(invalid utf-8, 1) == -1");
}

static void test_malloc_usable_size(void)
{
    printf("--- malloc_usable_size ---\n");
    CHECK(malloc_usable_size(NULL) == 0, "malloc_usable_size(NULL) == 0");

    void *p1 = malloc(32);
    CHECK(p1 != NULL, "malloc(32)");
    if (p1) {
        CHECK(malloc_usable_size(p1) >= 32, "malloc_usable_size(p1) >= 32");
        free(p1);
    }

    void *p2 = aligned_alloc(64, 128);
    CHECK(p2 != NULL, "aligned_alloc(64, 128)");
    if (p2) {
        CHECK(malloc_usable_size(p2) >= 128, "malloc_usable_size(aligned) >= 128");
        free(p2);
    }
}

static void test_intmax_math(void)
{
    printf("--- intmax_t math & conversion ---\n");
    CHECK(imaxabs(-9876543210LL) == 9876543210LL, "imaxabs(-9876543210LL)");
    CHECK(imaxabs(9876543210LL) == 9876543210LL, "imaxabs(9876543210LL)");

    imaxdiv_t dv = imaxdiv(100LL, 7LL);
    CHECK(dv.quot == 14LL && dv.rem == 2LL, "imaxdiv(100, 7) -> 14 rem 2");

    char *end = NULL;
    intmax_t sm = strtoimax("-123456789012345", &end, 10);
    CHECK(sm == -123456789012345LL && end && *end == '\0', "strtoimax correctly parses negative 64-bit int");

    uintmax_t um = strtoumax("0xDEADBEEFCAFE", &end, 16);
    CHECK(um == 0xDEADBEEFCAFEULL && end && *end == '\0', "strtoumax parses hex integer");
}

static void test_reentrant_random(void)
{
    printf("--- drand48_r / state ---\n");
    struct drand48_data buf1, buf2;
    memset(&buf1, 0, sizeof(buf1));
    memset(&buf2, 0, sizeof(buf2));

    srand48_r(54321, &buf1);
    srand48_r(54321, &buf2);

    double d1 = 0.0, d2 = 0.0;
    drand48_r(&buf1, &d1);
    drand48_r(&buf2, &d2);
    CHECK(d1 == d2 && d1 >= 0.0 && d1 < 1.0, "drand48_r repeatability and range [0, 1)");

    long l1 = 0, l2 = 0;
    lrand48_r(&buf1, &l1);
    lrand48_r(&buf2, &l2);
    CHECK(l1 == l2 && l1 >= 0, "lrand48_r repeatability and non-negative");

    char state_arr[64];
    char *old_state = initstate(1234, state_arr, sizeof(state_arr));
    CHECK(old_state != NULL, "initstate succeeds");
    long r1 = random();
    setstate(old_state);
    CHECK(r1 >= 0, "random() with customized state");
}

static void test_temporary_files(void)
{
    printf("--- mkostemp / mkstemps / mkostemps ---\n");
    char tmpl1[] = "/tmp/test_ostemp_XXXXXX";
    int fd1 = mkostemp(tmpl1, O_CLOEXEC);
    CHECK(fd1 >= 0, "mkostemp succeeds");
    if (fd1 >= 0) {
        close(fd1);
        unlink(tmpl1);
    }

    char tmpl2[] = "/tmp/test_stemps_XXXXXX.tmp";
    int fd2 = mkstemps(tmpl2, 4);
    CHECK(fd2 >= 0, "mkstemps succeeds with .tmp suffix");
    if (fd2 >= 0) {
        size_t l = strlen(tmpl2);
        CHECK(l > 4 && strcmp(tmpl2 + l - 4, ".tmp") == 0, "mkstemps preserved suffix");
        close(fd2);
        unlink(tmpl2);
    }

    char tmpl3[] = "/tmp/test_ostemps_XXXXXX.log";
    int fd3 = mkostemps(tmpl3, 4, O_CLOEXEC);
    CHECK(fd3 >= 0, "mkostemps succeeds with suffix and flags");
    if (fd3 >= 0) {
        close(fd3);
        unlink(tmpl3);
    }
}

static void test_realpath_and_canonicalize(void)
{
    printf("--- realpath & canonicalize_file_name ---\n");
    CHECK(realpath(NULL, NULL) == NULL && errno == ENOENT, "realpath(NULL) returns ENOENT");
    CHECK(realpath("", NULL) == NULL && errno == ENOENT, "realpath('') returns ENOENT");

    char res[4096];
    char *r = realpath("/", res);
    CHECK(r != NULL && strcmp(res, "/") == 0, "realpath('/') -> '/'");

    char *c = canonicalize_file_name("/bin/../bin/.");
    CHECK(c != NULL, "canonicalize_file_name('/bin/../bin/.') succeeds");
    if (c) {
        CHECK(strcmp(c, "/bin") == 0 || strcmp(c, "/") == 0, "canonicalize_file_name resolved . and ..");
        free(c);
    }
}

static void test_secure_getenv(void)
{
    printf("--- secure_getenv ---\n");
    char *p = secure_getenv("PATH");
    CHECK(p != NULL, "secure_getenv('PATH') returns value in normal unprivileged process");
    CHECK(secure_getenv("THIS_ENV_DOES_NOT_EXIST_XYZ123") == NULL, "secure_getenv misses absent variable");
}

static int s_quick_exit_hook_ran = 0;
static void on_quick_exit_hook(void)
{
    s_quick_exit_hook_ran = 1;
}

static int s_on_exit_hook_status = 0;
static void *s_on_exit_hook_arg = NULL;
static void on_exit_hook(int status, void *arg)
{
    s_on_exit_hook_status = status;
    s_on_exit_hook_arg = arg;
}

static void test_exit_lifecycle(void)
{
    printf("--- exit & quick_exit lifecycle ---\n");
    CHECK(at_quick_exit(on_quick_exit_hook) == 0, "at_quick_exit registration succeeds");
    CHECK(on_exit(on_exit_hook, (void *)0x1337) == 0, "on_exit registration succeeds");

    /* Test quick_exit in child process */
    pid_t pid = fork();
    if (pid == 0) {
        quick_exit(42);
    } else if (pid > 0) {
        int st = 0;
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42, "quick_exit(42) terminates with code 42");
    }

    /* Test on_exit in child process */
    pid = fork();
    if (pid == 0) {
        exit(17);
    } else if (pid > 0) {
        int st = 0;
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 17, "exit(17) with on_exit terminates with code 17");
    }
}

int main(void)
{
    printf("=== AzamiOS Extended stdlib.c / stdlib.h Regression Test ===\n");
    test_radix64();
    test_user_interaction();
    test_multibyte();
    test_malloc_usable_size();
    test_intmax_math();
    test_reentrant_random();
    test_temporary_files();
    test_realpath_and_canonicalize();
    test_secure_getenv();
    test_exit_lifecycle();

    printf("=== Results: %d failure(s) ===\n", g_fail);
    return g_fail ? 1 : 0;
}
