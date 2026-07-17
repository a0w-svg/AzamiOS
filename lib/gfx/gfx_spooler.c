#include "gfx_spooler.h"

static gfx_spool_cmd_t g_queue[GFX_SPOOLER_QUEUE_SIZE];
static volatile int g_head = 0;
static volatile int g_tail = 0;

void gfx_spooler_init(void) {
    g_head = 0;
    g_tail = 0;
}

static inline bool spooler_push_cmd(const gfx_spool_cmd_t *cmd) {
    int next_tail = (g_tail + 1) & GFX_SPOOLER_QUEUE_MASK;
    if (next_tail == g_head) {
        return false;
    }
    g_queue[g_tail] = *cmd;
    g_tail = next_tail;
    return true;
}

bool gfx_spooler_push_rect(int x, int y, int w, int h, uint32_t color) {
    gfx_spool_cmd_t cmd = {
        .type = GFX_SPOOL_CMD_RECT,
        .x = x, .y = y, .w = w, .h = h,
        .color = color, .shm_buf = NULL, .shm_pitch = 0
    };
    return spooler_push_cmd(&cmd);
}

bool gfx_spooler_push_pixel(int x, int y, uint32_t color) {
    gfx_spool_cmd_t cmd = {
        .type = GFX_SPOOL_CMD_PIXEL,
        .x = x, .y = y, .w = 1, .h = 1,
        .color = color, .shm_buf = NULL, .shm_pitch = 0
    };
    return spooler_push_cmd(&cmd);
}

bool gfx_spooler_push_scroll(int lines, uint32_t bg_color) {
    gfx_spool_cmd_t cmd = {
        .type = GFX_SPOOL_CMD_SCROLL,
        .x = 0, .y = 0, .w = 0, .h = lines,
        .color = bg_color, .shm_buf = NULL, .shm_pitch = 0
    };
    return spooler_push_cmd(&cmd);
}

bool gfx_spooler_push_clear(uint32_t color) {
    gfx_spool_cmd_t cmd = {
        .type = GFX_SPOOL_CMD_CLEAR,
        .x = 0, .y = 0, .w = 0, .h = 0,
        .color = color, .shm_buf = NULL, .shm_pitch = 0
    };
    return spooler_push_cmd(&cmd);
}

bool gfx_spooler_push_blit_shm(int dx, int dy, int w, int h, const uint32_t *shm_buf, int shm_pitch) {
    if (!shm_buf || w <= 0 || h <= 0) return false;
    gfx_spool_cmd_t cmd = {
        .type = GFX_SPOOL_CMD_BLIT_SHM,
        .x = dx, .y = dy, .w = w, .h = h,
        .color = 0, .shm_buf = shm_buf, .shm_pitch = shm_pitch
    };
    return spooler_push_cmd(&cmd);
}

int gfx_spooler_flush(gfx_blit_ctx_t *ctx) {
    if (!ctx || !ctx->backbuffer) return 0;
    int processed = 0;
    while (g_head != g_tail) {
        const gfx_spool_cmd_t *cmd = &g_queue[g_head];
        switch (cmd->type) {
            case GFX_SPOOL_CMD_RECT:
                gfx_blit_rect(ctx, cmd->x, cmd->y, cmd->w, cmd->h, cmd->color);
                break;
            case GFX_SPOOL_CMD_PIXEL:
                gfx_blit_put_pixel(ctx, cmd->x, cmd->y, cmd->color);
                break;
            case GFX_SPOOL_CMD_SCROLL:
                gfx_blit_scroll(ctx, cmd->h, cmd->color);
                break;
            case GFX_SPOOL_CMD_CLEAR:
                gfx_blit_clear(ctx, cmd->color);
                break;
            case GFX_SPOOL_CMD_BLIT_SHM: {
                int dx = cmd->x, dy = cmd->y, w = cmd->w, h = cmd->h;
                if (dx >= ctx->width || dy >= ctx->height || dx + w <= 0 || dy + h <= 0) break;
                int sx = 0, sy = 0;
                if (dx < 0) { sx = -dx; w += dx; dx = 0; }
                if (dy < 0) { sy = -dy; h += dy; dy = 0; }
                if (dx + w > ctx->width)  w = ctx->width - dx;
                if (dy + h > ctx->height) h = ctx->height - dy;
                for (int r = 0; r < h; r++) {
                    const uint32_t *src_row = cmd->shm_buf + (sy + r) * cmd->shm_pitch + sx;
                    uint32_t *dst_row = ctx->backbuffer + (dy + r) * ctx->width + dx;
                    for (int c = 0; c < w; c++) {
                        dst_row[c] = src_row[c];
                    }
                }
                break;
            }
            default:
                break;
        }
        g_head = (g_head + 1) & GFX_SPOOLER_QUEUE_MASK;
        processed++;
    }
    return processed;
}
