/**
 * lib/net/packet_buffer.c  –  AzamiOS Core Packet Buffer Manager Implementation
 *
 * Full, production-ready implementation of `PacketBuffer` (sk_buff / mbuf equivalent).
 * Zero-copy pointer shifts, rigorous memory safety checks, freestanding (`no_std`) compliant.
 */

#include "packet_buffer.h"

/* ── Global Allocator Pointers ─────────────────────────────────────────── */

static packet_buffer_alloc_fn_t g_pkt_buf_alloc_fn = NULL;
static packet_buffer_free_fn_t  g_pkt_buf_free_fn  = NULL;

/* ── Freestanding Static Memory Pool Fallback ──────────────────────────── */

typedef struct {
    PacketBuffer buf_struct;
    uint8_t      data_buf[PKT_BUF_MAX_DATA_SIZE];
    bool         in_use;
} pkt_buf_pool_entry_t;

static pkt_buf_pool_entry_t g_pkt_buf_pool[PKT_BUF_POOL_SIZE];

/* ── Global Allocator Configuration ────────────────────────────────────── */

void packet_buffer_init_allocator(packet_buffer_alloc_fn_t alloc_fn,
                                  packet_buffer_free_fn_t free_fn)
{
    g_pkt_buf_alloc_fn = alloc_fn;
    g_pkt_buf_free_fn  = free_fn;
}

/* ── Internal Helper Functions ─────────────────────────────────────────── */

static void packet_buffer_zero_struct(PacketBuffer *buf)
{
    uint8_t *ptr = (uint8_t *)buf;
    for (size_t i = 0; i < sizeof(PacketBuffer); i++) {
        ptr[i] = 0;
    }
}

static void packet_buffer_setup_pointers(PacketBuffer *buf, uint8_t *memory,
                                         size_t data_capacity, uint16_t flags,
                                         packet_buffer_free_fn_t free_fn)
{
    packet_buffer_zero_struct(buf);

    buf->head     = memory;
    buf->end      = memory + data_capacity;
    buf->capacity = data_capacity;

    /* Reserve default headroom if capacity permits, otherwise reserve what we can */
    size_t headroom = (data_capacity >= PKT_BUF_DEFAULT_HEADROOM)
                      ? PKT_BUF_DEFAULT_HEADROOM
                      : 0;

    buf->data      = buf->head + headroom;
    buf->tail      = buf->data;
    buf->len       = 0;
    buf->ref_count = 1;
    buf->flags     = flags;
    buf->free_fn   = free_fn;
    buf->next      = NULL;
    buf->prev      = NULL;
}

/* ── Allocation & Lifecycle API ────────────────────────────────────────── */

PacketBuffer *packet_buffer_allocate_ext(size_t capacity,
                                         packet_buffer_alloc_fn_t alloc_fn,
                                         packet_buffer_free_fn_t free_fn)
{
    /* Check against integer overflow when computing memory requirements */
    if (capacity > (size_t)(-1) - PKT_BUF_DEFAULT_HEADROOM - sizeof(PacketBuffer) - 16) {
        return NULL;
    }

    size_t data_capacity = capacity + PKT_BUF_DEFAULT_HEADROOM;
    /* Ensure 8-byte alignment for data capacity and struct size */
    data_capacity = (data_capacity + 7) & ~((size_t)7);
    size_t struct_align = (sizeof(PacketBuffer) + 7) & ~((size_t)7);
    size_t total_alloc  = struct_align + data_capacity;

    if (alloc_fn != NULL) {
        void *ptr = alloc_fn(total_alloc);
        if (ptr != NULL) {
            PacketBuffer *buf = (PacketBuffer *)ptr;
            uint8_t *data_mem = (uint8_t *)ptr + struct_align;
            packet_buffer_setup_pointers(buf, data_mem, data_capacity,
                                         PKT_BUF_FLAG_DYNAMIC, free_fn);
            return buf;
        }
    }

    /* Fallback to freestanding static memory pool if dynamic allocation is unavailable
     * or fails, provided requested size fits within static pool entry capacity.
     */
    if (data_capacity <= PKT_BUF_MAX_DATA_SIZE) {
        for (size_t i = 0; i < PKT_BUF_POOL_SIZE; i++) {
            if (!g_pkt_buf_pool[i].in_use) {
                g_pkt_buf_pool[i].in_use = true;
                PacketBuffer *buf = &g_pkt_buf_pool[i].buf_struct;
                packet_buffer_setup_pointers(buf, g_pkt_buf_pool[i].data_buf,
                                             PKT_BUF_MAX_DATA_SIZE,
                                             PKT_BUF_FLAG_POOL, NULL);
                return buf;
            }
        }
    }

    return NULL;
}

