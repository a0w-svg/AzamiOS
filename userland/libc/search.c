/* ============================================================================
 * AzamiOS Userspace — POSIX Search Tables Implementation (search.c)
 * File: userland/libc/search.c
 * ============================================================================ */

#include "include/search.h"
#include "include/stdlib.h"
#include "include/string.h"

/* ── Hash Table ──────────────────────────────────────────────────────────── */

static ENTRY *s_htable = NULL;
static size_t s_htable_size = 0;

static unsigned long hash_string(const char *str)
{
    unsigned long hash = 5381;
    int c;
    while ((c = (unsigned char)*str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

int hcreate(size_t nel)
{
    if (nel == 0) nel = 16;
    s_htable_size = nel;
    s_htable = (ENTRY *)calloc(s_htable_size, sizeof(ENTRY));
    return s_htable ? 1 : 0;
}

void hdestroy(void)
{
    if (s_htable) {
        free(s_htable);
        s_htable = NULL;
        s_htable_size = 0;
    }
}

ENTRY *hsearch(ENTRY item, ACTION action)
{
    if (!s_htable || !item.key) return NULL;
    unsigned long idx = hash_string(item.key) % s_htable_size;
    unsigned long start = idx;

    do {
        if (!s_htable[idx].key) {
            if (action == ENTER) {
                s_htable[idx] = item;
                return &s_htable[idx];
            }
            return NULL;
        }
        if (strcmp(s_htable[idx].key, item.key) == 0) {
            return &s_htable[idx];
        }
        idx = (idx + 1) % s_htable_size;
    } while (idx != start);

    return NULL;
}

/* ── Linear Search ───────────────────────────────────────────────────────── */

void *lfind(const void *key, const void *base, size_t *nelp, size_t width,
            int (*compar)(const void *, const void *))
{
    if (!key || !base || !nelp || width == 0) return NULL;
    const char *p = (const char *)base;
    for (size_t i = 0; i < *nelp; i++) {
        if (compar(key, p + (i * width)) == 0) {
            return (void *)(p + (i * width));
        }
    }
    return NULL;
}

void *lsearch(const void *key, void *base, size_t *nelp, size_t width,
              int (*compar)(const void *, const void *))
{
    void *found = lfind(key, base, nelp, width, compar);
    if (found) return found;

    char *dest = (char *)base + (*nelp * width);
    memcpy(dest, key, width);
    (*nelp)++;
    return dest;
}

/* ── Binary Tree Search ─────────────────────────────────────────────────── */

typedef struct node {
    const void *key;
    struct node *left;
    struct node *right;
} node_t;

void *tsearch(const void *key, void **rootp, int (*compar)(const void *, const void *))
{
    if (!rootp) return NULL;
    node_t **np = (node_t **)rootp;
    while (*np) {
        int cmp = compar(key, (*np)->key);
        if (cmp == 0) return *np;
        if (cmp < 0) np = &((*np)->left);
        else np = &((*np)->right);
    }
    node_t *new_node = (node_t *)malloc(sizeof(node_t));
    if (!new_node) return NULL;
    new_node->key = key;
    new_node->left = NULL;
    new_node->right = NULL;
    *np = new_node;
    return new_node;
}

void *tfind(const void *key, void *const *rootp, int (*compar)(const void *, const void *))
{
    if (!rootp || !*rootp) return NULL;
    node_t *n = *(node_t **)rootp;
    while (n) {
        int cmp = compar(key, n->key);
        if (cmp == 0) return n;
        if (cmp < 0) n = n->left;
        else n = n->right;
    }
    return NULL;
}

void *tdelete(const void *key, void **rootp, int (*compar)(const void *, const void *))
{
    if (!rootp || !*rootp) return NULL;
    node_t **np = (node_t **)rootp;
    while (*np) {
        int cmp = compar(key, (*np)->key);
        if (cmp == 0) {
            node_t *del = *np;
            if (!del->left) {
                *np = del->right;
            } else if (!del->right) {
                *np = del->left;
            } else {
                /* Find successor */
                node_t **succ = &del->right;
                while ((*succ)->left) succ = &((*succ)->left);
                del->key = (*succ)->key;
                del = *succ;
                *succ = (*succ)->right;
            }
            free(del);
            return np;
        }
        if (cmp < 0) np = &((*np)->left);
        else np = &((*np)->right);
    }
    return NULL;
}

static void twalk_recurse(const node_t *root, void (*action)(const void *, VISIT, int), int depth)
{
    if (!root) return;
    if (!root->left && !root->right) {
        action(root, leaf, depth);
        return;
    }
    action(root, preorder, depth);
    twalk_recurse(root->left, action, depth + 1);
    action(root, postorder, depth);
    twalk_recurse(root->right, action, depth + 1);
    action(root, endorder, depth);
}

void twalk(const void *root, void (*action)(const void *, VISIT, int))
{
    if (root && action) {
        twalk_recurse((const node_t *)root, action, 0);
    }
}
