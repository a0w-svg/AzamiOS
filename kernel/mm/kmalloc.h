/* ============================================================================
 * AzamiOS — Kernel Memory Allocator (kmalloc / kfree) Header
 * File: kernel/mm/kmalloc.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

/** kmalloc_init() — Initialize the kernel slab/bucket allocator. */
void kmalloc_init(void);

/** kmalloc(size) — Allocate `size` bytes of kernel memory. */
void *kmalloc(size_t size);

/** kzalloc(size) — Allocate and zero `size` bytes of kernel memory. */
void *kzalloc(size_t size);

/** krealloc(ptr, new_size) — Reallocate memory block to `new_size`. */
void *krealloc(void *ptr, size_t new_size);

/** kfree(ptr) — Free kernel memory previously allocated by kmalloc/kzalloc. */
void kfree(void *ptr);
