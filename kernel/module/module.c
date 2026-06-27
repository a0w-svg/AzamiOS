/**
 * module.c  –  AzamiOS Kernel Module Registry Engine
 */
#include "include/module.h"
#include "../klibc/include/stdio.h"
#include "../klibc/include/string.h"

static kernel_module_t *g_mod_registry = (kernel_module_t*)0;
static int g_mod_count = 0;

static const char *type_names[] = {
    "CORE", "MEM ", "FS  ", "DRV ", "NET ", "PROC"
};

void module_register(kernel_module_t *mod) {
    if (!mod) return;
    mod->active = 0;
    mod->next = (kernel_module_t*)0;

    if (!g_mod_registry) {
        g_mod_registry = mod;
    } else {
        kernel_module_t *curr = g_mod_registry;
        while (curr->next) curr = curr->next;
        curr->next = mod;
    }
    g_mod_count++;
}

int module_init_all(void) {
    kprintf("\n=== AzamiOS Modular Kernel Subsystem Bootstrap ===\n");
    int active_cnt = 0;

    /* Initialize in strict dependency order: CORE -> MEM -> FS -> DRV -> NET -> PROC */
    for (int t = MOD_CORE; t <= MOD_PROC; t++) {
        kernel_module_t *curr = g_mod_registry;
        while (curr) {
            if ((int)curr->type == t && !curr->active) {
                if (curr->probe && !curr->probe()) {
                    kprintf("[mod] Probing      [%s] %-10s : %s ... NO DEVICE (Skipped)\n", type_names[t], curr->name, curr->desc);
                } else {
                    kprintf("[mod] Initializing [%s] %-10s : %s ... ", type_names[t], curr->name, curr->desc);
                    int res = 0;
                    if (curr->init) res = curr->init();
                    if (res == 0) {
                        curr->active = 1;
                        active_cnt++;
                        kprintf("OK\n");
                    } else {
                        kprintf("FAILED (%d)\n", res);
                    }
                }
            }
            curr = curr->next;
        }
    }
    kprintf("=== Bootstrap Complete: %d / %d Modules Active ===\n\n", active_cnt, g_mod_count);
    return active_cnt;
}

int module_get_count(void) {
    return g_mod_count;
}

const kernel_module_t *module_get_registry(void) {
    return g_mod_registry;
}

static void append_str(char *buf, const char *str, int max_len) {
    int clen = strlen(buf);
    int slen = strlen(str);
    if (clen >= max_len - 1) return;
    int to_copy = slen;
    if (clen + to_copy > max_len - 1) {
        to_copy = max_len - 1 - clen;
    }
    memcpy(buf + clen, str, to_copy);
    buf[clen + to_copy] = '\0';
}

int module_get_summary_table(char *buf, int max_len) {
    if (!buf || max_len <= 0) return 0;
    buf[0] = '\0';

    const char *header = "AzamiOS Ring 0 Kernel Module Registry:\n"
                         "TYPE   MODULE     STATUS  DESCRIPTION\n"
                         "----------------------------------------------------\n";
    strncpy(buf, header, max_len - 1);
    buf[max_len - 1] = '\0';

    kernel_module_t *curr = g_mod_registry;
    char line[128];
    while (curr) {
        int t = (int)curr->type;
        if (t < 0 || t > 5) t = 0;
        line[0] = '\0';
        append_str(line, "[", sizeof(line));
        append_str(line, type_names[t], sizeof(line));
        append_str(line, "] ", sizeof(line));
        append_str(line, curr->name, sizeof(line));
        int nlen = strlen(curr->name);
        for (int p = nlen; p < 11; p++) append_str(line, " ", sizeof(line));
        append_str(line, curr->active ? "ACTIVE  " : "INACT   ", sizeof(line));
        append_str(line, curr->desc, sizeof(line));
        append_str(line, "\n", sizeof(line));

        append_str(buf, line, max_len);
        curr = curr->next;
    }
    return strlen(buf);
}

int module_reload(const char *name) {
    if (!name) return -1;
    kernel_module_t *curr = g_mod_registry;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            kprintf("[mod] Live reload requested for [%s] : %s\n", curr->name, curr->desc);
            if (curr->active && curr->exit) {
                curr->exit();
            }
            curr->active = 0;
            if (curr->probe && !curr->probe()) {
                kprintf("[mod] Reload probe failed for [%s]\n", curr->name);
                return -2;
            }
            int res = 0;
            if (curr->init) res = curr->init();
            if (res == 0) {
                curr->active = 1;
                kprintf("[mod] Reloaded [%s] successfully without reboot.\n", curr->name);
                return 0;
            } else {
                kprintf("[mod] Reload init failed (%d)\n", res);
                return res;
            }
        }
        curr = curr->next;
    }
    kprintf("[mod] Module '%s' not found in registry\n", name);
    return -1;
}
