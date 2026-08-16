/* ============================================================================
 * AzamiOS — NT/WDM-Style Driver Registry Implementation
 * File: hal/driver.c
 *
 * Maintains a global linked list of registered drivers.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "driver.h"
#include "../drivers/char/console.h"
#include "../arch/x86_64/cpu/spinlock.h"


/* ── Global state ────────────────────────────────────────────────────────── */
static driver_t  *g_driver_list = NULL;
static spinlock_t g_driver_lock = SPINLOCK_INIT;

/* ── Internal helpers ────────────────────────────────────────────────────── */

static bool drv_streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return *a == *b;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void driver_init(void)
{
    g_driver_list = NULL;
    pr_debug("[HAL] Driver registry initialized\n");
}

az_status_t driver_register(driver_t *drv)
{
    if (!drv || !drv->name[0]) return AZ_ERROR_INVAL;

    /* Check for duplicate names */
    irqflags_t flags = spinlock_lock_irqsave(&g_driver_lock);

    for (driver_t *d = g_driver_list; d; d = d->next) {
        if (drv_streq(d->name, drv->name)) {
            spinlock_unlock_irqrestore(&g_driver_lock, flags);
            pr_debug("[HAL] Driver '%s' already registered\n", drv->name);
            return AZ_ERROR_EXIST;
        }
    }

    drv->next = g_driver_list;
    g_driver_list = drv;

    spinlock_unlock_irqrestore(&g_driver_lock, flags);

    pr_debug("[HAL] Driver registered: %s\n", drv->name);
    return AZ_STATUS_SUCCESS;
}

void driver_unregister(driver_t *drv)
{
    if (!drv) return;

    irqflags_t flags = spinlock_lock_irqsave(&g_driver_lock);

    if (g_driver_list == drv) {
        g_driver_list = drv->next;
    } else {
        driver_t *prev = g_driver_list;
        while (prev && prev->next != drv) prev = prev->next;
        if (prev) prev->next = drv->next;
    }
    drv->next = NULL;

    spinlock_unlock_irqrestore(&g_driver_lock, flags);
}

driver_t *driver_find(const char *name)
{
    if (!name) return NULL;

    irqflags_t flags = spinlock_lock_irqsave(&g_driver_lock);

    for (driver_t *d = g_driver_list; d; d = d->next) {
        if (drv_streq(d->name, name)) {
            spinlock_unlock_irqrestore(&g_driver_lock, flags);
            return d;
        }
    }

    spinlock_unlock_irqrestore(&g_driver_lock, flags);
    return NULL;
}
