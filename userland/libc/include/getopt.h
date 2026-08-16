/* ============================================================================
 * AzamiOS Userspace — POSIX getopt Header
 * File: userland/libc/include/getopt.h
 * ============================================================================ */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

extern char *optarg;
extern int   optind;
extern int   opterr;
extern int   optopt;

int getopt(int argc, char * const argv[], const char *optstring);

#ifdef __cplusplus
}
#endif
