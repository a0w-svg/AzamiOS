/* ============================================================================
 * AzamiOS Desktop Environment — Minimal PNG Loader
 * File: userland/apps/shared/png_decode.h
 *
 * Header-only PNG decoder for DE apps that want a real image file instead of
 * a hand-drawn icon or a procedurally-generated wallpaper — see uk_load_png()
 * at the bottom. Every function is static, matching the rest of this shared
 * header set (ui_kit.h, de_font.h): no separate translation unit, no ODR
 * risk from being included by more than one app.
 *
 * ── Inflate ───────────────────────────────────────────────────────────────
 * A PNG's pixel data is a zlib (RFC 1950) DEFLATE (RFC 1951) stream, split
 * across one or more IDAT chunks. The decompressor below started as
 * fs/squashfs/squashfs_zlib.h ported to userland types (uint8_t/uint16_t/
 * uint32_t in place of the kernel's u8/u16/u32) — same format, same wire-
 * format handling, so there was nothing to design there, only to re-host:
 * SquashFS blocks and PNG IDAT streams are both "a zlib stream some caller
 * already sized an output buffer for". See that file for the wire-format
 * commentary; it isn't repeated here. One thing was added on top of the
 * port: pngz_huff_decode_fast()'s lookahead table, since a PNG this size is
 * asked to decode synchronously and interactively (an app's own startup, a
 * wallpaper going up) in a way a filesystem block usually isn't — see the
 * Limitations note on it below.
 *
 * ── Limitations (deliberately out of scope, not accidentally missing) ────
 *  • Bit depth 8 only — no 1/2/4/16-bit-per-channel PNGs.
 *  • Color types 0 (grayscale), 2 (RGB), 4 (grayscale+alpha) and 6 (RGBA)
 *    only — no palette (color type 3, needs PLTE/tRNS plumbing this doesn't
 *    have) and no 16-bit-per-sample images.
 *  • No interlacing — Adam7 PNGs are rejected, not silently mis-decoded.
 *  • No chunk CRC verification, same tradeoff squashfs_zlib.h makes for its
 *    Adler-32: these are local app assets (icons, wallpapers), not untrusted
 *    network input, so the cost of checking buys little here. Every buffer
 *    access is still bounds-checked regardless — skipping the CRC means a
 *    corrupt file can decode to a garbled image, never an out-of-bounds read.
 *  • Huffman decode has a table-driven fast path (pngz_huff_decode_fast(),
 *    used by the literal/length and distance decode in pngz_inflate_block())
 *    for any code up to PNGZ_FASTBITS bits — the large majority in real
 *    data — falling back to pngz_huff_decode()'s one-bit-at-a-time walk only
 *    for the rare longer code. An earlier version used the bit-at-a-time
 *    path everywhere and took ~20-30 seconds to decode a 640x400 RGBA
 *    wallpaper in this environment; with the fast path it's well under a
 *    second. Still decode-once-and-cache territory (see
 *    wallpaper/main.c's wp_load_image_wallpaper() + g_has_image_wallpaper)
 *    rather than something to call per frame, but no longer for the same
 *    "it's just slow" reason.
 *
 * A file outside these limits makes uk_load_png() return false, exactly like
 * a missing file — callers already have to handle "no image", so a "can't
 * decode this one" folds into the same fallback path for free (see
 * uk_load_icon32() in ui_kit.h for the established pattern this follows).
 * ============================================================================ */
#pragma once

#include "../../libc/include/stdint.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/sys/syscall.h"

/* ============================================================================
 * zlib/DEFLATE inflate — ported from fs/squashfs/squashfs_zlib.h
 * ============================================================================ */

typedef struct {
    const uint8_t *src;
    size_t         src_len;
    size_t         src_pos;
    uint32_t       bits;
    uint32_t       bits_valid;
} pngz_bitrd_t;

static inline void pngz_br_init(pngz_bitrd_t *br, const uint8_t *src, size_t len)
{
    br->src = src;
    br->src_len = len;
    br->src_pos = 0;
    br->bits = 0;
    br->bits_valid = 0;
}

static inline int pngz_br_fill(pngz_bitrd_t *br, uint32_t need)
{
    while (br->bits_valid < need) {
        if (br->src_pos >= br->src_len) return -1;
        br->bits |= ((uint32_t)br->src[br->src_pos++]) << br->bits_valid;
        br->bits_valid += 8;
    }
    return 0;
}

static inline uint32_t pngz_br_peek(pngz_bitrd_t *br, uint32_t n)
{
    return br->bits & ((1u << n) - 1u);
}

