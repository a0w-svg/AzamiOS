/* ============================================================================
 * AzamiOS — Intrusive Red-Black Tree
 * File: kernel/lib/rbtree.h
 *
 * A Linux-compliant intrusive Red-Black tree for O(log N) lookups.
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../include/azami/defs.h"

#define RB_RED   0
#define RB_BLACK 1

struct rb_node {
    struct rb_node *rb_parent;
    struct rb_node *rb_left;
    struct rb_node *rb_right;
    int rb_color;
};

struct rb_root {
    struct rb_node *rb_node;
};

#define RB_ROOT (struct rb_root) { NULL }

/* Initialize a node to be linked */
static inline void rb_link_node(struct rb_node *node, struct rb_node *parent, struct rb_node **rb_link)
{
    node->rb_parent = parent;
    node->rb_color = RB_RED;
    node->rb_left = node->rb_right = NULL;
    *rb_link = node;
}

/* Rebalance after insertion */
void rb_insert_color(struct rb_node *node, struct rb_root *root);

/* Erase a node from the tree and rebalance */
void rb_erase(struct rb_node *node, struct rb_root *root);

/* Find the leftmost node (smallest) */
struct rb_node *rb_first(const struct rb_root *root);

/* Find the next node in ascending order */
struct rb_node *rb_next(const struct rb_node *node);

/* Find the previous node in descending order */
struct rb_node *rb_prev(const struct rb_node *node);

#define rb_entry(ptr, type, member) container_of(ptr, type, member)
