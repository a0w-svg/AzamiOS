/* ============================================================================
 * AzamiOS Userspace — POSIX String Operations (strings.h)
 * File: userland/libc/include/strings.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"

int   strcasecmp(const char *s1, const char *s2);
int   strncasecmp(const char *s1, const char *s2, size_t n);
int   ffs(int i);
int   ffsl(long i);
int   ffsll(long long i);
void  bzero(void *s, size_t n);
void  bcopy(const void *src, void *dest, size_t n);
int   bcmp(const void *s1, const void *s2, size_t n);
char *index(const char *s, int c);
char *rindex(const char *s, int c);