static inline void pngz_br_consume(pngz_bitrd_t *br, uint32_t n)
{
    br->bits >>= n;
    br->bits_valid -= n;
}

static inline int pngz_br_read(pngz_bitrd_t *br, uint32_t n, uint32_t *out)
{
    if (n == 0) { *out = 0; return 0; }
    if (pngz_br_fill(br, n)) return -1;
    *out = pngz_br_peek(br, n);
    pngz_br_consume(br, n);
    return 0;
}

#define PNGZ_HUFF_MAX_BITS  15
#define PNGZ_HUFF_LITLEN    288
#define PNGZ_HUFF_DIST      32

/* Fast-path lookahead table width. Symbol frequencies in real DEFLATE data
 * skew heavily toward short codes (that's the whole point of Huffman
 * coding), so a 9-bit table's ~512 entries catch the large majority of
 * decodes in one lookup; the rare longer code falls back to
 * pngz_huff_decode()'s bit-at-a-time walk below. 9 was picked the usual
 * way this constant is picked elsewhere (zlib, puff.c): big enough to cover
 * almost everything, small enough that the table (a few KB per tree,
 * rebuilt for every dynamic-Huffman block) is cheap to construct. */
#define PNGZ_FASTBITS  9
#define PNGZ_FAST_SIZE (1 << PNGZ_FASTBITS)

typedef struct {
    uint16_t counts[PNGZ_HUFF_MAX_BITS + 1];
    uint16_t symbols[PNGZ_HUFF_LITLEN];
    int      n_sym;
    /* Lookahead table indexed by the next PNGZ_FASTBITS bits of the stream
     * (already bit-reversed at build time — see pngz_huff_build() — so a
     * plain LSB-first peek can index it directly). fast_len[i] == 0 means
     * "the code starting here is longer than PNGZ_FASTBITS bits, use the
     * slow decoder"; otherwise fast_len[i]/fast_sym[i] give the length to
     * consume and the symbol, in one lookup instead of a bit-by-bit walk. */
    uint8_t  fast_len[PNGZ_FAST_SIZE];
    uint16_t fast_sym[PNGZ_FAST_SIZE];
} pngz_huff_t;

static int pngz_huff_build(pngz_huff_t *t, const uint8_t *lens, int n)
{
    uint16_t offs[PNGZ_HUFF_MAX_BITS + 1];

    memset(t->counts, 0, sizeof(t->counts));
    t->n_sym = n;

    for (int i = 0; i < n; i++) {
        if (lens[i] > PNGZ_HUFF_MAX_BITS) return -1;
        t->counts[lens[i]]++;
    }
    t->counts[0] = 0;

    uint16_t off = 0;
    for (int b = 1; b <= PNGZ_HUFF_MAX_BITS; b++) {
        offs[b] = off;
        off = (uint16_t)(off + t->counts[b]);
    }

    for (int i = 0; i < n; i++) {
        uint8_t len = lens[i];
        if (len == 0) continue;
        t->symbols[offs[len]++] = (uint16_t)i;
    }

    /*
     * Fast table. Canonical Huffman codes are assigned per RFC 1951 §3.2.2:
     * shortest-length symbols first, in symbol order within a length, each
     * length's starting code derived from a running total of shorter
     * codes. That gives every symbol an explicit MSB-first bit pattern —
     * but pngz_br_peek() hands back the next bits LSB-first (bit 0 of the
     * result is the very next bit in the stream), so a code has to be
     * *bit-reversed* before it can be compared against raw lookahead: the
     * reversed pattern is what the low bits of an LSB-first read actually
     * spell out when that code is next in the stream.
     *
     * Once reversed, a length-`len` code's table entry isn't unique — the
     * PNGZ_FASTBITS-len bits above it belong to whatever comes after this
     * code, not to it — so it's written to every slot sharing those low
     * `len` bits, all 2^(PNGZ_FASTBITS-len) of them.
     */
    memset(t->fast_len, 0, sizeof(t->fast_len));
    {
        uint32_t next_code[PNGZ_HUFF_MAX_BITS + 1];
        uint32_t code = 0;
        next_code[0] = 0;
        for (int b = 1; b <= PNGZ_HUFF_MAX_BITS; b++) {
            code = (code + t->counts[b - 1]) << 1;
            next_code[b] = code;
        }
        for (int i = 0; i < n; i++) {
            uint8_t len = lens[i];
            if (len == 0 || len > PNGZ_FASTBITS) continue;
            uint32_t c = next_code[len]++;
            uint32_t rev = 0;
            for (int b = 0; b < len; b++) rev |= ((c >> b) & 1u) << (len - 1 - b);
            for (uint32_t idx = rev; idx < PNGZ_FAST_SIZE; idx += (1u << len)) {
                t->fast_len[idx] = len;
                t->fast_sym[idx] = (uint16_t)i;
            }
        }
    }
    return 0;
}

