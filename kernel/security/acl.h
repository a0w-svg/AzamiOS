/* ============================================================================
 * AzamiOS — POSIX Access Control Lists (ACL) Subsystem Header
 * File: kernel/security/acl.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../include/azami/defs.h"
#include "../../fs/vfs.h"

#define ACL_MAGIC       0x41434C31 /* "ACL1" */
#define ACL_MAX_ENTRIES 32

typedef enum {
    ACL_UNDEFINED_TAG = 0,
    ACL_USER_OBJ      = 1, /* File Owner */
    ACL_USER          = 2, /* Named User (by UID) */
    ACL_GROUP_OBJ     = 3, /* File Group */
    ACL_GROUP         = 4, /* Named Group (by GID) */
    ACL_MASK          = 5, /* Effective Permissions Mask */
    ACL_OTHER         = 6  /* Other (World) */
} acl_tag_t;

#define ACL_READ    0x04
#define ACL_WRITE   0x02
#define ACL_EXECUTE 0x01

typedef struct __attribute__((packed)) {
    u32 tag;       /* acl_tag_t */
    u32 id;        /* UID or GID */
    u32 perms;     /* Bitmask of ACL_READ, ACL_WRITE, ACL_EXECUTE */
} acl_entry_t;

typedef struct __attribute__((packed)) {
    u32 magic;
    u32 count;
    acl_entry_t entries[ACL_MAX_ENTRIES];
} acl_table_t;

/** acl_init() — Initialize kernel ACL subsystem. */
void acl_init(void);

/** acl_get_for_inode() — Retrieve ACL table for given inode. */
int acl_get_for_inode(inode_t *inode, acl_entry_t *out_entries, int max_entries);

/** acl_set_for_inode() — Assign ACL table to given inode. */
int acl_set_for_inode(inode_t *inode, const acl_entry_t *entries, int count);

/** acl_check_permission() — Evaluate permissions for UID/GID against file inode. */
bool acl_check_permission(inode_t *inode, u32 uid, u32 gid, int requested_mode);
