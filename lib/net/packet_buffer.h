/**
 * lib/net/packet_buffer.h  –  AzamiOS Core Packet Buffer Manager
 *
 * Equivalent to Linux's `sk_buff` or BSD's `mbuf`.
 * Designed for a bare-metal, monolithic x86_64 kernel (`no_std`).
 *
 * Features:
 *   - Zero-copy header manipulation (`push`, `pull`, `put`).
 *   - Strict out-of-bounds safety checks and integer overflow protection.
 *   - Configurable allocator hooks (`kmalloc` / `kfree` or custom) with a
 *     built-in static pool fallback for freestanding environments.
 */

#ifndef PACKET_BUFFER_H
#define PACKET_BUFFER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Default reservation space at buffer start for link-layer and network headers */
#define PKT_BUF_DEFAULT_HEADROOM 128

/* Built-in static pool configuration for freestanding fallback allocation */
#define PKT_BUF_POOL_SIZE     64
#define PKT_BUF_MAX_DATA_SIZE 2048

/* Buffer flags */
#define PKT_BUF_FLAG_POOL    (1 << 0)  /* Allocated from built-in static pool */
#define PKT_BUF_FLAG_DYNAMIC (1 << 1)  /* Allocated from dynamic memory allocator */
#define PKT_BUF_FLAG_CLONED  (1 << 2)  /* Buffer is a zero-copy clone/reference */

/* Allocator function pointer types */
typedef void *(*packet_buffer_alloc_fn_t)(size_t size);
typedef void  (*packet_buffer_free_fn_t)(void *ptr);

/**
 * PacketBuffer Structure
 *
 * Memory Layout:
 *   +-----------------------------------------------------------------------+
 *   | Headroom (Headers)      | Valid Packet Data (len) | Tailroom          |
 *   +-----------------------------------------------------------------------+
 *   ^                         ^                         ^                   ^
 *   head                      data                      tail                end
 *
 * Invariants:
 *   1. head <= data <= tail <= end
 *   2. capacity == (size_t)(end - head)
 *   3. len == (size_t)(tail - data)
 */
typedef struct PacketBuffer {
    uint8_t *head;               /* Head of allocated memory buffer (inclusive) */
    uint8_t *data;               /* Current start of valid packet data (inclusive) */
    uint8_t *tail;               /* Current end of valid packet data (exclusive) */
    uint8_t *end;                /* End of allocated memory buffer (exclusive) */
    size_t   capacity;           /* Total data buffer capacity (`end - head`) */
    size_t   len;                /* Current valid data length (`tail - data`) */
    uint16_t ref_count;          /* Reference counter for zero-copy cloning */
    uint16_t flags;              /* Allocation and lifecycle state flags */
    packet_buffer_free_fn_t free_fn; /* Release function for dynamic allocations */
    struct PacketBuffer *next;   /* Next buffer in packet queue/list */
    struct PacketBuffer *prev;   /* Previous buffer in packet queue/list */
} PacketBuffer;

/* ── Global Allocator Configuration ────────────────────────────────────── */

/**
 * packet_buffer_init_allocator - Register global allocation and release functions.
 * @alloc_fn: Pointer to dynamic memory allocation function (e.g., kmalloc).
 * @free_fn:  Pointer to memory release function (e.g., kfree).
 *
 * If not set, or if `alloc_fn` returns NULL, `packet_buffer_allocate()` falls back
 * safely to the built-in static memory pool (`g_pkt_buf_pool`).
 */
void packet_buffer_init_allocator(packet_buffer_alloc_fn_t alloc_fn,
                                  packet_buffer_free_fn_t free_fn);

/* ── Allocation & Lifecycle API ────────────────────────────────────────── */

/**
 * packet_buffer_allocate - Allocate and initialize a new PacketBuffer.
 * @capacity: Requested data buffer capacity in bytes.
 *
 * Reserves `PKT_BUF_DEFAULT_HEADROOM` bytes at the start (`data = head + headroom`)
 * and ensures `capacity` bytes are available across headroom + tailroom.
 *
 * Returns a pointer to the initialized `PacketBuffer`, or NULL on failure.
 */
PacketBuffer *packet_buffer_allocate(size_t capacity);

/**
 * packet_buffer_allocate_ext - Allocate a buffer using explicit custom allocators.
 * @capacity: Requested data buffer capacity in bytes.
 * @alloc_fn: Custom allocation function pointer.
 * @free_fn:  Custom release function pointer.
 *
 * Returns a pointer to the initialized `PacketBuffer`, or NULL on failure.
 */
PacketBuffer *packet_buffer_allocate_ext(size_t capacity,
                                         packet_buffer_alloc_fn_t alloc_fn,
                                         packet_buffer_free_fn_t free_fn);

