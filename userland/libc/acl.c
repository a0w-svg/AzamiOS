/* ============================================================================
 * AzamiOS Userspace C Library — POSIX Access Control Lists (ACL) Implementation
 * File: userland/libc/acl.c
 * ============================================================================ */

#include "include/sys/acl.h"
#include "include/sys/syscall.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/stdio.h"
#include "include/unistd.h"

extern long syscall3(long num, long a1, long a2, long a3);

acl_t acl_init(int count)
{
    (void)count;
    acl_t acl = (acl_t)malloc(sizeof(acl_struct_t));
    if (!acl) return NULL;
    acl->magic = ACL_MAGIC;
    acl->count = 0;
    memset(acl->entries, 0, sizeof(acl->entries));
    return acl;
}

int acl_free(void *obj_p)
{
    if (obj_p) {
        free(obj_p);
    }
    return 0;
}

acl_t acl_get_file(const char *path_p, int type)
{
    (void)type;
    if (!path_p) return NULL;

    acl_t acl = acl_init(ACL_MAX_ENTRIES);
    if (!acl) return NULL;

    long res = syscall3(SYS_getfacl, (long)path_p, (long)acl->entries, ACL_MAX_ENTRIES);
    if (res < 0) {
        acl_free(acl);
        return NULL;
    }

    acl->count = (uint32_t)res;
    return acl;
}

int acl_set_file(const char *path_p, int type, acl_t acl)
{
    (void)type;
    if (!path_p) return -1;

    int count = acl ? (int)acl->count : 0;
    const acl_entry_t *entries = acl ? acl->entries : NULL;

    long res = syscall3(SYS_setfacl, (long)path_p, (long)entries, count);
    return (res < 0) ? -1 : 0;
}

static void format_perms(uint32_t perms, char *out)
{
    out[0] = (perms & ACL_READ)    ? 'r' : '-';
    out[1] = (perms & ACL_WRITE)   ? 'w' : '-';
    out[2] = (perms & ACL_EXECUTE) ? 'x' : '-';
    out[3] = '\0';
}

static uint32_t parse_perms(const char *s)
{
    uint32_t p = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == 'r' || s[i] == 'R') p |= ACL_READ;
        else if (s[i] == 'w' || s[i] == 'W') p |= ACL_WRITE;
        else if (s[i] == 'x' || s[i] == 'X') p |= ACL_EXECUTE;
    }
    return p;
}

char *acl_to_text(acl_t acl, ssize_t *len_p)
{
    if (!acl || acl->magic != ACL_MAGIC) return NULL;

    char *buf = (char *)malloc(1024);
    if (!buf) return NULL;
    buf[0] = '\0';

    char line[128];
    char pstr[8];

    for (uint32_t i = 0; i < acl->count; i++) {
        acl_entry_t *e = &acl->entries[i];
        format_perms(e->perms, pstr);

        switch (e->tag) {
        case ACL_USER_OBJ:
            snprintf(line, sizeof(line), "user::%s\n", pstr);
            break;
        case ACL_USER:
            if (e->id == 0) snprintf(line, sizeof(line), "user:root:%s\n", pstr);
            else if (e->id == 1000) snprintf(line, sizeof(line), "user:azami:%s\n", pstr);
            else snprintf(line, sizeof(line), "user:%u:%s\n", e->id, pstr);
            break;
        case ACL_GROUP_OBJ:
            snprintf(line, sizeof(line), "group::%s\n", pstr);
            break;
        case ACL_GROUP:
            if (e->id == 0) snprintf(line, sizeof(line), "group:root:%s\n", pstr);
            else if (e->id == 10) snprintf(line, sizeof(line), "group:wheel:%s\n", pstr);
            else if (e->id == 100) snprintf(line, sizeof(line), "group:users:%s\n", pstr);
            else snprintf(line, sizeof(line), "group:%u:%s\n", e->id, pstr);
            break;
        case ACL_MASK:
            snprintf(line, sizeof(line), "mask::%s\n", pstr);
            break;
        case ACL_OTHER:
            snprintf(line, sizeof(line), "other::%s\n", pstr);
            break;
        default:
            continue;
        }
        strcat(buf, line);
    }

    if (len_p) *len_p = (ssize_t)strlen(buf);
    return buf;
}

