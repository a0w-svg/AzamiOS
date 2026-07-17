#ifndef MMP_H
#define MMP_H

#include <stdint.h>
#include <stddef.h>

/**
 * kmalloc - Allocate strictly 16-byte aligned kernel heap memory.
 * @size: Number of bytes requested (0 returns NULL).
 * Return: Pointer to 16-byte aligned payload buffer, or NULL on OOM.
 */
void* kmalloc(size_t size);

/**
 * kfree - Free kernel heap memory and securely coalesce free blocks.
 * @ptr: Pointer returned by kmalloc (NULL is safe no-op).
 */
void kfree(void* ptr);

/**
 * kheap_init - Initialize kernel heap virtual address range and state.
 */
void kheap_init(void);

/**
 * kheap_get_used_bytes - Query total bytes currently allocated on kernel heap.
 */
size_t kheap_get_used_bytes(void);

/**
 * kheap_get_free_bytes - Query total free bytes currently available across heap.
 */
size_t kheap_get_free_bytes(void);

#endif