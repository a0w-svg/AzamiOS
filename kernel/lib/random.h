/* ============================================================================
 * AzamiOS — Kernel CSPRNG
 * File: kernel/lib/random.h
 *
 * A ChaCha20 fast-key-erasure CSPRNG, seeded from RDSEED/RDRAND + timestamp +
 * address-space entropy. Backs getrandom(2), /dev/random, /dev/urandom and the
 * ELF loader's AT_RANDOM / stack-canary seed.
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

/** krandom_init() — Seed the CSPRNG. Safe to call once, early in boot.
 *  krandom_bytes() will lazily self-seed if called first. */
void krandom_init(void);

/** krandom_bytes(buf, n) — Fill buf with n cryptographically-strong bytes. */
void krandom_bytes(void *buf, size_t n);

/** krandom_u64() — Return a random 64-bit value. */
u64 krandom_u64(void);

/** krandom_add_entropy(buf, n) — Mix caller-supplied entropy into the pool
 *  (e.g. from /dev/random writes or interrupt timing jitter). */
void krandom_add_entropy(const void *buf, size_t n);
