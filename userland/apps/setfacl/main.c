/* ============================================================================
 * AzamiOS Userspace — Set File Access Control Lists (setfacl.elf)
 * File: userland/apps/setfacl/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/sys/stat.h"
#include "../../libc/include/sys/acl.h"

static void print_usage(void)
{
    printf("Usage: setfacl [-bk] [{-m|-x} acl_spec] <file> ...\n");
    printf("  -m, --modify=acl     Modify or add entries to file ACL\n");
    printf("  -x, --remove=acl     Remove specific entries from file ACL\n");
    printf("  -b, --remove-all     Remove all extended ACL entries\n");
    printf("  -h, --help           Display this help\n\n");
    printf("ACL Spec Examples:\n");
    printf("  u:azami:rwx          Grant user 'azami' read, write, execute\n");
    printf("  g:wheel:r-x          Grant group 'wheel' read, execute\n");
    printf("  m::rwx               Set ACL mask permissions\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char *modify_spec = NULL;
    const char *remove_spec = NULL;
    int remove_all = 0;
    int first_file_idx = 1;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--modify") == 0) && i + 1 < argc) {
            modify_spec = argv[++i];
        } else if (strncmp(argv[i], "-m", 2) == 0 && strlen(argv[i]) > 2) {
            modify_spec = argv[i] + 2;
        } else if ((strcmp(argv[i], "-x") == 0 || strcmp(argv[i], "--remove") == 0) && i + 1 < argc) {
            remove_spec = argv[++i];
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--remove-all") == 0) {
            remove_all = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if (argv[i][0] != '-') {
            first_file_idx = i;
            break;
        }
    }

    if (!modify_spec && !remove_spec && !remove_all) {
        print_usage();
        return 1;
    }

    int errors = 0;

    for (int i = first_file_idx; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        const char *path = argv[i];

        struct stat st;
        if (stat(path, &st) < 0) {
            printf("setfacl: %s: No such file or directory\n", path);
            errors++;
            continue;
        }

        if (remove_all) {
            if (acl_set_file(path, 0, NULL) < 0) {
                printf("setfacl: %s: Failed to clear ACL entries\n", path);
                errors++;
            }
            continue;
        }

        acl_t current_acl = acl_get_file(path, 0);
        if (!current_acl) {
            current_acl = acl_init(ACL_MAX_ENTRIES);
            if (current_acl) {
                acl_add_entry(current_acl, ACL_USER_OBJ, st.st_uid, (st.st_mode >> 6) & 7);
                acl_add_entry(current_acl, ACL_GROUP_OBJ, st.st_gid, (st.st_mode >> 3) & 7);
                acl_add_entry(current_acl, ACL_OTHER, 0, st.st_mode & 7);
            }
        }

        if (!current_acl) {
            printf("setfacl: %s: Failed to obtain ACL object\n", path);
            errors++;
            continue;
        }

        if (modify_spec) {
            acl_t mod_acl = acl_from_text(modify_spec);
            if (!mod_acl) {
                printf("setfacl: Invalid ACL specification: %s\n", modify_spec);
                acl_free(current_acl);
                return 1;
            }
            for (uint32_t m = 0; m < mod_acl->count; m++) {
                acl_add_entry(current_acl, mod_acl->entries[m].tag, mod_acl->entries[m].id, mod_acl->entries[m].perms);
            }
            acl_calc_mask(current_acl);
            acl_free(mod_acl);
        }

        if (remove_spec) {
            char tag_type[32] = "";
            char id_str[64] = "";
            char copy[128];
            strncpy(copy, remove_spec, sizeof(copy) - 1);
            copy[sizeof(copy) - 1] = '\0';
            char *c1 = strchr(copy, ':');
            if (c1) {
                *c1 = '\0';
                strncpy(tag_type, copy, sizeof(tag_type) - 1);
                strncpy(id_str, c1 + 1, sizeof(id_str) - 1);
            } else {
                strncpy(tag_type, copy, sizeof(tag_type) - 1);
            }

            if (strcmp(tag_type, "u") == 0 || strcmp(tag_type, "user") == 0) {
                uint32_t uid = (strcmp(id_str, "root") == 0) ? 0 :
                               (strcmp(id_str, "azami") == 0) ? 1000 : (uint32_t)atoi(id_str);
                acl_delete_entry(current_acl, ACL_USER, uid);
            } else if (strcmp(tag_type, "g") == 0 || strcmp(tag_type, "group") == 0) {
                uint32_t gid = (strcmp(id_str, "root") == 0) ? 0 :
                               (strcmp(id_str, "wheel") == 0) ? 10 :
                               (strcmp(id_str, "users") == 0) ? 100 : (uint32_t)atoi(id_str);
                acl_delete_entry(current_acl, ACL_GROUP, gid);
            } else if (strcmp(tag_type, "m") == 0 || strcmp(tag_type, "mask") == 0) {
                acl_delete_entry(current_acl, ACL_MASK, 0);
            }
            acl_calc_mask(current_acl);
        }

        if (acl_set_file(path, 0, current_acl) < 0) {
            printf("setfacl: %s: Failed to apply ACL\n", path);
            errors++;
        }

        acl_free(current_acl);
    }

    return errors ? 1 : 0;
}
