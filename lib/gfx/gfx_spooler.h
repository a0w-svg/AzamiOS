#ifndef LIB_GFX_SPOOLER_H
#define LIB_GFX_SPOOLER_H

#include "gfx_blit.h"
#include <stdbool.h>
#include <stddef.h>

#define GFX_SPOOLER_QUEUE_SIZE 256
#define GFX_SPOOLER_QUEUE_MASK (GFX_SPOOLER_QUEUE_SIZE - 1)

typedef enum {
    GFX_SPOOL_CMD_RECT = 1,
    GFX_SPOOL_CMD_PIXEL,
    GFX_SPOOL_CMD_SCROLL,
    GFX_SPOOL_CMD_CLEAR,
    GFX_SPOOL_CMD_BLIT_SHM
} gfx_spool_cmd_type_t;

typedef struct {
    gfx_spool_cmd_type_t type;
    int x, y, w, h;
    uint32_t color;
    const uint32_t *shm_buf;
    int shm_pitch;
} gfx_spool_cmd_t;

void gfx_spooler_init(void);
bool gfx_spooler_push_rect(int x, int y, int w, int h, uint32_t color);
bool gfx_spooler_push_pixel(int x, int y, uint32_t color);
bool gfx_spooler_push_scroll(int lines, uint32_t bg_color);
bool gfx_spooler_push_clear(uint32_t color);
bool gfx_spooler_push_blit_shm(int dx, int dy, int w, int h, const uint32_t *shm_buf, int shm_pitch);
int  gfx_spooler_flush(gfx_blit_ctx_t *ctx);

#endif /* LIB_GFX_SPOOLER_H */
