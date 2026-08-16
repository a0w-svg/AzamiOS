/* ============================================================================
 * AzamiOS Userspace — Standard Math Implementation (math.c)
 * File: userland/libc/math.c
 * ============================================================================ */

#include "include/math.h"

double fabs(double x)
{
    return (x < 0.0) ? -x : x;
}

double floor(double x)
{
    long long n = (long long)x;
    if (x < 0.0 && x != (double)n) n--;
    return (double)n;
}

double ceil(double x)
{
    long long n = (long long)x;
    if (x > 0.0 && x != (double)n) n++;
    return (double)n;
}

double round(double x)
{
    return (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
}

double fmod(double x, double y)
{
    if (y == 0.0) return NAN;
    long long n = (long long)(x / y);
    return x - (double)n * y;
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

double tan(double x)
{
    double c = cos(x);
    if (fabs(c) < 1e-15) return INFINITY;
    return sin(x) / c;
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

double log(double x)
{
    if (x <= 0.0) return -INFINITY;
    if (x == 1.0) return 0.0;

    /* Range reduction: x = m * 2^k */
    int k = 0;
    while (x > 2.0) { x *= 0.5; k++; }
    while (x < 0.5) { x *= 2.0; k--; }

    /* Series expansion for ln((1+y)/(1-y)) where y = (x-1)/(x+1) */
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

double atan2(double y, double x)
{
    if (x > 0.0) {
        double t = y / x;
        return t / (1.0 + 0.28 * t * t); /* Pade approx */
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
