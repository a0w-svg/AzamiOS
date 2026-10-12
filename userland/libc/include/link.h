/* ============================================================================
 * AzamiOS Userspace — Linker Interface Header (link.h)
 * File: userland/libc/include/link.h
 * ============================================================================ */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dl_phdr_info {
    uintptr_t         dlpi_addr;
    const char       *dlpi_name;
    const void       *dlpi_phdr;
    uint16_t          dlpi_phnum;
    unsigned long long dlpi_adds;
    unsigned long long dlpi_subs;
    size_t            dlpi_tls_modid;
    void             *dlpi_tls_data;
};

int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *info, size_t size, void *data), void *data);

#ifdef __cplusplus
}
#endif
