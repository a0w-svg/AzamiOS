/* ============================================================================
 * AzamiOS Game Framework — Asset Manager Implementation
 * File: userland/libgame/game_asset.c
 * ============================================================================ */

#include "include/game/game_asset.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

/* ── Initialization ───────────────────────────────────────────────────────── */

void asset_init(asset_mgr_t *mgr)
{
    memset(mgr, 0, sizeof(*mgr));
}

void asset_free_all(asset_mgr_t *mgr)
{
    for (int i = 0; i < mgr->count; i++) {
        if (mgr->entries[i].active && mgr->entries[i].data) {
            free(mgr->entries[i].data);
            mgr->entries[i].data = NULL;
            mgr->entries[i].active = false;
        }
    }
    mgr->count = 0;
}

/* ── Internal: find or allocate slot ──────────────────────────────────────── */

static asset_t *asset_alloc_slot(asset_mgr_t *mgr, const char *name)
{
    /* Check if name already exists */
    for (int i = 0; i < mgr->count; i++) {
        if (mgr->entries[i].active &&
            strncmp(mgr->entries[i].name, name, ASSET_NAME_LEN - 1) == 0) {
            /* Free old data */
            if (mgr->entries[i].data) free(mgr->entries[i].data);
            memset(&mgr->entries[i], 0, sizeof(asset_t));
            strncpy(mgr->entries[i].name, name, ASSET_NAME_LEN - 1);
            return &mgr->entries[i];
        }
    }

    /* Find free slot */
    for (int i = 0; i < ASSET_MAX_ENTRIES; i++) {
        if (!mgr->entries[i].active) {
            memset(&mgr->entries[i], 0, sizeof(asset_t));
            strncpy(mgr->entries[i].name, name, ASSET_NAME_LEN - 1);
            if (i >= mgr->count) mgr->count = i + 1;
            return &mgr->entries[i];
        }
    }

    return NULL;
}

/* ── Load Raw Pixels ──────────────────────────────────────────────────────── */

asset_t *asset_load_pixels(asset_mgr_t *mgr, const char *name,
                            uint32_t *pixels, int w, int h)
{
    if (!pixels || w <= 0 || h <= 0) return NULL;

    asset_t *a = asset_alloc_slot(mgr, name);
    if (!a) return NULL;

    size_t sz = (size_t)w * (size_t)h * sizeof(uint32_t);
    a->data = malloc(sz);
    if (!a->data) return NULL;

    memcpy(a->data, pixels, sz);
    a->type = ASSET_IMAGE;
    a->width = w;
    a->height = h;
    a->size = (int)sz;
    a->active = true;
    return a;
}

/* ── Load Image from PPM File ─────────────────────────────────────────────── */

asset_t *asset_load_image(asset_mgr_t *mgr, const char *name, const char *path)
{
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;

    /* Read header — expecting PPM P6 format */
    char header[128];
    int hlen = 0;
    char c;
    while (hlen < 127 && read(fd, &c, 1) == 1) {
        if (c == '#') {
            /* Skip comment line */
            while (read(fd, &c, 1) == 1 && c != '\n');
            continue;
        }
        header[hlen++] = c;

        /* We need: "P6\n<width> <height>\n<maxval>\n" */
        /* Count newlines to know when we have the full header */
        if (c == '\n') {
            header[hlen] = '\0';
            /* Check if we have enough: P6 + dimensions + maxval */
            int w = 0, h = 0, maxval = 0;
            if (sscanf(header, "P6 %d %d %d", &w, &h, &maxval) == 3 ||
                sscanf(header, "P6\n%d %d\n%d", &w, &h, &maxval) == 3) {
                if (w > 0 && h > 0 && maxval > 0) {
                    /* Read pixel data */
                    size_t pixel_count = (size_t)w * (size_t)h;
                    size_t rgb_size = pixel_count * 3;
                    unsigned char *rgb = (unsigned char *)malloc(rgb_size);
                    if (!rgb) { close(fd); return NULL; }

                    size_t total_read = 0;
                    while (total_read < rgb_size) {
                        int r = (int)read(fd, rgb + total_read, rgb_size - total_read);
                        if (r <= 0) break;
                        total_read += (size_t)r;
                    }
                    close(fd);

                    /* Convert RGB to ARGB32 */
                    uint32_t *argb = (uint32_t *)malloc(pixel_count * sizeof(uint32_t));
                    if (!argb) { free(rgb); return NULL; }

                    for (size_t i = 0; i < pixel_count; i++) {
                        argb[i] = 0xFF000000u |
                                  ((uint32_t)rgb[i * 3 + 0] << 16) |
                                  ((uint32_t)rgb[i * 3 + 1] << 8) |
                                  (uint32_t)rgb[i * 3 + 2];
                    }
                    free(rgb);

                    asset_t *a = asset_alloc_slot(mgr, name);
                    if (!a) { free(argb); return NULL; }

                    a->data = argb;
                    a->type = ASSET_IMAGE;
                    a->width = w;
                    a->height = h;
                    a->size = (int)(pixel_count * sizeof(uint32_t));
                    a->active = true;
                    return a;
                }
            }
        }
    }

    close(fd);
    return NULL;
}