/**
 * packet_buffer_free - Safely release memory associated with a PacketBuffer.
 * @buf: Pointer to the buffer to release.
 *
 * Handles reference counted zero-copy clones properly: if `ref_count > 1`,
 * decrements the counter and returns without freeing memory.
 */
void packet_buffer_free(PacketBuffer *buf);

/* ── Pointer Manipulation API ──────────────────────────────────────────── */

/**
 * packet_buffer_push - Move the data pointer backward to reserve space for headers.
 * @buf:  Pointer to the PacketBuffer.
 * @size: Number of bytes to prepend (`data -= size`).
 *
 * Includes strict out-of-bounds safety check against `head`.
 * Returns the new `data` pointer on success, or NULL if insufficient headroom.
 */
uint8_t *packet_buffer_push(PacketBuffer *buf, size_t size);

/**
 * packet_buffer_pull - Move the data pointer forward to strip headers.
 * @buf:  Pointer to the PacketBuffer.
 * @size: Number of bytes to strip (`data += size`).
 *
 * Includes strict out-of-bounds safety check against `tail`.
 * Returns the new `data` pointer on success, or NULL if `size > len`.
 */
uint8_t *packet_buffer_pull(PacketBuffer *buf, size_t size);

/**
 * packet_buffer_put - Move the tail pointer forward to append data.
 * @buf:  Pointer to the PacketBuffer.
 * @size: Number of bytes to append (`tail += size`).
 *
 * Includes strict out-of-bounds safety check against `end`.
 * Returns the old `tail` pointer where new data should be copied, or NULL on error.
 */
uint8_t *packet_buffer_put(PacketBuffer *buf, size_t size);

/* ── Auxiliary Helper & Zero-Copy API ──────────────────────────────────── */

/**
 * packet_buffer_reserve - Adjust headroom by moving both data and tail forward.
 * @buf: Pointer to the PacketBuffer.
 * @len: Number of bytes to advance `data` and `tail`.
 *
 * Must be called when `len == 0` (empty buffer). Checks safety against `end`.
 * Returns true on success, false on out-of-bounds error.
 */
bool packet_buffer_reserve(PacketBuffer *buf, size_t len);

/**
 * packet_buffer_trim - Trims data from the tail down to `len` bytes.
 * @buf: Pointer to the PacketBuffer.
 * @len: Target maximum length.
 *
 * If `buf->len > len`, moves `tail` backward so `buf->len == len`.
 * Returns true on success, false if `buf == NULL`.
 */
bool packet_buffer_trim(PacketBuffer *buf, size_t len);

/**
 * packet_buffer_reference - Increment reference counter for safe zero-copy sharing.
 * @buf: Pointer to the PacketBuffer.
 *
 * Returns the same buffer pointer (`buf`), or NULL if `buf == NULL`.
 */
PacketBuffer *packet_buffer_reference(PacketBuffer *buf);

/**
 * packet_buffer_reset - Reset data and tail pointers to initial clean state.
 * @buf: Pointer to the PacketBuffer.
 *
 * Sets `len = 0` and restores `data = tail = head + headroom` without freeing.
 */
void packet_buffer_reset(PacketBuffer *buf);

/**
 * packet_buffer_headroom - Get available headroom (`data - head`).
 */
size_t packet_buffer_headroom(const PacketBuffer *buf);

/**
 * packet_buffer_tailroom - Get available tailroom (`end - tail`).
 */
size_t packet_buffer_tailroom(const PacketBuffer *buf);

/**
 * packet_buffer_length - Get valid packet data length (`len`).
 */
size_t packet_buffer_length(const PacketBuffer *buf);

/**
 * packet_buffer_copy_from - Append `len` bytes of data from `src` using `put`.
 * @buf: Pointer to the PacketBuffer.
 * @src: Source data pointer.
 * @len: Number of bytes to copy.
 *
 * Returns true on success, false if out of memory/tailroom or NULL pointers.
 */
bool packet_buffer_copy_from(PacketBuffer *buf, const void *src, size_t len);

/**
 * packet_buffer_copy_to - Copy `len` bytes from packet `data` to `dst`.
 * @buf: Pointer to the PacketBuffer.
 * @dst: Destination buffer pointer.
 * @len: Number of bytes to copy.
 *
 * Returns number of bytes actually copied (`min(len, buf->len)`), or 0 on error.
 */
size_t packet_buffer_copy_to(const PacketBuffer *buf, void *dst, size_t len);

/**
 * packet_buffer_selftest - Execute comprehensive verification suite.
 *
 * Tests all allocation paths, bounds checks, integer overflow protections,
 * and zero-copy pointer shifts.
 *
 * Returns true if all tests pass, false otherwise.
 */
bool packet_buffer_selftest(void);

#endif /* PACKET_BUFFER_H */