int acl_add_entry(acl_t acl, acl_tag_t tag, uint32_t id, uint32_t perms)
{
    if (!acl || acl->magic != ACL_MAGIC) return -1;

    /* Check if matching tag + id already exists; update it if so */
    for (uint32_t i = 0; i < acl->count; i++) {
        if (acl->entries[i].tag == tag && (tag != ACL_USER && tag != ACL_GROUP ? 1 : acl->entries[i].id == id)) {
            acl->entries[i].perms = perms;
            return 0;
        }
    }

    if (acl->count >= ACL_MAX_ENTRIES) return -1;

    acl->entries[acl->count].tag = tag;
    acl->entries[acl->count].id = id;
    acl->entries[acl->count].perms = perms;
    acl->count++;

    return 0;
}

int acl_delete_entry(acl_t acl, acl_tag_t tag, uint32_t id)
{
    if (!acl || acl->magic != ACL_MAGIC) return -1;

    for (uint32_t i = 0; i < acl->count; i++) {
        if (acl->entries[i].tag == tag && (tag != ACL_USER && tag != ACL_GROUP ? 1 : acl->entries[i].id == id)) {
            for (uint32_t j = i; j < acl->count - 1; j++) {
                acl->entries[j] = acl->entries[j + 1];
            }
            acl->count--;
            return 0;
        }
    }
    return -1;
}

int acl_calc_mask(acl_t acl)
{
    if (!acl || acl->magic != ACL_MAGIC) return -1;

    uint32_t mask = 0;
    bool need_mask = false;

    for (uint32_t i = 0; i < acl->count; i++) {
        if (acl->entries[i].tag == ACL_USER || acl->entries[i].tag == ACL_GROUP_OBJ || acl->entries[i].tag == ACL_GROUP) {
            mask |= acl->entries[i].perms;
            need_mask = true;
        }
    }

    if (need_mask) {
        acl_add_entry(acl, ACL_MASK, 0, mask);
    }
    return 0;
}

acl_t acl_from_text(const char *buf_p)
{
    if (!buf_p) return NULL;

    acl_t acl = acl_init(ACL_MAX_ENTRIES);
    if (!acl) return NULL;

    char copy[512];
    strncpy(copy, buf_p, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(copy, ",\n", &saveptr);

    while (token) {
        while (*token == ' ') token++;
        if (*token && *token != '#') {
            char tag_type[32] = "";
            char id_str[64] = "";
            char perm_str[32] = "";

            /* Parse spec format: type:[id]:perms or type::perms */
            char *c1 = strchr(token, ':');
            if (c1) {
                *c1 = '\0';
                strncpy(tag_type, token, sizeof(tag_type) - 1);
                char *c2 = strchr(c1 + 1, ':');
                if (c2) {
                    *c2 = '\0';
                    strncpy(id_str, c1 + 1, sizeof(id_str) - 1);
                    strncpy(perm_str, c2 + 1, sizeof(perm_str) - 1);
                } else {
                    strncpy(perm_str, c1 + 1, sizeof(perm_str) - 1);
                }

                uint32_t perms = parse_perms(perm_str);
                if (strcmp(tag_type, "u") == 0 || strcmp(tag_type, "user") == 0) {
                    if (strlen(id_str) == 0) {
                        acl_add_entry(acl, ACL_USER_OBJ, 0, perms);
                    } else {
                        uint32_t uid = (strcmp(id_str, "root") == 0) ? 0 :
                                       (strcmp(id_str, "azami") == 0) ? 1000 :
                                       (uint32_t)atoi(id_str);
                        acl_add_entry(acl, ACL_USER, uid, perms);
                    }
                } else if (strcmp(tag_type, "g") == 0 || strcmp(tag_type, "group") == 0) {
                    if (strlen(id_str) == 0) {
                        acl_add_entry(acl, ACL_GROUP_OBJ, 0, perms);
                    } else {
                        uint32_t gid = (strcmp(id_str, "root") == 0) ? 0 :
                                       (strcmp(id_str, "wheel") == 0) ? 10 :
                                       (strcmp(id_str, "users") == 0) ? 100 :
                                       (uint32_t)atoi(id_str);
                        acl_add_entry(acl, ACL_GROUP, gid, perms);
                    }
                } else if (strcmp(tag_type, "m") == 0 || strcmp(tag_type, "mask") == 0) {
                    acl_add_entry(acl, ACL_MASK, 0, perms);
                } else if (strcmp(tag_type, "o") == 0 || strcmp(tag_type, "other") == 0) {
                    acl_add_entry(acl, ACL_OTHER, 0, perms);
                }
            }
        }
        token = strtok_r(NULL, ",\n", &saveptr);
    }

    acl_calc_mask(acl);
    return acl;
}
