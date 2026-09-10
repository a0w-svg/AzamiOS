/* ============================================================================
 * AzamiOS Userspace — NUMA Memory Policy (numaif.h)
 * File: userland/libc/include/numaif.h
 *
 * set_mempolicy(2), get_mempolicy(2), mbind(2), migrate_pages(2) and
 * set_mempolicy_home_node(2) — the kernel interface libnuma is built on.
 *
 * AzamiOS presents a single memory node. That does not make these calls
 * meaningless: a policy is stored and reported back faithfully, so a program
 * that sets one and reads it back sees what it set, and a nodemask naming a
 * node that does not exist is rejected rather than quietly ignored. What it
 * does mean is that no policy can change where a page comes from, because
 * there is only one place it can come from — exactly the situation on a
 * one-node Linux machine.
 * ============================================================================ */
#pragma once

#include "sys/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Policy modes. */
#define MPOL_DEFAULT     0   /* fall back to the system default              */
#define MPOL_PREFERRED   1   /* prefer the named node, fall back if full     */
#define MPOL_BIND        2   /* allocate only from the named nodes           */
#define MPOL_INTERLEAVE  3   /* round-robin across the named nodes           */
#define MPOL_LOCAL       4   /* allocate on the node running the thread      */

/* Mode flags, OR'd into the mode argument. */
#define MPOL_F_STATIC_NODES   (1 << 15)
#define MPOL_F_RELATIVE_NODES (1 << 14)

/* get_mempolicy() flags. */
#define MPOL_F_NODE          (1 << 0)  /* report the node, not the policy    */
#define MPOL_F_ADDR          (1 << 1)  /* query the policy at an address     */
#define MPOL_F_MEMS_ALLOWED  (1 << 2)  /* report the permitted node set      */

/* mbind() flags. */
#define MPOL_MF_STRICT    (1 << 0)  /* fail if a page cannot comply          */
#define MPOL_MF_MOVE      (1 << 1)  /* move pages this process owns          */
#define MPOL_MF_MOVE_ALL  (1 << 2)  /* move shared pages too (privileged)    */

int set_mempolicy(int mode, const unsigned long *nodemask,
                  unsigned long maxnode);
int get_mempolicy(int *mode, unsigned long *nodemask, unsigned long maxnode,
                  void *addr, unsigned long flags);
int mbind(void *addr, unsigned long len, int mode,
          const unsigned long *nodemask, unsigned long maxnode,
          unsigned int flags);
long migrate_pages(int pid, unsigned long maxnode,
                   const unsigned long *old_nodes,
                   const unsigned long *new_nodes);
int set_mempolicy_home_node(void *start, unsigned long len,
                            unsigned long home_node, unsigned long flags);
/* Querying (nodes == NULL) reports node 0 for every page currently mapped
 * at pages[i] and -ENOENT in status[i] for one that isn't; a move request
 * succeeds for a target of node 0 (the page is already there) and fails
 * -ENODEV for any other target — the honest one-node answer, same as the
 * rest of this header. Only pid == 0 (or the caller's own pid) is
 * supported. */
long move_pages(int pid, unsigned long count, void **pages,
                const int *nodes, int *status, int flags);

#ifdef __cplusplus
}
#endif
