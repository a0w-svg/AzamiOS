/* ============================================================================
 * AzamiOS — POSIX Access Control Lists (ACL) Implementation
 * File: kernel/security/acl.c
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "acl.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/lib/string.h"

#define ACL_NODE_MAP_SIZE 256

typedef struct acl_node_entry {
    u64 ino;
    void *sb;
    acl_table_t acl;
    struct acl_node_entry *next;
} acl_node_entry_t;

static acl_node_entry_t *g_acl_hash[ACL_NODE_MAP_SIZE];
static spinlock_t g_acl_lock = SPINLOCK_INIT;

static inline u32 acl_hash(u64 ino, void *sb)
{
    return (u32)(((ino ^ (uintptr_t)sb) >> 4) % ACL_NODE_MAP_SIZE);
}

void acl_init(void)
{
    irqflags_t flags = spinlock_lock_irqsave(&g_acl_lock);
    memset(g_acl_hash, 0, sizeof(g_acl_hash));
    spinlock_unlock_irqrestore(&g_acl_lock, flags);
    pr_debug("[ACL] Kernel POSIX Access Control List engine initialized.\n");
}

int acl_get_for_inode(inode_t *inode, acl_entry_t *out_entries, int max_entries)
{
    if (!inode || !out_entries || max_entries <= 0) return -EINVAL;

    irqflags_t flags = spinlock_lock_irqsave(&g_acl_lock);

    u32 h = acl_hash(inode->i_ino, inode->i_sb);
    acl_node_entry_t *cur = g_acl_hash[h];
    while (cur) {
        if (cur->ino == inode->i_ino && cur->sb == inode->i_sb) {
            int count = (int)cur->acl.count;
            if (count > max_entries) count = max_entries;
            memcpy(out_entries, cur->acl.entries, sizeof(acl_entry_t) * count);
            spinlock_unlock_irqrestore(&g_acl_lock, flags);
            return count;
        }
        cur = cur->next;
    }

    spinlock_unlock_irqrestore(&g_acl_lock, flags);

    /* Synthesize minimal base POSIX ACL from standard mode bits */
    if (max_entries < 3) return -ERANGE;

    out_entries[0].tag = ACL_USER_OBJ;
    out_entries[0].id = inode->i_uid;
    out_entries[0].perms = (inode->i_mode >> 6) & 7;

    out_entries[1].tag = ACL_GROUP_OBJ;
    out_entries[1].id = inode->i_gid;
    out_entries[1].perms = (inode->i_mode >> 3) & 7;

    out_entries[2].tag = ACL_OTHER;
    out_entries[2].id = 0;
    out_entries[2].perms = inode->i_mode & 7;

    return 3;
}

int acl_set_for_inode(inode_t *inode, const acl_entry_t *entries, int count)
{
    if (!inode || count < 0 || count > ACL_MAX_ENTRIES) return -EINVAL;

    irqflags_t flags = spinlock_lock_irqsave(&g_acl_lock);

    u32 h = acl_hash(inode->i_ino, inode->i_sb);
    acl_node_entry_t **prev = &g_acl_hash[h];
    acl_node_entry_t *cur = g_acl_hash[h];

    while (cur) {
        if (cur->ino == inode->i_ino && cur->sb == inode->i_sb) {
            if (count == 0) {
                /* Remove extended ACL */
                *prev = cur->next;
                kfree(cur);
                spinlock_unlock_irqrestore(&g_acl_lock, flags);
                return 0;
            }
            cur->acl.count = (u32)count;
            memcpy(cur->acl.entries, entries, sizeof(acl_entry_t) * count);
            spinlock_unlock_irqrestore(&g_acl_lock, flags);
            return 0;
        }
        prev = &cur->next;
        cur = cur->next;
    }

    if (count > 0 && entries) {
        acl_node_entry_t *node = (acl_node_entry_t *)kmalloc(sizeof(acl_node_entry_t));
        if (!node) {
            spinlock_unlock_irqrestore(&g_acl_lock, flags);
            return -ENOMEM;
        }
        node->ino = inode->i_ino;
        node->sb = inode->i_sb;
        node->acl.magic = ACL_MAGIC;
        node->acl.count = (u32)count;
        memcpy(node->acl.entries, entries, sizeof(acl_entry_t) * count);

        node->next = g_acl_hash[h];
        g_acl_hash[h] = node;
    }

    spinlock_unlock_irqrestore(&g_acl_lock, flags);
    return 0;
}

bool acl_check_permission(inode_t *inode, u32 uid, u32 gid, int requested_mode)
{
    if (!inode) return false;

    /* Root bypasses all permission checks */
    if (uid == 0) return true;

    acl_entry_t entries[ACL_MAX_ENTRIES];
    int count = acl_get_for_inode(inode, entries, ACL_MAX_ENTRIES);
    if (count <= 0) {
        /* Fallback to traditional UNIX mode check */
        int mode = inode->i_mode;
        if (uid == inode->i_uid) {
            int user_perms = (mode >> 6) & 7;
            return ((user_perms & requested_mode) == requested_mode);
        } else if (gid == inode->i_gid) {
            int group_perms = (mode >> 3) & 7;
            return ((group_perms & requested_mode) == requested_mode);
        } else {
            int other_perms = mode & 7;
            return ((other_perms & requested_mode) == requested_mode);
        }
    }

    u32 mask = 0x7;
    bool has_mask = false;

    for (int i = 0; i < count; i++) {
        if (entries[i].tag == ACL_MASK) {
            mask = entries[i].perms;
            has_mask = true;
            break;
        }
    }

    /* 1. Check Owner match */
    for (int i = 0; i < count; i++) {
        if (entries[i].tag == ACL_USER_OBJ && uid == inode->i_uid) {
            return ((entries[i].perms & (u32)requested_mode) == (u32)requested_mode);
        }
    }

    /* 2. Check Named User match */
    for (int i = 0; i < count; i++) {
        if (entries[i].tag == ACL_USER && entries[i].id == uid) {
            u32 effective = has_mask ? (entries[i].perms & mask) : entries[i].perms;
            return ((effective & (u32)requested_mode) == (u32)requested_mode);
        }
    }

    /* 3. Check Group matches */
    bool group_matched = false;
    for (int i = 0; i < count; i++) {
        if ((entries[i].tag == ACL_GROUP_OBJ && gid == inode->i_gid) ||
            (entries[i].tag == ACL_GROUP && entries[i].id == gid)) {
            group_matched = true;
            u32 effective = has_mask ? (entries[i].perms & mask) : entries[i].perms;
            if ((effective & (u32)requested_mode) == (u32)requested_mode) {
                return true;
            }
        }
    }
    if (group_matched) return false;

    /* 4. Check Other / World */
    for (int i = 0; i < count; i++) {
        if (entries[i].tag == ACL_OTHER) {
            return ((entries[i].perms & (u32)requested_mode) == (u32)requested_mode);
        }
    }

    return false;
}
