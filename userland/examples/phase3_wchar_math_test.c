/* ============================================================================
 * AzamiOS — Phase 3 wchar.h/math.c completeness regression test
 * File: userland/examples/phase3_wchar_math_test.c
 * ============================================================================ */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("[PASS] %s\n", msg); \
    else { printf("[FAIL] %s\n", msg); g_fail++; } \
} while (0)

#define CHECK_NEAR(got, want, eps, msg) do { \
    double _g = (got), _w = (want), _d = _g - _w; \
    if (_d < 0) _d = -_d; \
    if (_d <= (eps)) printf("[PASS] %s (%.6f)\n", msg, _g); \
    else { printf("[FAIL] %s: got %.6f want %.6f\n", msg, _g, _w); g_fail++; } \
} while (0)

static void test_wchar(void)
{
    printf("--- wchar.h additions ---\n");
    wchar_t buf[32];

    wchar_t hay[] = { 'h', 'e', 'l', 'l', 'o', 0 };
    CHECK(wcschr(hay, 'l') == &hay[2], "wcschr finds first match");
    CHECK(wcschr(hay, 'z') == 0, "wcschr misses absent char");
    CHECK(wcsrchr(hay, 'l') == &hay[3], "wcsrchr finds last match");

    wcscpy(buf, L"foo");
    wcscat(buf, L"bar");
    CHECK(wcscmp(buf, L"foobar") == 0, "wcscat appends correctly");

    wcscpy(buf, L"foo");
    wcsncat(buf, L"barbaz", 3);
    CHECK(wcscmp(buf, L"foobar") == 0, "wcsncat respects the length limit");

    wchar_t data[] = { 1, 2, 3, 4, 5 };
    CHECK(wmemchr(data, 3, 5) == &data[2], "wmemchr finds the target");
    CHECK(wmemchr(data, 9, 5) == 0, "wmemchr misses an absent value");

    wchar_t a[] = { 1, 2, 3 }, b[] = { 1, 2, 3 }, c[] = { 1, 2, 4 };
    CHECK(wmemcmp(a, b, 3) == 0, "wmemcmp: equal arrays compare equal");
    CHECK(wmemcmp(a, c, 3) < 0, "wmemcmp: differing arrays compare correctly");

    wchar_t dst[6] = {0};
    wmemcpy(dst, a, 3);
    CHECK(wmemcmp(dst, a, 3) == 0, "wmemcpy copies correctly");

    wchar_t overlap[6] = { 1, 2, 3, 4, 5, 6 };
    wmemmove(overlap + 1, overlap, 4);
    CHECK(overlap[1] == 1 && overlap[2] == 2 && overlap[3] == 3 && overlap[4] == 4,
          "wmemmove handles overlap correctly");

    wchar_t filled[4];
    wmemset(filled, 'x', 4);
    CHECK(filled[0] == 'x' && filled[3] == 'x', "wmemset fills correctly");

    /* Confirms the link succeeds too — these symbols didn't exist before. */
    printf("[PASS] all nine wchar functions linked and ran\n");
}

static void test_math(void)
{
    printf("--- math.c C99 additions ---\n");

    CHECK_NEAR(asinh(0.0), 0.0, 1e-9, "asinh(0) == 0");
    CHECK_NEAR(asinh(sinh(1.5)), 1.5, 1e-6, "asinh(sinh(x)) round-trips");
    CHECK_NEAR(acosh(1.0), 0.0, 1e-9, "acosh(1) == 0");
    CHECK_NEAR(acosh(cosh(2.0)), 2.0, 1e-6, "acosh(cosh(x)) round-trips");
    CHECK_NEAR(atanh(0.0), 0.0, 1e-9, "atanh(0) == 0");
    CHECK_NEAR(atanh(tanh(0.5)), 0.5, 1e-6, "atanh(tanh(x)) round-trips");

    CHECK_NEAR(log1p(0.0), 0.0, 1e-9, "log1p(0) == 0");
    CHECK_NEAR(log1p(1e-8), 1e-8, 1e-12, "log1p handles a tiny x without cancellation");
    CHECK_NEAR(expm1(0.0), 0.0, 1e-9, "expm1(0) == 0");
    CHECK_NEAR(expm1(1e-8), 1e-8, 1e-12, "expm1 handles a tiny x without cancellation");

    CHECK_NEAR(copysign(3.0, -1.0), -3.0, 1e-9, "copysign takes the sign of its 2nd arg");
    CHECK_NEAR(copysign(-3.0, 1.0), 3.0, 1e-9, "copysign flips a negative magnitude");

    double n = nextafter(1.0, 2.0);
    CHECK(n > 1.0, "nextafter(1.0, 2.0) steps upward");
    double n2 = nextafter(n, 0.0);
    CHECK(n2 == 1.0, "nextafter is reversible by one step");

    CHECK_NEAR(scalbn(1.0, 3), 8.0, 1e-9, "scalbn(1.0, 3) == 8.0");
    CHECK_NEAR(scalbln(3.0, 2), 12.0, 1e-9, "scalbln(3.0, 2) == 12.0");

    CHECK_NEAR(remainder(5.0, 3.0), -1.0, 1e-9, "remainder(5,3) rounds to nearest");
    int quo = 0;
    double r = remquo(5.0, 3.0, &quo);
    CHECK_NEAR(r, -1.0, 1e-9, "remquo's remainder matches remainder()");
    CHECK(quo != 0, "remquo reports a nonzero partial quotient");

    CHECK_NEAR(fma(2.0, 3.0, 1.0), 7.0, 1e-9, "fma(2,3,1) == 7");
    CHECK_NEAR((double)fmaf(2.0f, 3.0f, 1.0f), 7.0, 1e-6, "fmaf(2,3,1) == 7");

    CHECK(!isnan(1.0), "isnan(1.0) is false");
    CHECK(isnan(NAN), "isnan(NAN) is true");
    CHECK(isinf(INFINITY), "isinf(INFINITY) is true");
    CHECK(!isinf(1.0), "isinf(1.0) is false");
    CHECK(isfinite(1.0), "isfinite(1.0) is true");
    CHECK(!isfinite(INFINITY), "isfinite(INFINITY) is false");
    CHECK(signbit(-1.0), "signbit(-1.0) is true");
    CHECK(!signbit(1.0), "signbit(1.0) is false");
    CHECK(fpclassify(0.0) == FP_ZERO, "fpclassify(0.0) == FP_ZERO");
    CHECK(fpclassify(1.0) == FP_NORMAL, "fpclassify(1.0) == FP_NORMAL");
    CHECK(fpclassify(NAN) == FP_NAN, "fpclassify(NAN) == FP_NAN");
}

int main(void)
{
    printf("=== AzamiOS Phase 3 wchar/math test ===\n");
    test_wchar();
    test_math();

    if (g_fail == 0) {
        printf("=== ALL PASS ===\n");
        return 0;
    }
    printf("=== %d FAILURE(S) ===\n", g_fail);
    return 1;
}
