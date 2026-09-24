/* ============================================================================
 * AzamiOS Game Framework — Asset Manager
 * File: userland/libgame/include/game/game_asset.h
 *
 * Asset loading and management:
 *  • Named asset table (name → pointer map)
 *  • Image loading from raw ARGB32 data, PPM, and procedural generation
 *  • Asset lifecycle (load, get, free)
 *
 * Tuning defines:
 *   ASSET_MAX_ENTRIES — max loaded assets (default 64)
 *   ASSET_NAME_LEN   — max asset name length (default 32)
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifndef ASSET_MAX_ENTRIES
#define ASSET_MAX_ENTRIES 64
#endif
#ifndef ASSET_NAME_LEN
#define ASSET_NAME_LEN    32
#endif

/* ── Asset Types ──────────────────────────────────────────────────────────── */

typedef enum {
    ASSET_IMAGE,
    ASSET_SOUND,
    ASSET_DATA
} asset_type_t;

typedef struct {
    char        name[ASSET_NAME_LEN];
    asset_type_t type;
    void        *data;      /* type-specific data (owned, malloc'd) */
    int          width;     /* for images */
    int          height;    /* for images */
    int          size;      /* data size in bytes */
    bool         active;
} asset_t;

typedef struct {
    asset_t entries[ASSET_MAX_ENTRIES];
    int     count;
} asset_mgr_t;

/* ── API (implemented in game_asset.c) ────────────────────────────────────── */

void     asset_init(asset_mgr_t *mgr);
void     asset_free_all(asset_mgr_t *mgr);

/* Load raw ARGB32 pixel data as a named image asset. */
asset_t *asset_load_pixels(asset_mgr_t *mgr, const char *name,
                            uint32_t *pixels, int w, int h);

/* Load an image from a file (PPM P6 format). */
asset_t *asset_load_image(asset_mgr_t *mgr, const char *name, const char *path);

/* Load raw binary data as a named asset. */
asset_t *asset_load_data(asset_mgr_t *mgr, const char *name,
                          const void *data, int size);

/* Create a solid-color image asset (useful for prototyping). */
asset_t *asset_create_solid(asset_mgr_t *mgr, const char *name,
                             int w, int h, uint32_t color);

/* Create a checkerboard pattern image asset. */
asset_t *asset_create_checker(asset_mgr_t *mgr, const char *name,
                               int w, int h, int cell_size,
                               uint32_t color_a, uint32_t color_b);

/* Create a horizontal gradient image asset. */
asset_t *asset_create_gradient_h(asset_mgr_t *mgr, const char *name,
                                   int w, int h,
                                   uint32_t left_color, uint32_t right_color);

/* Lookup a loaded asset by name. */
asset_t *asset_get(asset_mgr_t *mgr, const char *name);

/* Free a single named asset. */
void     asset_free(asset_mgr_t *mgr, const char *name);