static int pngz_huff_decode(pngz_bitrd_t *br, const pngz_huff_t *t, uint32_t *sym_out)
{
    uint32_t code = 0;
    uint16_t first = 0;
    uint16_t idx = 0;

    for (int b = 1; b <= PNGZ_HUFF_MAX_BITS; b++) {
        uint32_t bit;
        if (pngz_br_read(br, 1, &bit)) return -1;
        code = (code << 1) | bit;
        uint16_t cnt = t->counts[b];
        if (code - first < cnt) {
            *sym_out = t->symbols[idx + (uint16_t)(code - first)];
            return 0;
        }
        idx = (uint16_t)(idx + cnt);
        first = (uint16_t)((first + cnt) << 1);
    }
    return -1;
}

/*
 * pngz_huff_decode_fast() — pngz_huff_decode(), sped up with the lookahead
 * table pngz_huff_build() built: one table lookup for the common case (a
 * code no longer than PNGZ_FASTBITS bits) instead of up to 15 single-bit
 * reads, each of which was its own function call. Falls back to the plain
 * bit-at-a-time decoder — still needed as the correct-but-slow path — both
 * for a genuinely long code and for not having a full lookahead's worth of
 * bits left near the end of the stream; either way the fallback is only
 * ever a handful of decodes per block, not the hot path.
 *
 * This is the actual reason a 640x400 wallpaper PNG went from ~20-30s to
 * decode down to a fraction of a second: almost every one of the (few
 * hundred thousand, for an image that size) Huffman symbols in the stream
 * hits this table lookup rather than pngz_huff_decode()'s loop.
 */
static inline int pngz_huff_decode_fast(pngz_bitrd_t *br, const pngz_huff_t *t, uint32_t *sym_out)
{
    if (pngz_br_fill(br, PNGZ_FASTBITS) == 0) {
        uint32_t idx = pngz_br_peek(br, PNGZ_FASTBITS);
        uint8_t len = t->fast_len[idx];
        if (len != 0) {
            pngz_br_consume(br, len);
            *sym_out = t->fast_sym[idx];
            return 0;
        }
    }
    return pngz_huff_decode(br, t, sym_out);
}

static void pngz_build_fixed(pngz_huff_t *lit, pngz_huff_t *dist)
{
    uint8_t lens[288];
    for (int i = 0;   i <= 143; i++) lens[i] = 8;
    for (int i = 144; i <= 255; i++) lens[i] = 9;
    for (int i = 256; i <= 279; i++) lens[i] = 7;
    for (int i = 280; i <= 287; i++) lens[i] = 8;
    pngz_huff_build(lit, lens, 288);

    for (int i = 0; i < 32; i++) lens[i] = 5;
    pngz_huff_build(dist, lens, 32);
}

static const uint8_t  g_pngz_len_extra[29]  = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t g_pngz_len_base[29]   = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t  g_pngz_dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};
static const uint16_t g_pngz_dist_base[30]  = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,
    8193,12289,16385,24577
};

static int pngz_inflate_block(pngz_bitrd_t *br,
                              const pngz_huff_t *lit, const pngz_huff_t *dist,
                              uint8_t *out, size_t out_len, size_t *pos)
{
    for (;;) {
        uint32_t sym;
        if (pngz_huff_decode_fast(br, lit, &sym)) return -1;

        if (sym < 256) {
            if (*pos >= out_len) return -1;
            out[(*pos)++] = (uint8_t)sym;
        } else if (sym == 256) {
            break;
        } else {
            uint32_t len_idx = sym - 257;
            if (len_idx >= 29) return -1;
            uint32_t extra, length, dist_sym, distance;
            if (pngz_br_read(br, g_pngz_len_extra[len_idx], &extra)) return -1;
            length = g_pngz_len_base[len_idx] + extra;

            if (pngz_huff_decode_fast(br, dist, &dist_sym)) return -1;
            if (dist_sym >= 30) return -1;
            if (pngz_br_read(br, g_pngz_dist_extra[dist_sym], &extra)) return -1;
            distance = g_pngz_dist_base[dist_sym] + extra;

            if (distance > *pos) return -1;
            size_t src_off = *pos - distance;
            for (uint32_t i = 0; i < length; i++) {
                if (*pos >= out_len) return -1;
                out[*pos] = out[src_off + i];
                (*pos)++;
            }
        }
    }
    return 0;
}

