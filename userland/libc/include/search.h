/* ============================================================================
 * AzamiOS Userspace — Search Tables (search.h)
 * File: userland/libc/include/search.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"

typedef enum {
    FIND,
    ENTER
} ACTION;

typedef struct entry {
    char *key;
    void *data;
} ENTRY;

typedef enum {
    preorder,
    postorder,
    endorder,
    leaf
} VISIT;

/* Hash search */
int   hcreate(size_t nel);
void  hdestroy(void);
ENTRY *hsearch(ENTRY item, ACTION action);

/* Binary tree search */
void *tsearch(const void *key, void **rootp, int (*compar)(const void *, const void *));
void *tfind(const void *key, void *const *rootp, int (*compar)(const void *, const void *));
void *tdelete(const void *key, void **rootp, int (*compar)(const void *, const void *));
void  twalk(const void *root, void (*action)(const void *, VISIT, int));

/* Linear search */
void *lfind(const void *key, const void *base, size_t *nelp, size_t width,
            int (*compar)(const void *, const void *));
void *lsearch(const void *key, void *base, size_t *nelp, size_t width,
              int (*compar)(const void *, const void *));
