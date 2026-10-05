#pragma once
#include <stddef.h>
#include <stdint.h>

/* GNU ELF64 hash tables have no explicit dynsym count. The largest bucket
 * starts the last chain; its low-bit terminator marks the final symbol.
 * Bound every read by the containing PT_LOAD mapping. */
static inline uint32_t __elf_gnu_hash_count(const uint32_t *hash, size_t bytes)
{
    if (bytes < 4 * sizeof(uint32_t)) return 0;
    size_t words = bytes / sizeof(uint32_t);
    uint32_t buckets_count = hash[0], first_symbol = hash[1];
    uint32_t bloom_count = hash[2];
    if (!bloom_count || bloom_count > (words - 4) / 2) return 0;
    size_t buckets_offset = 4 + (size_t)bloom_count * 2;
    if (buckets_count > words - buckets_offset) return 0;
    const uint32_t *buckets = hash + buckets_offset;
    size_t chains_offset = buckets_offset + buckets_count;
    uint32_t last = 0;
    for (uint32_t i = 0; i < buckets_count; i++) {
        if (buckets[i] && buckets[i] < first_symbol) return 0;
        if (buckets[i] > last) last = buckets[i];
    }
    if (!last) return first_symbol;
    size_t chain = (size_t)last - first_symbol;
    size_t chain_count = words - chains_offset;
    while (chain < chain_count && last != UINT32_MAX) {
        if (hash[chains_offset + chain] & 1) return last + 1;
        chain++;
        last++;
    }
    return 0;
}