/* ── Load Raw Binary Data ─────────────────────────────────────────────────── */

asset_t *asset_load_data(asset_mgr_t *mgr, const char *name,
                          const void *data, int size)
{
    if (!data || size <= 0) return NULL;

    asset_t *a = asset_alloc_slot(mgr, name);
    if (!a) return NULL;

    a->data = malloc((size_t)size);
    if (!a->data) return NULL;

    memcpy(a->data, data, (size_t)size);
    a->type = ASSET_DATA;
    a->size = size;
    a->active = true;
    return a;
}

/* ── Procedural Image Generation ──────────────────────────────────────────── */

asset_t *asset_create_solid(asset_mgr_t *mgr, const char *name,
                             int w, int h, uint32_t color)
{
    if (w <= 0 || h <= 0) return NULL;

    size_t pixel_count = (size_t)w * (size_t)h;
    uint32_t *pixels = (uint32_t *)malloc(pixel_count * sizeof(uint32_t));
    if (!pixels) return NULL;

    for (size_t i = 0; i < pixel_count; i++)
        pixels[i] = color;

    asset_t *a = asset_alloc_slot(mgr, name);
    if (!a) { free(pixels); return NULL; }

    a->data = pixels;
    a->type = ASSET_IMAGE;
    a->width = w;
    a->height = h;
    a->size = (int)(pixel_count * sizeof(uint32_t));
    a->active = true;
    return a;
}

asset_t *asset_create_checker(asset_mgr_t *mgr, const char *name,
                               int w, int h, int cell_size,
                               uint32_t color_a, uint32_t color_b)
{
    if (w <= 0 || h <= 0 || cell_size <= 0) return NULL;

    size_t pixel_count = (size_t)w * (size_t)h;
    uint32_t *pixels = (uint32_t *)malloc(pixel_count * sizeof(uint32_t));
    if (!pixels) return NULL;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            bool even = ((x / cell_size) + (y / cell_size)) % 2 == 0;
            pixels[y * w + x] = even ? color_a : color_b;
        }
    }

    asset_t *a = asset_alloc_slot(mgr, name);
    if (!a) { free(pixels); return NULL; }

    a->data = pixels;
    a->type = ASSET_IMAGE;
    a->width = w;
    a->height = h;
    a->size = (int)(pixel_count * sizeof(uint32_t));
    a->active = true;
    return a;
}

asset_t *asset_create_gradient_h(asset_mgr_t *mgr, const char *name,
                                   int w, int h,
                                   uint32_t left_color, uint32_t right_color)
{
    if (w <= 0 || h <= 0) return NULL;

    size_t pixel_count = (size_t)w * (size_t)h;
    uint32_t *pixels = (uint32_t *)malloc(pixel_count * sizeof(uint32_t));
    if (!pixels) return NULL;

    for (int x = 0; x < w; x++) {
        int t = (w > 1) ? (x * 255 / (w - 1)) : 0;
        int inv_t = 255 - t;

        uint32_t r = (((left_color >> 16) & 0xFF) * (uint32_t)inv_t + ((right_color >> 16) & 0xFF) * (uint32_t)t) / 255;
        uint32_t g = (((left_color >> 8) & 0xFF) * (uint32_t)inv_t + ((right_color >> 8) & 0xFF) * (uint32_t)t) / 255;
        uint32_t b = ((left_color & 0xFF) * (uint32_t)inv_t + (right_color & 0xFF) * (uint32_t)t) / 255;
        uint32_t col = 0xFF000000u | (r << 16) | (g << 8) | b;

        for (int y = 0; y < h; y++)
            pixels[y * w + x] = col;
    }

    asset_t *a = asset_alloc_slot(mgr, name);
    if (!a) { free(pixels); return NULL; }

    a->data = pixels;
    a->type = ASSET_IMAGE;
    a->width = w;
    a->height = h;
    a->size = (int)(pixel_count * sizeof(uint32_t));
    a->active = true;
    return a;
}

/* ── Lookup ───────────────────────────────────────────────────────────────── */

asset_t *asset_get(asset_mgr_t *mgr, const char *name)
{
    for (int i = 0; i < mgr->count; i++) {
        if (mgr->entries[i].active &&
            strncmp(mgr->entries[i].name, name, ASSET_NAME_LEN - 1) == 0) {
            return &mgr->entries[i];
        }
    }
    return NULL;
}

void asset_free(asset_mgr_t *mgr, const char *name)
{
    for (int i = 0; i < mgr->count; i++) {
        if (mgr->entries[i].active &&
            strncmp(mgr->entries[i].name, name, ASSET_NAME_LEN - 1) == 0) {
            if (mgr->entries[i].data) free(mgr->entries[i].data);
            memset(&mgr->entries[i], 0, sizeof(asset_t));
            return;
        }
    }
}
