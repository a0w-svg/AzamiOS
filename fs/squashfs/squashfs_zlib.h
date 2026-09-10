/* ============================================================================
 * AzamiOS — SquashFS: Minimal self-contained zlib/DEFLATE decompressor
 * File: fs/squashfs/squashfs_zlib.h
 *
 * Design
 * ──────
 * This is a header-only, static-inline decompressor for zlib-wrapped DEFLATE
 * streams (RFC 1950 + RFC 1951).  It is intentionally self-contained so that
 * the SquashFS driver has no dependency on any external library.
 *
 * Limitations
 * ──────────
 *  • Only zlib (method 8) streams are accepted — no raw DEFLATE, no gzip.
 *  • No Adler-32 verification (saves ~20 lines in a hot path; the block
 *    device layer already CRCs sectors).
 *  • Maximum 32 KiB history window (the DEFLATE spec maximum).
 *  • No dynamic allocation on the hot path: the caller supplies the output
 *    buffer; the Huffman tree arrays live on the stack (< 6 KiB).
 *
 * Public API
 * ──────────
 *   int sqfs_zlib_decompress(const void *in,  size_t in_len,
 *                             void       *out, size_t out_len,
 *                             size_t     *out_actual);
 *   Returns 0 on success, -1 on error.
 *   *out_actual is set to the number of bytes written to out.
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

/* ── Bit-reader state ──────────────────────────────────────────────────────── */
typedef struct {
    const u8 *src;
    size_t    src_len;
    size_t    src_pos;
    u32       bits;       /* bit accumulator                */
    u32       bits_valid; /* how many bits are in bits      */
} sqfs_bitrd_t;

static inline void sqfs_br_init(sqfs_bitrd_t *br, const u8 *src, size_t len)
{
    br->src       = src;
    br->src_len   = len;
    br->src_pos   = 0;
    br->bits       = 0;
    br->bits_valid = 0;
}

static inline int sqfs_br_fill(sqfs_bitrd_t *br, u32 need)
{
    while (br->bits_valid < need) {
        if (br->src_pos >= br->src_len) return -1;
        br->bits |= ((u32)br->src[br->src_pos++]) << br->bits_valid;
        br->bits_valid += 8;
    }
    return 0;
}

static inline u32 sqfs_br_peek(sqfs_bitrd_t *br, u32 n)
{
    return br->bits & ((1u << n) - 1u);
}

static inline void sqfs_br_consume(sqfs_bitrd_t *br, u32 n)
{
    br->bits >>= n;
    br->bits_valid -= n;
}

static inline int sqfs_br_read(sqfs_bitrd_t *br, u32 n, u32 *out)
{
    if (n == 0) { *out = 0; return 0; }
    if (sqfs_br_fill(br, n)) return -1;
    *out = sqfs_br_peek(br, n);
    sqfs_br_consume(br, n);
    return 0;
}

/* ── Huffman table ─────────────────────────────────────────────────────────── */
#define SQFS_HUFF_MAX_BITS  15
#define SQFS_HUFF_LITLEN    288
#define SQFS_HUFF_DIST      32

typedef struct {
    u16 counts[SQFS_HUFF_MAX_BITS + 1]; /* number of codes for each length */
    u16 symbols[SQFS_HUFF_LITLEN];      /* symbols sorted by code           */
    int n_sym;
} sqfs_huff_t;

/* Build a canonical Huffman table from an array of code lengths. */
static int sqfs_huff_build(sqfs_huff_t *t, const u8 *lens, int n)
{
    u16 offs[SQFS_HUFF_MAX_BITS + 1];

    __builtin_memset(t->counts, 0, sizeof(t->counts));
    t->n_sym = n;

    for (int i = 0; i < n; i++) {
        if (lens[i] > SQFS_HUFF_MAX_BITS) return -1;
        t->counts[lens[i]]++;
    }
    t->counts[0] = 0;

    /* Compute offsets into the symbol array per bit-length */
    u16 off = 0;
    for (int b = 1; b <= SQFS_HUFF_MAX_BITS; b++) {
        offs[b] = off;
        off = (u16)(off + t->counts[b]);
    }

    for (int i = 0; i < n; i++) {
        u8 len = lens[i];
        if (len == 0) continue;
        t->symbols[offs[len]++] = (u16)i;
    }
    return 0;
}

