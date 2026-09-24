/* ============================================================================
 * AzamiOS Userspace — SHA-256 (sha256.h)
 *
 * Shared by every account-management tool that needs to hash a password
 * against /etc/shadow (lockscreen.elf, passwd.elf, useradd.elf) so there is
 * one tested implementation instead of a fresh copy in each.
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <stddef.h>

/* Hashes `str` and writes 64 lowercase hex characters + NUL into out_hex. */
void sha256_hash_string(const char *str, char out_hex[65]);

/* Real random salt for a new /etc/shadow entry: `len` lowercase hex
 * characters (len must be even) + NUL into out_hex, from the kernel CSPRNG
 * (getrandom(2)) -- not anything guessable like the PID or the time. */
void sha256_gen_salt_hex(char *out_hex, size_t len);

/* Formats a real, salted /etc/shadow-style hash: "$sha256$<salt>$<hash>".
 * out_buf must be at least 8 + salt_len + 64 + 1 bytes; salt_len is
 * measured in hex characters (e.g. 16). */
void sha256_make_shadow_entry(const char *password, size_t salt_len, char *out_buf, size_t out_buf_len);

/* Verifies `password` against a stored /etc/shadow hash field: either a
 * bare 64-char hex digest or a "$sha256$salt$hash" salted entry (the same
 * two formats lockscreen.elf's own verifier accepts). Returns 1 on match,
 * 0 otherwise -- including for "!"/"x"/empty (locked/no-password account,
 * which never matches any password). Constant-time compare, zeroes its
 * scratch buffers before returning. */
int sha256_verify_shadow_hash(const char *stored_hash, const char *password);

/* Hashes the whole contents of the file at `path`, writing 64 lowercase hex
 * characters + NUL into out_hex. Returns 0, or -1 if the file could not be
 * opened or read (out_hex is left untouched then). Streams the file in
 * fixed-size chunks, so hashing a multi-megabyte package archive costs one
 * buffer, not its size in memory -- pkg.elf checks every archive it fetches
 * against the digest in the repository index this way. */
int sha256_hash_file(const char *path, char out_hex[65]);
