/**
 * math.h — AzamiOS libc: floating-point mathematics
 *
 * Implemented in pure C / FPU double arithmetic.
 * No libm dependency — compiles -ffreestanding.
 */
#ifndef _MATH_H
#define _MATH_H

/* ── Constants ──────────────────────────────────────────────────── */
#define M_PI    3.14159265358979323846
#define M_E     2.71828182845904523536
#define M_LN2   0.69314718055994530942
#define M_SQRT2 1.41421356237309504880
#define HUGE_VAL __builtin_huge_val()
#define NAN      __builtin_nanf("")
#define INFINITY __builtin_inff()

/* ── Integer helpers ────────────────────────────────────────────── */
int    abs  (int n);
long   labs (long n);

/* ── Rounding ───────────────────────────────────────────────────── */
double floor(double x);
double ceil (double x);
double round(double x);
double trunc(double x);
double fabs (double x);
double fmod (double x, double y);

/* ── Roots & powers ─────────────────────────────────────────────── */
double sqrt(double x);
double pow (double base, double exp);
double exp (double x);
double exp2(double x);

/* ── Logarithms ─────────────────────────────────────────────────── */
double log  (double x);
double log2 (double x);
double log10(double x);

/* ── Trigonometry ───────────────────────────────────────────────── */
double sin (double x);
double cos (double x);
double tan (double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

/* ── Hyperbolic ─────────────────────────────────────────────────── */
double sinh(double x);
double cosh(double x);
double tanh(double x);

/* ── Decomposition ──────────────────────────────────────────────── */
double frexp(double x, int *exp);
double ldexp(double x, int exp);
double modf (double x, double *iptr);

/* ── Classification (inline macros) ───────────────────────────────*/
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define fmin(a,b)   ((a) < (b) ? (a) : (b))
#define fmax(a,b)   ((a) > (b) ? (a) : (b))

#endif /* _MATH_H */