PacketBuffer *packet_buffer_allocate(size_t capacity)
{
    return packet_buffer_allocate_ext(capacity, g_pkt_buf_alloc_fn, g_pkt_buf_free_fn);
}

void packet_buffer_free(PacketBuffer *buf)
{
    if (buf == NULL) {
        return;
    }

    /* Support zero-copy cloning by decrementing reference count */
    if (buf->ref_count > 1) {
        buf->ref_count--;
        return;
    }

    buf->ref_count = 0;

    if (buf->flags & PKT_BUF_FLAG_POOL) {
        for (size_t i = 0; i < PKT_BUF_POOL_SIZE; i++) {
            if (&g_pkt_buf_pool[i].buf_struct == buf) {
                buf->head     = NULL;
                buf->data     = NULL;
                buf->tail     = NULL;
                buf->end      = NULL;
                buf->capacity = 0;
                buf->len      = 0;
                g_pkt_buf_pool[i].in_use = false;
                return;
            }
        }
    } else if (buf->flags & PKT_BUF_FLAG_DYNAMIC) {
        if (buf->free_fn != NULL) {
            buf->free_fn(buf);
        } else if (g_pkt_buf_free_fn != NULL) {
            g_pkt_buf_free_fn(buf);
        }
    }
}

/* ── Pointer Manipulation API ──────────────────────────────────────────── */

uint8_t *packet_buffer_push(PacketBuffer *buf, size_t size)
{
    if (buf == NULL) {
        return NULL;
    }
    if (size == 0) {
        return buf->data;
    }

    /* Safety checks against bounds and integer underflow/overflow */
    if (buf->data < buf->head || (size_t)(buf->data - buf->head) < size) {
        return NULL; /* Out-of-bounds check against head pointer failed */
    }
    if (buf->len + size < buf->len) {
        return NULL; /* Integer overflow on length */
    }

    buf->data -= size;
    buf->len  += size;
    return buf->data;
}

uint8_t *packet_buffer_pull(PacketBuffer *buf, size_t size)
{
    if (buf == NULL) {
        return NULL;
    }
    if (size == 0) {
        return buf->data;
    }

    /* Safety checks against tail pointer and valid data length */
    if (buf->data > buf->tail || (size_t)(buf->tail - buf->data) < size) {
        return NULL; /* Out-of-bounds check against tail pointer failed */
    }
    if (buf->len < size) {
        return NULL;
    }

    buf->data += size;
    buf->len  -= size;
    return buf->data;
}

uint8_t *packet_buffer_put(PacketBuffer *buf, size_t size)
{
    if (buf == NULL) {
        return NULL;
    }
    if (size == 0) {
        return buf->tail;
    }

    /* Safety checks against end pointer and integer overflow */
    if (buf->tail > buf->end || (size_t)(buf->end - buf->tail) < size) {
        return NULL; /* Out-of-bounds check against end pointer failed */
    }
    if (buf->len + size < buf->len) {
        return NULL; /* Integer overflow on length */
    }

    uint8_t *old_tail = buf->tail;
    buf->tail += size;
    buf->len  += size;
    return old_tail;
}

/* ── Auxiliary Helper & Zero-Copy API ──────────────────────────────────── */

bool packet_buffer_reserve(PacketBuffer *buf, size_t len)
{
    if (buf == NULL) {
        return false;
    }
    /* Can only reserve headroom cleanly when valid payload length is 0 */
    if (buf->len != 0) {
        return false;
    }
    if (buf->data < buf->head || buf->data > buf->end) {
        return false;
    }
    if ((size_t)(buf->end - buf->data) < len) {
        return false;
    }

    buf->data += len;
    buf->tail  = buf->data;
    return true;
}

bool packet_buffer_trim(PacketBuffer *buf, size_t len)
{
    if (buf == NULL) {
        return false;
    }
    if (buf->len > len) {
        buf->tail = buf->data + len;
        buf->len  = len;
    }
    return true;
}

PacketBuffer *packet_buffer_reference(PacketBuffer *buf)
{
    if (buf == NULL) {
        return NULL;
    }
    /* Check for reference counter overflow */
    if (buf->ref_count == (uint16_t)(-1)) {
        return NULL;
    }
    buf->ref_count++;
    return buf;
}

void packet_buffer_reset(PacketBuffer *buf)
{
    if (buf == NULL) {
        return;
    }
    size_t headroom = (buf->capacity >= PKT_BUF_DEFAULT_HEADROOM)
                      ? PKT_BUF_DEFAULT_HEADROOM
                      : 0;
    buf->data = buf->head + headroom;
    buf->tail = buf->data;
    buf->len  = 0;
}

