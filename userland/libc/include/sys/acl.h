/* ============================================================================
 * AzamiOS Userspace C Library — POSIX Access Control Lists (ACL) Header
 * File: userland/libc/include/sys/acl.h
 * ============================================================================ */
#pragma once

#include "types.h"

#define ACL_MAGIC       0x41434C31
#define ACL_MAX_ENTRIES 32

typedef enum {
    ACL_UNDEFINED_TAG = 0,
    ACL_USER_OBJ      = 1, /* Owner */
    ACL_USER          = 2, /* Named User */
    ACL_GROUP_OBJ     = 3, /* Group */
    ACL_GROUP         = 4, /* Named Group */
    ACL_MASK          = 5, /* Permissions Mask */
    ACL_OTHER         = 6  /* Other */
} acl_tag_t;

#define ACL_READ    0x04
#define ACL_WRITE   0x02
#define ACL_EXECUTE 0x01

typedef struct __attribute__((packed)) {
    uint32_t tag;
    uint32_t id;
    uint32_t perms;
} acl_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t count;
    acl_entry_t entries[ACL_MAX_ENTRIES];
} acl_struct_t;

typedef acl_struct_t *acl_t;

/* POSIX ACL Library Functions */
acl_t acl_init(int count);
int   acl_free(void *obj_p);
acl_t acl_get_file(const char *path_p, int type);
int   acl_set_file(const char *path_p, int type, acl_t acl);
char *acl_to_text(acl_t acl, ssize_t *len_p);
acl_t acl_from_text(const char *buf_p);
int   acl_add_entry(acl_t acl, acl_tag_t tag, uint32_t id, uint32_t perms);
int   acl_delete_entry(acl_t acl, acl_tag_t tag, uint32_t id);
int   acl_calc_mask(acl_t acl);
