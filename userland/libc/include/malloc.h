/* ============================================================================
 * AzamiOS Userspace — Memory Allocation Header (malloc.h)
 * File: userland/libc/include/malloc.h
 * ============================================================================ */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void  *malloc(size_t size);
void  *calloc(size_t nmemb, size_t size);
void  *realloc(void *ptr, size_t size);
void   free(void *ptr);
void  *aligned_alloc(size_t alignment, size_t size);
int    posix_memalign(void **memptr, size_t alignment, size_t size);
void  *reallocarray(void *ptr, size_t nmemb, size_t size);
void  *valloc(size_t size);
void  *pvalloc(size_t size);

int    malloc_trim(size_t pad);
size_t malloc_usable_size(void *ptr);

#ifdef __cplusplus
}
#endif