/* Decode one symbol from the bit-reader using a canonical Huffman table. */
static int sqfs_huff_decode(sqfs_bitrd_t *br, const sqfs_huff_t *t, u32 *sym_out)
{
    u32  code  = 0;
    u16  first = 0;
    u16  idx   = 0;

    for (int b = 1; b <= SQFS_HUFF_MAX_BITS; b++) {
        u32 bit;
        if (sqfs_br_read(br, 1, &bit)) return -1;
        code = (code << 1) | bit;
        u16 cnt = t->counts[b];
        if (code - first < cnt) {
            *sym_out = t->symbols[idx + (u16)(code - first)];
            return 0;
        }
        idx   = (u16)(idx + cnt);
        first = (u16)((first + cnt) << 1);
    }
    return -1;
}

/* ── Fixed Huffman tables (RFC 1951 §3.2.6) ───────────────────────────────── */
static void sqfs_build_fixed(sqfs_huff_t *lit, sqfs_huff_t *dist)
{
    u8 lens[288];
    for (int i = 0;   i <= 143; i++) lens[i] = 8;
    for (int i = 144; i <= 255; i++) lens[i] = 9;
    for (int i = 256; i <= 279; i++) lens[i] = 7;
    for (int i = 280; i <= 287; i++) lens[i] = 8;
    sqfs_huff_build(lit, lens, 288);

    for (int i = 0; i < 32; i++) lens[i] = 5;
    sqfs_huff_build(dist, lens, 32);
}

/* ── DEFLATE length/distance extra bits tables ─────────────────────────────── */
static const u8  g_sqfs_len_extra[29]  = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const u16 g_sqfs_len_base[29]   = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const u8  g_sqfs_dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};
static const u16 g_sqfs_dist_base[30]  = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,
    8193,12289,16385,24577
};

/* ── Inflate one compressed or stored block ─────────────────────────────────── */
static int sqfs_inflate_block(sqfs_bitrd_t *br,
                               const sqfs_huff_t *lit, const sqfs_huff_t *dist,
                               u8 *out, size_t out_len, size_t *pos)
{
    for (;;) {
        u32 sym;
        if (sqfs_huff_decode(br, lit, &sym)) return -1;

        if (sym < 256) {
            if (*pos >= out_len) return -1;
            out[(*pos)++] = (u8)sym;
        } else if (sym == 256) {
            break; /* end of block */
        } else {
            u32 len_idx = sym - 257;
            if (len_idx >= 29) return -1;
            u32 extra, length, dist_sym, distance;
            if (sqfs_br_read(br, g_sqfs_len_extra[len_idx], &extra)) return -1;
            length = g_sqfs_len_base[len_idx] + extra;

            if (sqfs_huff_decode(br, dist, &dist_sym)) return -1;
            if (dist_sym >= 30) return -1;
            if (sqfs_br_read(br, g_sqfs_dist_extra[dist_sym], &extra)) return -1;
            distance = g_sqfs_dist_base[dist_sym] + extra;

            if (distance > *pos) return -1;
            size_t src_off = *pos - distance;
            for (u32 i = 0; i < length; i++) {
                if (*pos >= out_len) return -1;
                out[*pos] = out[src_off + i];
                (*pos)++;
            }
        }
    }
    return 0;
}

