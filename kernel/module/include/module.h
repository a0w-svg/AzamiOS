/**
 * module.h  –  AzamiOS Ring 0 Kernel Module Registry Framework
 * Decouples monolithic initialization into self-describing modular subsystems.
 */
#ifndef _KERNEL_MODULE_H
#define _KERNEL_MODULE_H

#include <stdbool.h>

typedef enum {
    MOD_CORE = 0,
    MOD_MEM,
    MOD_FS,
    MOD_DRV,
    MOD_NET,
    MOD_PROC
} mod_type_t;

typedef struct kernel_module {
    const char *name;
    const char *desc;
    mod_type_t  type;
    bool (*probe)(void);
    int (*init)(void);
    int (*exit)(void);
    int active;
    struct kernel_module *next;
} kernel_module_t;


void module_register(kernel_module_t *mod);
int  module_init_all(void);
int  module_get_count(void);
const kernel_module_t *module_get_registry(void);
int  module_get_summary_table(char *buf, int max_len);

#endif /* _KERNEL_MODULE_H */
