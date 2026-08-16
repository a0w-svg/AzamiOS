/* ============================================================================
 * AzamiOS Userspace — Standard Math Functions (math.h)
 * File: userland/libc/include/math.h
 * ============================================================================ */
#pragma once

#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_E        2.71828182845904523536
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_SQRT2    1.41421356237309504880

#define INFINITY   (__builtin_inff())
#define NAN        (__builtin_nanf(""))

double fabs(double x);
double floor(double x);
double ceil(double x);
double round(double x);
double sqrt(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double pow(double x, double y);
double exp(double x);
double log(double x);
double fmod(double x, double y);
double atan2(double y, double x);