size_t packet_buffer_headroom(const PacketBuffer *buf)
{
    if (buf == NULL || buf->data < buf->head) {
        return 0;
    }
    return (size_t)(buf->data - buf->head);
}

size_t packet_buffer_tailroom(const PacketBuffer *buf)
{
    if (buf == NULL || buf->tail > buf->end) {
        return 0;
    }
    return (size_t)(buf->end - buf->tail);
}

size_t packet_buffer_length(const PacketBuffer *buf)
{
    if (buf == NULL) {
        return 0;
    }
    return buf->len;
}

bool packet_buffer_copy_from(PacketBuffer *buf, const void *src, size_t len)
{
    if (buf == NULL || src == NULL) {
        return false;
    }
    if (len == 0) {
        return true;
    }
    uint8_t *dest = packet_buffer_put(buf, len);
    if (dest == NULL) {
        return false;
    }
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < len; i++) {
        dest[i] = s[i];
    }
    return true;
}

size_t packet_buffer_copy_to(const PacketBuffer *buf, void *dst, size_t len)
{
    if (buf == NULL || dst == NULL || buf->data == NULL) {
        return 0;
    }
    size_t copy_len = (len < buf->len) ? len : buf->len;
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < copy_len; i++) {
        d[i] = buf->data[i];
    }
    return copy_len;
}

/* ── Self-Test & Verification Suite ────────────────────────────────────── */

bool packet_buffer_selftest(void)
{
    /* 1. Test basic allocation and initial headroom reservation */
    PacketBuffer *buf = packet_buffer_allocate(512);
    if (buf == NULL) {
        return false;
    }
    if (packet_buffer_headroom(buf) != PKT_BUF_DEFAULT_HEADROOM) {
        packet_buffer_free(buf);
        return false;
    }
    if (packet_buffer_length(buf) != 0) {
        packet_buffer_free(buf);
        return false;
    }
    if (packet_buffer_tailroom(buf) < 512) {
        packet_buffer_free(buf);
        return false;
    }

    /* 2. Test put (appending payload) */
    uint8_t payload[64];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i & 0xFF);
    }
    uint8_t *tail_ptr = packet_buffer_put(buf, sizeof(payload));
    if (tail_ptr == NULL || packet_buffer_length(buf) != sizeof(payload)) {
        packet_buffer_free(buf);
        return false;
    }
    for (size_t i = 0; i < sizeof(payload); i++) {
        tail_ptr[i] = payload[i];
    }

    /* 3. Test push (prepending header) */
    uint8_t *hdr_ptr = packet_buffer_push(buf, 14); /* 14-byte Ethernet header */
    if (hdr_ptr == NULL || packet_buffer_length(buf) != sizeof(payload) + 14) {
        packet_buffer_free(buf);
        return false;
    }
    if (packet_buffer_headroom(buf) != PKT_BUF_DEFAULT_HEADROOM - 14) {
        packet_buffer_free(buf);
        return false;
    }
    /* Write dummy header */
    for (size_t i = 0; i < 14; i++) {
        hdr_ptr[i] = 0xAA;
    }

    /* 4. Test pull (stripping header) */
    uint8_t *pulled_ptr = packet_buffer_pull(buf, 14);
    if (pulled_ptr == NULL || packet_buffer_length(buf) != sizeof(payload)) {
        packet_buffer_free(buf);
        return false;
    }
    if (pulled_ptr[0] != payload[0]) {
        packet_buffer_free(buf);
        return false;
    }

    /* 5. Test out-of-bounds safety checks */
    size_t excessive_headroom = packet_buffer_headroom(buf) + 10;
    if (packet_buffer_push(buf, excessive_headroom) != NULL) {
        packet_buffer_free(buf);
        return false; /* Should have failed due to head pointer check */
    }

    size_t excessive_tailroom = packet_buffer_tailroom(buf) + 10;
    if (packet_buffer_put(buf, excessive_tailroom) != NULL) {
        packet_buffer_free(buf);
        return false; /* Should have failed due to end pointer check */
    }

    size_t excessive_pull = packet_buffer_length(buf) + 1;
    if (packet_buffer_pull(buf, excessive_pull) != NULL) {
        packet_buffer_free(buf);
        return false; /* Should have failed due to tail pointer check */
    }

    /* 6. Test reference counting and zero-copy release */
    PacketBuffer *ref = packet_buffer_reference(buf);
    if (ref != buf || buf->ref_count != 2) {
        packet_buffer_free(buf);
        return false;
    }
    packet_buffer_free(buf); /* Should decrement ref_count to 1 without freeing */
    if (buf->ref_count != 1) {
        return false;
    }
    packet_buffer_free(buf); /* Should now fully release buffer back to pool */

    return true;
}