/* ── Dynamic Huffman table decoder ────────────────────────────────────────── */
static const u8 g_sqfs_clcl_order[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

static int sqfs_inflate_dynamic(sqfs_bitrd_t *br, sqfs_huff_t *lit, sqfs_huff_t *dist)
{
    u32 hlit, hdist, hclen;
    if (sqfs_br_read(br, 5, &hlit))  return -1;
    if (sqfs_br_read(br, 5, &hdist)) return -1;
    if (sqfs_br_read(br, 4, &hclen)) return -1;
    hlit  += 257;
    hdist += 1;
    hclen += 4;

    u8 clcl_lens[19];
    __builtin_memset(clcl_lens, 0, sizeof(clcl_lens));
    for (u32 i = 0; i < hclen; i++) {
        u32 v;
        if (sqfs_br_read(br, 3, &v)) return -1;
        clcl_lens[g_sqfs_clcl_order[i]] = (u8)v;
    }

    sqfs_huff_t clcl;
    if (sqfs_huff_build(&clcl, clcl_lens, 19)) return -1;

    u8 lens[SQFS_HUFF_LITLEN + SQFS_HUFF_DIST];
    __builtin_memset(lens, 0, sizeof(lens));
    u32 total = hlit + hdist;
    u32 i = 0;
    while (i < total) {
        u32 sym;
        if (sqfs_huff_decode(br, &clcl, &sym)) return -1;
        if (sym <= 15) {
            lens[i++] = (u8)sym;
        } else if (sym == 16) {
            if (i == 0) return -1;
            u32 rep;
            if (sqfs_br_read(br, 2, &rep)) return -1;
            rep += 3;
            u8 prev = lens[i - 1];
            for (u32 j = 0; j < rep && i < total; j++) lens[i++] = prev;
        } else if (sym == 17) {
            u32 rep;
            if (sqfs_br_read(br, 3, &rep)) return -1;
            rep += 3;
            for (u32 j = 0; j < rep && i < total; j++) lens[i++] = 0;
        } else if (sym == 18) {
            u32 rep;
            if (sqfs_br_read(br, 7, &rep)) return -1;
            rep += 11;
            for (u32 j = 0; j < rep && i < total; j++) lens[i++] = 0;
        } else {
            return -1;
        }
    }

    if (sqfs_huff_build(lit,  lens,        (int)hlit))  return -1;
    if (sqfs_huff_build(dist, lens + hlit, (int)hdist)) return -1;
    return 0;
}

/* ── Public entry point ────────────────────────────────────────────────────── */

/**
 * sqfs_zlib_decompress() — Decompress a zlib (RFC 1950) stream.
 *
 * @in         Pointer to compressed data (zlib header + DEFLATE payload).
 * @in_len     Length of compressed data in bytes.
 * @out        Output buffer (caller-provided, must be large enough).
 * @out_len    Size of output buffer in bytes.
 * @out_actual On success, set to the number of decompressed bytes.
 *
 * Returns 0 on success, -1 on any format or capacity error.
 */
static int sqfs_zlib_decompress(const void *in,  size_t in_len,
                                 void       *out, size_t out_len,
                                 size_t     *out_actual)
{
    const u8 *src = (const u8 *)in;
    u8       *dst = (u8 *)out;

    /* ── zlib header (2 bytes) ────────────────────────────────────────────── */
    if (in_len < 2) return -1;
    u8 cmf = src[0], flg = src[1];

    u8 cm    =  cmf & 0x0Fu;
    u8 cinfo = (u8)((cmf >> 4) & 0x0Fu);
    if (cm != 8 || cinfo > 7) return -1;
    if ((((u32)cmf * 256u) + flg) % 31u != 0u) return -1;

    size_t hdr_skip = 2;
    if (flg & 0x20u) {
        if (in_len < 6) return -1;
        hdr_skip += 4; /* skip preset-dictionary Adler-32 */
    }

    sqfs_bitrd_t br;
    sqfs_br_init(&br, src + hdr_skip, in_len - hdr_skip);

    size_t pos = 0;
    int bfinal;
    do {
        u32 val;
        if (sqfs_br_read(&br, 1, &val)) return -1;
        bfinal = (int)val;

        u32 btype;
        if (sqfs_br_read(&br, 2, &btype)) return -1;

        if (btype == 0) {
            /* Stored block: discard partial byte */
            br.bits       = 0;
            br.bits_valid  = 0;

            if (br.src_pos + 4 > br.src_len) return -1;
            u16 blen = (u16)((u32)br.src[br.src_pos]
                           | ((u32)br.src[br.src_pos + 1] << 8));
            u16 nlen = (u16)((u32)br.src[br.src_pos + 2]
                           | ((u32)br.src[br.src_pos + 3] << 8));
            br.src_pos += 4;
            if ((u16)(blen ^ nlen) != 0xFFFFu) return -1;
            if (br.src_pos + blen > br.src_len) return -1;
            if (pos + blen > out_len) return -1;
            __builtin_memcpy(dst + pos, br.src + br.src_pos, blen);
            br.src_pos += blen;
            pos        += blen;

        } else if (btype == 1) {
            sqfs_huff_t lit, dist;
            sqfs_build_fixed(&lit, &dist);
            if (sqfs_inflate_block(&br, &lit, &dist, dst, out_len, &pos)) return -1;

        } else if (btype == 2) {
            sqfs_huff_t lit, dist;
            if (sqfs_inflate_dynamic(&br, &lit, &dist)) return -1;
            if (sqfs_inflate_block(&br, &lit, &dist, dst, out_len, &pos)) return -1;

        } else {
            return -1; /* btype == 3: reserved/invalid */
        }
    } while (!bfinal);

    if (out_actual) *out_actual = pos;
    return 0;
}
