/* ============================================================================
 * AzamiOS — Font Subsystem Header & Binary Font Specification
 * File: userland/libc/include/azami/font.h
 *
 * Defines the native Azami Font Format (AZF v1), Linux PC Screen Font (PSF)
 * compatibility structures, and the high-performance font management and
 * rendering API.
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Standard File Locations on Drive ─────────────────────────────────────── */
#define AZ_FONT_DIR_SYSTEM     "/usr/share/fonts"
#define AZ_FONT_DIR_PERSISTENT "/hdd/fonts"
#define AZ_FONT_CONFIG_FILE    "/etc/font.conf"
#define AZ_FONT_CONFIG_HDD     "/hdd/etc/font.conf"

/* ── AZF v1 (Azami Font Format) Binary Header ─────────────────────────────── */
#define AZF_MAGIC       0x4E465A41 /* "AZFN" in Little-Endian */
#define AZF_VERSION     1

#define AZF_FLAG_MONO   (1 << 0)
#define AZF_FLAG_BOLD   (1 << 1)
#define AZF_FLAG_ITALIC (1 << 2)
#define AZF_FLAG_SERIF  (1 << 3)

typedef struct {
    uint32_t magic;             /* 0x4E465A41 ("AZFN") */
    uint16_t version;           /* 1 */
    uint16_t header_sz;         /* sizeof(azf_header_t) = 68 bytes */
    char     family[32];        /* NUL-terminated family name e.g. "Terminus", "VGA" */
    char     style[16];         /* NUL-terminated style name e.g. "Regular", "Bold" */
    uint8_t  glyph_w;           /* Glyph width in pixels (e.g. 8) */
    uint8_t  glyph_h;           /* Glyph height in pixels (e.g. 16, 8, 14, 20) */
    uint8_t  first_char;        /* First ASCII character (typically 0x20 or 0x00) */
    uint8_t  last_char;         /* Last ASCII character (typically 0x7E or 0xFF) */
    uint16_t num_glyphs;        /* Total glyph count */
    uint16_t bytes_per_glyph;   /* bytes per glyph: glyph_h * ((glyph_w + 7) / 8) */
    uint32_t flags;             /* AZF_FLAG_* */
} __attribute__((packed)) azf_header_t;

/* ── Linux PC Screen Font (PSF1 & PSF2) Compatibility Headers ─────────────── */
#define PSF1_MAGIC      0x0436
#define PSF1_MODE512    0x01

typedef struct {
    uint16_t magic;             /* 0x0436 */
    uint8_t  mode;              /* 0: 256 glyphs, 1: 512 glyphs */
    uint8_t  charsize;          /* Height in pixels (width is fixed to 8) */
} __attribute__((packed)) psf1_header_t;

#define PSF2_MAGIC      0x864AB572
#define PSF2_FLAG_UNICODE 0x01

typedef struct {
    uint32_t magic;             /* 0x864AB572 */
    uint32_t version;           /* 0 */
    uint32_t headersize;        /* Total header size */
    uint32_t flags;             /* Flags */
    uint32_t length;            /* Number of glyphs */
    uint32_t charsize;          /* Bytes per glyph */
    uint32_t height;            /* Height in pixels */
    uint32_t width;             /* Width in pixels */
} __attribute__((packed)) psf2_header_t;

/* ── Runtime Font Instance ─────────────────────────────────────────────────── */
typedef struct az_font {
    char     name[32];          /* Family name (e.g. "Terminus", "VGA") */
    char     style[16];         /* Style (e.g. "Regular", "Bold", "Small") */
    char     path[128];         /* Source file path or "(builtin)" */
    uint8_t  glyph_w;           /* Width in pixels */
    uint8_t  glyph_h;           /* Height in pixels */
    uint8_t  first_char;        /* First ASCII character code */
    uint8_t  last_char;         /* Last ASCII character code */
    uint16_t num_glyphs;        /* Total glyph count */
    uint16_t bytes_per_glyph;   /* Bytes per glyph */
    uint32_t flags;             /* AZF_FLAG_* */
    const unsigned char *glyph_data; /* Raw bitmap rows, MSB = leftmost pixel */
    bool     is_heap;           /* True if glyph_data was allocated dynamically */
} az_font_t;

/* ── Font Directory Scan Info ──────────────────────────────────────────────── */
typedef struct {
    char     name[32];
    char     style[16];
    char     path[128];
    uint8_t  glyph_w;
    uint8_t  glyph_h;
    uint32_t flags;
    size_t   file_size;
} az_font_info_t;

/* ── Font Subsystem API ────────────────────────────────────────────────────── */

/**
 * Load an AZF or PSF font from a file path on drive.
 * Returns a dynamically allocated az_font_t on success, or NULL on error.
 */
az_font_t *az_font_load(const char *path);

/**
 * Load the active system font:
 * 1. Checks /hdd/etc/font.conf (if SATA persistent drive is mounted)
 * 2. Checks /etc/font.conf
 * 3. Checks /usr/share/fonts/vga_regular.azf
 * 4. Falls back to embedded in-memory fallback font.
 * Guaranteed to return a valid font pointer (never returns NULL).
 */
az_font_t *az_font_load_default(void);

/**
 * Load a font by name or family (e.g. "terminus", "terminus_bold", "vga_small")
 * by searching standard directories (/usr/share/fonts, /hdd/fonts).
 */
az_font_t *az_font_load_by_name(const char *name);

/**
 * Free a font previously returned by az_font_load() or az_font_load_by_name().
 * Does not free static fallback fonts.
 */
void az_font_free(az_font_t *font);

/**
 * Get pointer to raw row bitmap for character `c`.
 * Returns pointer to glyph bitmap, or NULL/fallback if unavailable.
 */
const unsigned char *az_font_get_glyph(const az_font_t *font, int c);

/**
 * Blit a single character into an ARGB pixel buffer with strict boundary clipping.
 */
void az_font_draw_char(
    uint32_t *buf, uint32_t pitch_px,
    uint32_t buf_w, uint32_t buf_h,
    int px, int py, char c, uint32_t color,
    const az_font_t *font, int scale, bool bold);

/**
 * Blit a NUL-terminated string into an ARGB pixel buffer.
 */
void az_font_draw_str(
    uint32_t *buf, uint32_t pitch_px,
    uint32_t buf_w, uint32_t buf_h,
    int px, int py, const char *s, uint32_t color,
    const az_font_t *font, int scale, bool bold);

/**
 * Compute the rendered pixel width of a string.
 */
int az_font_str_width(const az_font_t *font, const char *s, int scale);

/**
 * Scan standard font directories (/usr/share/fonts, /hdd/fonts) and collect font metadata.
 * Returns total count of fonts found.
 */
int az_font_scan_dirs(az_font_info_t *out_list, int max_fonts);

/**
 * Set the system default font path in /etc/font.conf (and /hdd/etc/font.conf).
 */
int az_font_set_default_font(const char *font_path);

#ifdef __cplusplus
}
#endif
