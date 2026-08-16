/* ============================================================================
 * AzamiOS — Common Type Definitions
 * File: include/azami/types.h
 *
 * These types are shared between the kernel, drivers, and userspace headers.
 * Only freestanding headers are pulled in (stdint, stddef, stdbool).
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Status codes
 * Negative values indicate errors, 0 = success, positive = informational.
 * -------------------------------------------------------------------------- */
typedef int32_t az_status_t;

#define AZ_STATUS_SUCCESS          ((az_status_t)  0)
#define AZ_STATUS_PENDING          ((az_status_t)  1)
#define AZ_STATUS_ALREADY_DONE     ((az_status_t)  2)

#define AZ_ERROR_NOMEM             ((az_status_t) -1)
#define AZ_ERROR_INVAL             ((az_status_t) -2)
#define AZ_ERROR_NOTFOUND          ((az_status_t) -3)
#define AZ_ERROR_PERM              ((az_status_t) -4)
#define AZ_ERROR_BUSY              ((az_status_t) -5)
#define AZ_ERROR_TIMEDOUT          ((az_status_t) -6)
#define AZ_ERROR_OVERFLOW          ((az_status_t) -7)
#define AZ_ERROR_NOTSUP            ((az_status_t) -8)
#define AZ_ERROR_IO                ((az_status_t) -9)
#define AZ_ERROR_FAULT             ((az_status_t) -10)
#define AZ_ERROR_EXIST             ((az_status_t) -11)

/* Evaluates to true if the status code represents an error. */
#define AZ_FAILED(s)  ((s) < 0)
/* Legacy alias kept for source compatibility. */
#define AZ_ERROR(s)   AZ_FAILED(s)

/* --------------------------------------------------------------------------
 * Handle type  (opaque 64-bit token, process-local)
 * -------------------------------------------------------------------------- */
typedef uint64_t az_handle_t;
#define AZ_INVALID_HANDLE  ((az_handle_t)0)

/* --------------------------------------------------------------------------
 * Physical / virtual address helpers
 * -------------------------------------------------------------------------- */
typedef uintptr_t phys_addr_t;
typedef uintptr_t virt_addr_t;

/* Higher-half direct map base: phys 0 is mapped at HHDM_BASE in the kernel. */
#define HHDM_BASE          ((virt_addr_t)0xFFFF800000000000ULL)

/* Kernel image linked at -2 GB (0xFFFFFFFF80000000). */
#define KERNEL_VIRT_BASE   ((virt_addr_t)0xFFFFFFFF80000000ULL)

/* Convert physical address → kernel virtual address via HHDM. */
#define PHYS_TO_VIRT(p)    ((void *)((virt_addr_t)(p) + HHDM_BASE))
/* Convert HHDM virtual address → physical address. */
#define VIRT_TO_PHYS(v)    ((phys_addr_t)((virt_addr_t)(v) - HHDM_BASE))

/* --------------------------------------------------------------------------
 * Page size constants
 * -------------------------------------------------------------------------- */
#define PAGE_SIZE          4096UL
#define PAGE_SHIFT         12U
#define PAGE_MASK          (~(PAGE_SIZE - 1UL))

/* Align an address upward to the next page boundary. */
#define PAGE_ALIGN_UP(a)   (((virt_addr_t)(a) + PAGE_SIZE - 1UL) & PAGE_MASK)
/* Align an address downward to the current page boundary. */
#define PAGE_ALIGN_DOWN(a) ((virt_addr_t)(a) & PAGE_MASK)

/* --------------------------------------------------------------------------
 * Primitive aliases used across the kernel
 * -------------------------------------------------------------------------- */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
