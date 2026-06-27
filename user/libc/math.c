/**
 * math.c — AzamiOS libc: pure C math implementation
 */
#include "include/math.h"

double fabs(double x) {
    return (x < 0.0) ? -x : x;
}

double floor(double x) {
    long long i = (long long)x;
    if (x < 0.0 && x != (double)i) return (double)(i - 1);
    return (double)i;
}

double ceil(double x) {
    long long i = (long long)x;
    if (x > 0.0 && x != (double)i) return (double)(i + 1);
    return (double)i;
}

double round(double x) {
    return floor(x + 0.5);
}

double trunc(double x) {
    return (double)((long long)x);
}

double fmod(double x, double y) {
    if (y == 0.0) return NAN;
    double quot = trunc(x / y);
    return x - quot * y;
}

double sqrt(double x) {
    if (x < 0.0) return NAN;
    if (x == 0.0 || isnan(x)) return x;
    double res = x;
    for (int i = 0; i < 20; i++) {
        res = 0.5 * (res + x / res);
    }
    return res;
}

double exp(double x) {
    double sum = 1.0;
    double term = 1.0;
    for (int i = 1; i < 30; i++) {
        term *= x / i;
        sum += term;
        if (fabs(term) < 1e-15) break;
    }
    return sum;
}

double log(double x) {
    if (x <= 0.0) return NAN;
    if (x == 1.0) return 0.0;
    
    /* Simple Taylor approximation around 1 or using Newton-Raphson */
    double y = 0.0;
    while (x > 2.0) { x /= M_E; y += 1.0; }
    while (x < 0.5) { x *= M_E; y -= 1.0; }
    
    double z = (x - 1.0) / (x + 1.0);
    double z2 = z * z;
    double term = z;
    double sum = 0.0;
    for (int i = 1; i < 30; i += 2) {
        sum += term / i;
        term *= z2;
        if (fabs(term) < 1e-15) break;
    }
    return y + 2.0 * sum;
}

double pow(double base, double exp_val) {
    if (exp_val == 0.0) return 1.0;
    if (base == 0.0 && exp_val > 0.0) return 0.0;
    if (base < 0.0 && floor(exp_val) == exp_val) {
        long long e = (long long)exp_val;
        double res = pow(-base, exp_val);
        return (e % 2 == 0) ? res : -res;
    }
    return exp(exp_val * log(base));
}

double sin(double x) {
    x = fmod(x, 2.0 * M_PI);
    if (x > M_PI) x -= 2.0 * M_PI;
    if (x < -M_PI) x += 2.0 * M_PI;
    
    double sum = x;
    double term = x;
    double x2 = x * x;
    for (int i = 1; i < 15; i++) {
        term *= -x2 / ((2 * i) * (2 * i + 1));
        sum += term;
    }
    return sum;
}

double cos(double x) {
    return sin(x + M_PI / 2.0);
}

double tan(double x) {
    double c = cos(x);
    if (c == 0.0) return NAN;
    return sin(x) / c;
}