static const uint8_t g_pngz_clcl_order[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

static int pngz_inflate_dynamic(pngz_bitrd_t *br, pngz_huff_t *lit, pngz_huff_t *dist)
{
    uint32_t hlit, hdist, hclen;
    if (pngz_br_read(br, 5, &hlit))  return -1;
    if (pngz_br_read(br, 5, &hdist)) return -1;
    if (pngz_br_read(br, 4, &hclen)) return -1;
    hlit  += 257;
    hdist += 1;
    hclen += 4;

    uint8_t clcl_lens[19];
    memset(clcl_lens, 0, sizeof(clcl_lens));
    for (uint32_t i = 0; i < hclen; i++) {
        uint32_t v;
        if (pngz_br_read(br, 3, &v)) return -1;
        clcl_lens[g_pngz_clcl_order[i]] = (uint8_t)v;
    }

    pngz_huff_t clcl;
    if (pngz_huff_build(&clcl, clcl_lens, 19)) return -1;

    uint8_t lens[PNGZ_HUFF_LITLEN + PNGZ_HUFF_DIST];
    memset(lens, 0, sizeof(lens));
    uint32_t total = hlit + hdist;
    uint32_t i = 0;
    while (i < total) {
        uint32_t sym;
        if (pngz_huff_decode(br, &clcl, &sym)) return -1;
        if (sym <= 15) {
            lens[i++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (i == 0) return -1;
            uint32_t rep;
            if (pngz_br_read(br, 2, &rep)) return -1;
            rep += 3;
            uint8_t prev = lens[i - 1];
            for (uint32_t j = 0; j < rep && i < total; j++) lens[i++] = prev;
        } else if (sym == 17) {
            uint32_t rep;
            if (pngz_br_read(br, 3, &rep)) return -1;
            rep += 3;
            for (uint32_t j = 0; j < rep && i < total; j++) lens[i++] = 0;
        } else if (sym == 18) {
            uint32_t rep;
            if (pngz_br_read(br, 7, &rep)) return -1;
            rep += 11;
            for (uint32_t j = 0; j < rep && i < total; j++) lens[i++] = 0;
        } else {
            return -1;
        }
    }

    if (pngz_huff_build(lit,  lens,        (int)hlit))  return -1;
    if (pngz_huff_build(dist, lens + hlit, (int)hdist)) return -1;
    return 0;
}

static int pngz_zlib_decompress(const void *in,  size_t in_len,
                                void       *out, size_t out_len,
                                size_t     *out_actual)
{
    const uint8_t *src = (const uint8_t *)in;
    uint8_t       *dst = (uint8_t *)out;

    if (in_len < 2) return -1;
    uint8_t cmf = src[0], flg = src[1];

    uint8_t cm    =  cmf & 0x0Fu;
    uint8_t cinfo = (uint8_t)((cmf >> 4) & 0x0Fu);
    if (cm != 8 || cinfo > 7) return -1;
    if ((((uint32_t)cmf * 256u) + flg) % 31u != 0u) return -1;

    size_t hdr_skip = 2;
    if (flg & 0x20u) {
        if (in_len < 6) return -1;
        hdr_skip += 4;
    }

    pngz_bitrd_t br;
    pngz_br_init(&br, src + hdr_skip, in_len - hdr_skip);

    size_t pos = 0;
    int bfinal;
    do {
        uint32_t val;
        if (pngz_br_read(&br, 1, &val)) return -1;
        bfinal = (int)val;

        uint32_t btype;
        if (pngz_br_read(&br, 2, &btype)) return -1;

        if (btype == 0) {
            br.bits = 0;
            br.bits_valid = 0;

            if (br.src_pos + 4 > br.src_len) return -1;
            uint16_t blen = (uint16_t)((uint32_t)br.src[br.src_pos]
                           | ((uint32_t)br.src[br.src_pos + 1] << 8));
            uint16_t nlen = (uint16_t)((uint32_t)br.src[br.src_pos + 2]
                           | ((uint32_t)br.src[br.src_pos + 3] << 8));
            br.src_pos += 4;
            if ((uint16_t)(blen ^ nlen) != 0xFFFFu) return -1;
            if (br.src_pos + blen > br.src_len) return -1;
            if (pos + blen > out_len) return -1;
            memcpy(dst + pos, br.src + br.src_pos, blen);
            br.src_pos += blen;
            pos        += blen;

        } else if (btype == 1) {
            pngz_huff_t lit, dist;
            pngz_build_fixed(&lit, &dist);
            if (pngz_inflate_block(&br, &lit, &dist, dst, out_len, &pos)) return -1;

        } else if (btype == 2) {
            pngz_huff_t lit, dist;
            if (pngz_inflate_dynamic(&br, &lit, &dist)) return -1;
            if (pngz_inflate_block(&br, &lit, &dist, dst, out_len, &pos)) return -1;

        } else {
            return -1;
        }
    } while (!bfinal);

    if (out_actual) *out_actual = pos;
    return 0;
}

/* ============================================================================
 * PNG container: chunk walk, un-filtering, sample expansion to ARGB
 * ============================================================================ */

static inline uint32_t png_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* Paeth predictor, RFC 2083 §6.6. */
static inline uint8_t png_paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    if (pb <= pc) return (uint8_t)b;
    return (uint8_t)c;
}

/*
 * uk_load_png(path, out_pixels, out_w, out_h) — decode a PNG file into a
 * freshly malloc()'d array of 0xAARRGGBB pixels (free with uk_free_png()).
 *
 * Returns true and fills out_pixels/out_w/out_h on success. Returns
 * false — leaving the outputs untouched — for a missing file, a malformed
 * one, or one outside this decoder's supported subset (see the file header
 * for exactly what that excludes); callers already have a "no image, draw
 * something else" path for a missing file, and this folds "can't decode
 * this one" into the same path rather than needing a second one.
 */
static inline bool uk_load_png(const char *path, unsigned int **out_pixels,
                               int *out_w, int *out_h)
{
    int fd = sys_open(path, 0, 0);
    if (fd < 0) return false;

    /* Read the whole file. Grown in doubling chunks starting at 64 KiB —
     * generous for an icon, a few reallocs for a large wallpaper, and never
     * a fixed cap a bigger asset would silently truncate against. */
    size_t cap = 65536, len = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) { sys_close(fd); return false; }
    for (;;) {
        if (len == cap) {
            size_t ncap = cap * 2;
            uint8_t *nb = (uint8_t *)realloc(buf, ncap);
            if (!nb) { free(buf); sys_close(fd); return false; }
            buf = nb;
            cap = ncap;
        }
        ssize_t n = sys_read(fd, buf + len, cap - len);
        if (n < 0) { free(buf); sys_close(fd); return false; }
        if (n == 0) break;
        len += (size_t)n;
    }
    sys_close(fd);

    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (len < 8 || memcmp(buf, sig, 8) != 0) { free(buf); return false; }

    /* ── Chunk walk: pull IHDR, concatenate IDAT ────────────────────────── */
    uint32_t width = 0, height = 0;
    uint8_t bit_depth = 0, color_type = 0xFF, interlace = 0;
    uint8_t *idat = NULL;
    size_t idat_len = 0, idat_cap = 0;
    bool have_ihdr = false, have_iend = false;

    size_t off = 8;
    while (off + 8 <= len) {
        uint32_t clen = png_be32(buf + off);
        const uint8_t *ctype = buf + off + 4;
        const uint8_t *cdata = buf + off + 8;
        if (off + 12 + (size_t)clen > len) break; /* truncated chunk */

        if (memcmp(ctype, "IHDR", 4) == 0 && clen >= 13) {
            width      = png_be32(cdata);
            height     = png_be32(cdata + 4);
            bit_depth  = cdata[8];
            color_type = cdata[9];
            /* cdata[10] = compression method, [11] = filter method — both
             * always 0 in every PNG that exists; not worth rejecting on. */
            interlace  = cdata[12];
            have_ihdr = true;
        } else if (memcmp(ctype, "IDAT", 4) == 0) {
            if (idat_len + clen > idat_cap) {
                size_t ncap = idat_cap ? idat_cap * 2 : 65536;
                while (ncap < idat_len + clen) ncap *= 2;
                uint8_t *nb = (uint8_t *)realloc(idat, ncap);
                if (!nb) { free(idat); free(buf); return false; }
                idat = nb;
                idat_cap = ncap;
            }
            memcpy(idat + idat_len, cdata, clen);
            idat_len += clen;
        } else if (memcmp(ctype, "IEND", 4) == 0) {
            have_iend = true;
            break;
        }
        /* Anything else (PLTE without support for it, tEXt, pHYs, gAMA,
         * tRNS, ...) is ancillary to this decoder and skipped. */
        off += 12 + (size_t)clen;
    }
    free(buf);

    if (!have_ihdr || !have_iend || !idat || width == 0 || height == 0) {
        free(idat);
        return false;
    }
    if (bit_depth != 8 || interlace != 0) { free(idat); return false; }

    int channels;
    switch (color_type) {
    case 0: channels = 1; break; /* grayscale */
    case 2: channels = 3; break; /* RGB */
    case 4: channels = 2; break; /* grayscale + alpha */
    case 6: channels = 4; break; /* RGBA */
    default: free(idat); return false; /* palette or unknown */
    }

    /* ── Inflate ─────────────────────────────────────────────────────────── */
    size_t stride = (size_t)width * (size_t)channels;
    size_t raw_len = (stride + 1) * (size_t)height; /* +1 filter-type byte/row */
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) { free(idat); return false; }

    size_t raw_actual = 0;
    int rc = pngz_zlib_decompress(idat, idat_len, raw, raw_len, &raw_actual);
    free(idat);
    if (rc != 0 || raw_actual != raw_len) { free(raw); return false; }

    /* ── Un-filter each scanline in place (RFC 2083 §6) ─────────────────── */
    uint8_t *prev_row = NULL; /* all-zero for row 0, per spec */
    for (uint32_t y = 0; y < height; y++) {
        uint8_t *row = raw + y * (stride + 1);
        uint8_t filter = row[0];
        uint8_t *cur = row + 1;

        for (size_t x = 0; x < stride; x++) {
            int a = (x >= (size_t)channels) ? cur[x - channels] : 0;
            int b = prev_row ? prev_row[x] : 0;
            int c = (prev_row && x >= (size_t)channels) ? prev_row[x - channels] : 0;
            switch (filter) {
            case 0: break; /* None */
            case 1: cur[x] = (uint8_t)(cur[x] + a); break;              /* Sub */
            case 2: cur[x] = (uint8_t)(cur[x] + b); break;              /* Up */
            case 3: cur[x] = (uint8_t)(cur[x] + (a + b) / 2); break;    /* Average */
            case 4: cur[x] = (uint8_t)(cur[x] + png_paeth(a, b, c)); break; /* Paeth */
            default: free(raw); return false;
            }
        }
        prev_row = cur;
    }

    /* ── Expand samples to 0xAARRGGBB ───────────────────────────────────── */
    unsigned int *pixels = (unsigned int *)malloc((size_t)width * height * sizeof(unsigned int));
    if (!pixels) { free(raw); return false; }

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *src = raw + y * (stride + 1) + 1;
        unsigned int *dst = pixels + (size_t)y * width;
        for (uint32_t x = 0; x < width; x++) {
            switch (color_type) {
            case 0: {
                uint8_t g = src[x];
                dst[x] = 0xFF000000u | ((unsigned int)g << 16) | ((unsigned int)g << 8) | g;
                break;
            }
            case 2: {
                const uint8_t *p = src + x * 3;
                dst[x] = 0xFF000000u | ((unsigned int)p[0] << 16) |
                         ((unsigned int)p[1] << 8) | p[2];
                break;
            }
            case 4: {
                const uint8_t *p = src + x * 2;
                dst[x] = ((unsigned int)p[1] << 24) | ((unsigned int)p[0] << 16) |
                         ((unsigned int)p[0] << 8) | p[0];
                break;
            }
            default: { /* 6: RGBA */
                const uint8_t *p = src + x * 4;
                dst[x] = ((unsigned int)p[3] << 24) | ((unsigned int)p[0] << 16) |
                         ((unsigned int)p[1] << 8) | p[2];
                break;
            }
            }
        }
    }
    free(raw);

    *out_pixels = pixels;
    *out_w = (int)width;
    *out_h = (int)height;
    return true;
}

static inline void uk_free_png(unsigned int *pixels)
{
    free(pixels);
}

/* uk_draw_png() — an alpha-blitting drawer for a decoded image onto a
 * uk_window_t lives in ui_kit.h, not here: this file only ever deals in raw
 * ARGB arrays and has no uk_window_t of its own to draw one onto. A caller
 * with a plain pixel buffer instead of a uk_window_t (the wallpaper app,
 * which owns its shared-memory surface directly — see wallpaper/main.c)
 * composites the array uk_load_png() returns itself, the same way it
 * already composites its own procedural background. */
