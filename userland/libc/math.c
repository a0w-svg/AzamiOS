/* ============================================================================
 * AzamiOS Userspace — Standard Math Implementation (math.c)
 * File: userland/libc/math.c
 * ============================================================================ */

#include "include/math.h"

double fabs(double x)
{
    return (x < 0.0) ? -x : x;
}

float fabsf(float x)
{
    return (x < 0.0f) ? -x : x;
}

double floor(double x)
{
    long long n = (long long)x;
    if (x < 0.0 && x != (double)n) n--;
    return (double)n;
}

float floorf(float x)
{
    return (float)floor((double)x);
}

double ceil(double x)
{
    long long n = (long long)x;
    if (x > 0.0 && x != (double)n) n++;
    return (double)n;
}

float ceilf(float x)
{
    return (float)ceil((double)x);
}

double round(double x)
{
    return (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
}

float roundf(float x)
{
    return (float)round((double)x);
}

double trunc(double x)
{
    return (x >= 0.0) ? floor(x) : ceil(x);
}

float truncf(float x)
{
    return (float)trunc((double)x);
}

double fmod(double x, double y)
{
    if (y == 0.0) return NAN;
    long long n = (long long)(x / y);
    return x - (double)n * y;
}

float fmodf(float x, float y)
{
    return (float)fmod((double)x, (double)y);
}

double sqrt(double x)
{
    if (x < 0.0) return NAN;
    if (x == 0.0) return 0.0;

    double guess = x / 2.0;
    if (guess < 1.0) guess = 1.0;

    for (int i = 0; i < 20; i++) {
        guess = 0.5 * (guess + x / guess);
    }
    return guess;
}

float sqrtf(float x)
{
    return (float)sqrt((double)x);
}

double cbrt(double x)
{
    if (x == 0.0) return 0.0;
    double sign = (x < 0.0) ? -1.0 : 1.0;
    double ax = fabs(x);
    double guess = exp(log(ax) / 3.0);
    return sign * guess;
}

double hypot(double x, double y)
{
    return sqrt(x * x + y * y);
}

double sin(double x)
{
    x = fmod(x, 2.0 * M_PI);
    if (x < 0.0) x += 2.0 * M_PI;

    /* Taylor series expansion around 0 */
    double term = x;
    double sum = x;
    for (int i = 1; i <= 9; i++) {
        term *= -x * x / ((2 * i) * (2 * i + 1));
        sum += term;
    }
    return sum;
}

float sinf(float x)
{
    return (float)sin((double)x);
}

double cos(double x)
{
    x = fmod(x, 2.0 * M_PI);
    if (x < 0.0) x += 2.0 * M_PI;

    /* Taylor series expansion around 0 */
    double term = 1.0;
    double sum = 1.0;
    for (int i = 1; i <= 9; i++) {
        term *= -x * x / ((2 * i - 1) * (2 * i));
        sum += term;
    }
    return sum;
}

float cosf(float x)
{
    return (float)cos((double)x);
}

double tan(double x)
{
    double c = cos(x);
    if (fabs(c) < 1e-15) return INFINITY;
    return sin(x) / c;
}

float tanf(float x)
{
    return (float)tan((double)x);
}

double atan(double x)
{
    return atan2(x, 1.0);
}

double asin(double x)
{
    if (x < -1.0 || x > 1.0) return NAN;
    if (x == 1.0) return M_PI_2;
    if (x == -1.0) return -M_PI_2;
    return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x)
{
    if (x < -1.0 || x > 1.0) return NAN;
    return M_PI_2 - asin(x);
}

double atan2(double y, double x)
{
    if (x > 0.0) {
        double t = y / x;
        return t / (1.0 + 0.28 * t * t);
    } else if (x < 0.0 && y >= 0.0) {
        double t = y / x;
        return t / (1.0 + 0.28 * t * t) + M_PI;
    } else if (x < 0.0 && y < 0.0) {
        double t = y / x;
        return t / (1.0 + 0.28 * t * t) - M_PI;
    } else if (x == 0.0 && y > 0.0) {
        return M_PI_2;
    } else if (x == 0.0 && y < 0.0) {
        return -M_PI_2;
    }
    return 0.0;
}

float atan2f(float y, float x)
{
    return (float)atan2((double)y, (double)x);
}

double exp(double x)
{
    if (x < -50.0) return 0.0;
    if (x > 50.0) return INFINITY;

    double term = 1.0;
    double sum = 1.0;
    for (int i = 1; i <= 25; i++) {
        term *= x / (double)i;
        sum += term;
    }
    return sum;
}

float expf(float x)
{
    return (float)exp((double)x);
}

double sinh(double x)
{
    return 0.5 * (exp(x) - exp(-x));
}

double cosh(double x)
{
    return 0.5 * (exp(x) + exp(-x));
}

double tanh(double x)
{
    double ex = exp(x);
    double emx = exp(-x);
    return (ex - emx) / (ex + emx);
}

double log(double x)
{
    if (x <= 0.0) return -INFINITY;
    if (x == 1.0) return 0.0;

    int k = 0;
    while (x > 2.0) { x *= 0.5; k++; }
    while (x < 0.5) { x *= 2.0; k--; }

    double y = (x - 1.0) / (x + 1.0);
    double y2 = y * y;
    double term = y;
    double sum = y;

    for (int i = 3; i <= 19; i += 2) {
        term *= y2;
        sum += term / (double)i;
    }
    return 2.0 * sum + (double)k * M_LN2;
}

float logf(float x)
{
    return (float)log((double)x);
}

double log10(double x)
{
    return log(x) * M_LOG10E;
}

double log2(double x)
{
    return log(x) * M_LOG2E;
}

double pow(double x, double y)
{
    if (y == 0.0) return 1.0;
    if (x == 0.0) return (y > 0.0) ? 0.0 : INFINITY;
    if (x < 0.0) {
        if (fmod(y, 1.0) == 0.0) {
            double res = exp(y * log(-x));
            return ((long long)y % 2 != 0) ? -res : res;
        }
        return NAN;
    }
    return exp(y * log(x));
}

float powf(float x, float y)
{
    return (float)pow((double)x, (double)y);
}

double frexp(double x, int *e)
{
    if (x == 0.0) {
        if (e) *e = 0;
        return 0.0;
    }
    int exp_val = 0;
    double val = x;
    int neg = 0;
    if (val < 0.0) { neg = 1; val = -val; }
    while (val >= 1.0) { val *= 0.5; exp_val++; }
    while (val < 0.5)  { val *= 2.0; exp_val--; }
    if (e) *e = exp_val;
    return neg ? -val : val;
}

double ldexp(double x, int e)
{
    double factor = 1.0;
    if (e > 0) {
        while (e--) factor *= 2.0;
    } else if (e < 0) {
        while (e++) factor *= 0.5;
    }
    return x * factor;
}

double modf(double x, double *iptr)
{
    long long n = (long long)x;
    if (iptr) *iptr = (double)n;
    return x - (double)n;
}
