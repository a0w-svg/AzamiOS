/* ============================================================================
 * AzamiOS Userspace — Standard POSIX Implementation Limits (limits.h)
 * File: userland/libc/include/limits.h
 * ============================================================================ */
#pragma once

/* Basic Types Numerical Limits */
#define CHAR_BIT      8
#define SCHAR_MIN   (-128)
#define SCHAR_MAX     127
#define UCHAR_MAX     255

#ifdef __CHAR_UNSIGNED__
#define CHAR_MIN      0
#define CHAR_MAX      UCHAR_MAX
#else
#define CHAR_MIN      SCHAR_MIN
#define CHAR_MAX      SCHAR_MAX
#endif

#define MB_LEN_MAX    16

#define SHRT_MIN    (-32768)
#define SHRT_MAX      32767
#define USHRT_MAX     65535

#define INT_MIN     (-2147483647 - 1)
#define INT_MAX       2147483647
#define UINT_MAX      4294967295U

#define LONG_MIN    (-9223372036854775807L - 1L)
#define LONG_MAX      9223372036854775807L
#define ULONG_MAX     18446744073709551615UL

#define LLONG_MIN   (-9223372036854775807LL - 1LL)
#define LLONG_MAX     9223372036854775807LL
#define ULLONG_MAX    18446744073709551615ULL

/* POSIX File and Path Limits */
#define PATH_MAX      4096
#define NAME_MAX      255
#define OPEN_MAX      64
#define PIPE_BUF      4096
#define IOV_MAX       1024
#define LINE_MAX      2048
#define ARG_MAX       131072
#define HOST_NAME_MAX 64
#define LOGIN_NAME_MAX 32
#define TTY_NAME_MAX  32
#define NGROUPS_MAX   32

/* POSIX Thread Limits */
#define PTHREAD_KEYS_MAX       64
#define PTHREAD_STACK_MIN      4096
#define PTHREAD_DESTRUCTOR_ITERATIONS 4
