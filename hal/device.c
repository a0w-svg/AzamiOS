/* ============================================================================
 * AzamiOS — NT/WDM-Style Device Object Implementation
 * File: hal/device.c
 *
 * Manages the kernel device tree and integrates with the object manager.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "device.h"
#include "driver.h"
#include "../kernel/mm/kmalloc.h"
#include "../kernel/object/object.h"
#include "../drivers/char/console.h"
#include "../arch/x86_64/cpu/spinlock.h"


/* ── Global state ────────────────────────────────────────────────────────── */
static device_t *g_root_device = NULL;
static spinlock_t g_device_lock = SPINLOCK_INIT;

/* ── Internal helpers ────────────────────────────────────────────────────── */

/* Copy up to n-1 characters from src to dst and null-terminate. */
static void dev_strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    while (i < n - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Compare two null-terminated strings.  Returns true if equal. */
static bool dev_streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return *a == *b;
}

/* String length. */


/* Register a device in the object manager under "\Device\<name>". */
static void dev_register_object(device_t *dev)
{
    /* Build the object path: "\Device\<name>" */
    char path[64];
    const char *prefix = "\\Device\\";
    size_t pi = 0;
    while (prefix[pi]) {
        path[pi] = prefix[pi];
        pi++;
    }
    size_t ni = 0;
    while (ni < sizeof(dev->name) - 1 && dev->name[ni] && pi < 63) {
        path[pi++] = dev->name[ni++];
    }
    path[pi] = '\0';

    dev->obj = az_object_create(path, AZ_OBJ_DEVICE, dev, NULL);
}

/* Add child to parent's child list (append at end for stable ordering). */
static void dev_add_child(device_t *parent, device_t *child)
{
    child->parent = parent;
    child->sibling = NULL;

    if (!parent->children) {
        parent->children = child;
        return;
    }
    device_t *last = parent->children;
    while (last->sibling) last = last->sibling;
    last->sibling = child;
}

/* Remove child from parent's child list. */
static void dev_remove_child(device_t *parent, device_t *child)
{
    if (!parent || !parent->children) return;

    if (parent->children == child) {
        parent->children = child->sibling;
        child->sibling = NULL;
        return;
    }
    device_t *prev = parent->children;
    while (prev->sibling && prev->sibling != child) {
        prev = prev->sibling;
    }
    if (prev->sibling == child) {
        prev->sibling = child->sibling;
        child->sibling = NULL;
    }
}

/* Recursive depth-first search by name. */
static device_t *dev_find_recursive(device_t *node, const char *name)
{
    if (!node) return NULL;
    if (dev_streq(node->name, name)) return node;

    for (device_t *child = node->children; child; child = child->sibling) {
        device_t *found = dev_find_recursive(child, name);
        if (found) return found;
    }
    return NULL;
}

/* Print device tree with indentation. */
static void dev_print_tree(device_t *node, u32 depth)
{
    if (!node) return;

    /* Indentation */
    for (u32 i = 0; i < depth; i++) kprintf("  ");

    /* Device type string */
    const char *type_str;
    switch (node->type) {
    case DEVICE_TYPE_ROOT:    type_str = "Root";    break;
    case DEVICE_TYPE_BUS:     type_str = "Bus";     break;
    case DEVICE_TYPE_BLOCK:   type_str = "Block";   break;
    case DEVICE_TYPE_CHAR:    type_str = "Char";    break;
    case DEVICE_TYPE_DISPLAY: type_str = "Display"; break;
    case DEVICE_TYPE_INPUT:   type_str = "Input";   break;
    case DEVICE_TYPE_NETWORK: type_str = "Network"; break;
    case DEVICE_TYPE_AUDIO:   type_str = "Audio";   break;
    default:                  type_str = "Other";   break;
    }

    kprintf("%-16s [%s]", node->name, type_str);
    if (node->driver) {
        kprintf(" drv=%s", node->driver->name);
    }
    kprintf("\n");

    for (device_t *child = node->children; child; child = child->sibling) {
        dev_print_tree(child, depth + 1);
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void device_init(void)
{
    /* Create the root device — parent of all devices in the system. */
    g_root_device = (device_t *)kzalloc(sizeof(device_t));
    BUG_ON(!g_root_device);

    dev_strncpy(g_root_device->name, "System", sizeof(g_root_device->name));
    g_root_device->type = DEVICE_TYPE_ROOT;
    g_root_device->flags = DEVICE_FLAG_STARTED;

    dev_register_object(g_root_device);

    pr_debug("[HAL] Device tree initialized (root: \\Device\\System)\n");
}

device_t *device_create(const char *name, device_type_t type, device_t *parent)
{
    if (!name) return NULL;
    if (!parent) parent = g_root_device;

    device_t *dev = (device_t *)kzalloc(sizeof(device_t));
    if (!dev) return NULL;

    dev_strncpy(dev->name, name, sizeof(dev->name));
    dev->type = type;

    irqflags_t flags = spinlock_lock_irqsave(&g_device_lock);
    dev_add_child(parent, dev);
    spinlock_unlock_irqrestore(&g_device_lock, flags);

    /* Register in object manager */
    dev_register_object(dev);

    return dev;
}

void device_destroy(device_t *dev)
{
    if (!dev || dev == g_root_device) return;

    irqflags_t flags = spinlock_lock_irqsave(&g_device_lock);

    /* Unlink from parent */
    if (dev->parent) {
        dev_remove_child(dev->parent, dev);
    }

    spinlock_unlock_irqrestore(&g_device_lock, flags);

    /* Dereference object manager entry */
    if (dev->obj) {
        az_object_dereference(dev->obj);
        dev->obj = NULL;
    }

    kfree(dev);
}

void device_set_driver(device_t *dev, struct driver *drv)
{
    if (!dev) return;
    dev->driver = drv;
}

device_t *device_find(const char *name)
{
    if (!name || !g_root_device) return NULL;

    irqflags_t flags = spinlock_lock_irqsave(&g_device_lock);
    device_t *found = dev_find_recursive(g_root_device, name);
    spinlock_unlock_irqrestore(&g_device_lock, flags);

    return found;
}

device_t *device_tree_root(void)
{
    return g_root_device;
}

void device_dump_tree(void)
{
    pr_debug("[HAL] Device Tree:\n");
    dev_print_tree(g_root_device, 1);
}
