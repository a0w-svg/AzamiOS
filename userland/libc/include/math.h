/* ============================================================================
 * AzamiOS Userspace — Standard Math Functions (math.h)
 * File: userland/libc/include/math.h
 * ============================================================================ */
#pragma once

#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_E        2.71828182845904523536
#define M_LOG2E    1.44269504088896340736
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

#define INFINITY   (__builtin_inff())
#define NAN        (__builtin_nanf(""))
#define HUGE_VAL   ((double)INFINITY)
#define HUGE_VALF  (INFINITY)

double fabs(double x);
double floor(double x);
double ceil(double x);
double round(double x);
double trunc(double x);
double sqrt(double x);
double cbrt(double x);
double hypot(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double pow(double x, double y);
double exp(double x);
double log(double x);
double log10(double x);
double log2(double x);
double fmod(double x, double y);
double frexp(double x, int *exp);
double ldexp(double x, int exp);
double modf(double x, double *iptr);

/* C99 additions, previously undeclared here. Not IEEE-754-exact (this
 * whole file is naive Taylor-series/Newton's-method arithmetic, not a
 * fdlibm-derived implementation) — usable, not bit-for-bit-correct. */
double asinh(double x);
double acosh(double x);
double atanh(double x);
double log1p(double x);
double expm1(double x);
double copysign(double x, double y);
double nextafter(double x, double y);
double scalbn(double x, int n);
double scalbln(double x, long n);
double remainder(double x, double y);
double remquo(double x, double y, int *quo);
double fma(double x, double y, double z);
float  fmaf(float x, float y, float z);

/* isnan/isinf/isfinite/signbit/fpclassify are C99 macros, not functions —
 * GCC's type-generic __builtin_* forms need no libm and work for float,
 * double, and long double alike. */
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, (x))
#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)
#define signbit(x)    __builtin_signbit(x)

/* Single-precision float variants */
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float roundf(float x);
float truncf(float x);
float sqrtf(float x);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float atan2f(float y, float x);
float powf(float x, float y);
float expf(float x);
float logf(float x);
float fmodf(float x, float y);
