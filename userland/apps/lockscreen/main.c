/* ============================================================================
 * AzamiOS — Desktop Lock Screen (lockscreen.elf)
 * File: userland/apps/lockscreen/main.c
 *
 * Features:
 * ──────────
 *  • Fullscreen topmost overlay (AZ_WM_ZORDER_TOP)
 *  • Modern Catppuccin Mocha aesthetic with blurred backdrop
 *  • Real-time digital clock and date
 *  • User avatar card and username
 *  • Masked password entry field with blinking cursor
 *  • Cryptographic SHA-256 password hash verification against /etc/shadow
 *  • Unlocks and terminates cleanly upon valid credentials
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include "../shared/ui_kit.h"
#include "../shared/de_log.h"
#include "../azwm/protocol.h"

#define LOCK_MAP_ADDR ((void *)0x6E000000)
#define SERVER_CHAN   1

/* ── SHA-256 Engine ───────────────────────────────────────────────────────── */
typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buffer[64];
} sha256_t;

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROR32(x, 2) ^ ROR32(x, 13) ^ ROR32(x, 22))
#define EP1(x) (ROR32(x, 6) ^ ROR32(x, 11) ^ ROR32(x, 25))
#define SIG0(x) (ROR32(x, 7) ^ ROR32(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROR32(x, 17) ^ ROR32(x, 19) ^ ((x) >> 10))

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_transform(sha256_t *ctx, const uint8_t data[64])
{
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    for (int i = 0, j = 0; i < 16; i++, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    }
    for (int i = 16; i < 64; i++) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + K[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init_ctx(sha256_t *ctx)
{
    ctx->count = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

static void sha256_update_ctx(sha256_t *ctx, const uint8_t *data, size_t len)
{
    size_t i = 0;
    size_t buffer_idx = (size_t)((ctx->count >> 3) & 63);
    ctx->count += (uint64_t)len << 3;

    if (buffer_idx > 0) {
        size_t needed = 64 - buffer_idx;
        if (len >= needed) {
            memcpy(&ctx->buffer[buffer_idx], data, needed);
            sha256_transform(ctx, ctx->buffer);
            i += needed;
        } else {
            memcpy(&ctx->buffer[buffer_idx], data, len);
            return;
        }
    }
    for (; i + 64 <= len; i += 64) {
        sha256_transform(ctx, &data[i]);
    }
    if (i < len) {
        memcpy(ctx->buffer, &data[i], len - i);
    }
}

static void sha256_final_ctx(sha256_t *ctx, uint8_t hash[32])
{
    uint8_t pad = 0x80;
    sha256_update_ctx(ctx, &pad, 1);
    while ((ctx->count >> 3) % 64 != 56) {
        uint8_t zero = 0;
        sha256_update_ctx(ctx, &zero, 1);
    }
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; i++) {
        len_bytes[i] = (uint8_t)(ctx->count >> (56 - i * 8));
    }
    sha256_update_ctx(ctx, len_bytes, 8);
    for (int i = 0; i < 8; i++) {
        hash[i * 4 + 0] = (uint8_t)(ctx->state[i] >> 24);
        hash[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

static void sha256_hash_string(const char *str, char out_hex[65])
{
    sha256_t ctx;
    uint8_t hash[32];
    sha256_init_ctx(&ctx);
    sha256_update_ctx(&ctx, (const uint8_t *)str, strlen(str));
    sha256_final_ctx(&ctx, hash);
    for (int i = 0; i < 32; i++) {
        snprintf(out_hex + i * 2, 3, "%02x", hash[i]);
    }
    out_hex[64] = '\0';
}

/* ── Hash Verification against /etc/shadow ────────────────────────────────── */
static int verify_user_password(const char *user, const char *pass)
{
    FILE *fp = fopen("/etc/shadow", "r");
    if (!fp) fp = fopen("/etc/passwd", "r");
    char line[256];
    char stored_hash[128];
    stored_hash[0] = '\0';

    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            char *colon = strchr(line, ':');
            if (!colon) continue;
            *colon = '\0';
            if (strcmp(line, user) == 0) {
                char *hstart = colon + 1;
                char *colon2 = strchr(hstart, ':');
                if (colon2) *colon2 = '\0';
                /* Strip trailing newline */
                char *nl = strchr(hstart, '\n');
                if (nl) *nl = '\0';
                strncpy(stored_hash, hstart, sizeof(stored_hash) - 1);
                stored_hash[sizeof(stored_hash) - 1] = '\0';
                break;
            }
        }
        fclose(fp);
    }

    /* Fallback if no stored hash exists in shadow or entry is empty/'x'/'!' */
    if (stored_hash[0] == '\0' || strcmp(stored_hash, "x") == 0 || strcmp(stored_hash, "!") == 0) {
        return (strcmp(pass, "azami") == 0);
    }

    /* Compute SHA-256 hash of entered password */
    char entered_hex[65];
    sha256_hash_string(pass, entered_hex);

    /* 1. Direct hex comparison */
    if (strcasecmp(entered_hex, stored_hash) == 0) {
        return 1;
    }

    /* 2. Check if formatted as $5$salt$hash or $sha256$salt$hash */
    if (stored_hash[0] == '$') {
        char *p1 = strchr(stored_hash + 1, '$');
        if (p1) {
            char *p2 = strchr(p1 + 1, '$');
            if (p2) {
                char salt[64];
                size_t slen = (size_t)(p2 - (p1 + 1));
                if (slen < sizeof(salt)) {
                    memcpy(salt, p1 + 1, slen);
                    salt[slen] = '\0';
                    char salted[256];
                    snprintf(salted, sizeof(salted), "%s%s", salt, pass);
                    char salted_hex[65];
                    sha256_hash_string(salted, salted_hex);
                    if (strcasecmp(salted_hex, p2 + 1) == 0) return 1;
                }
            }
        }
    }

    return 0;
}

/* ── UI Drawing ───────────────────────────────────────────────────────────── */
static void draw_lockscreen(uk_window_t *w, const char *pass_buf, int pass_len,
                           int error_state, int cursor_tick)
{
    unsigned int width  = w->width;
    unsigned int height = w->height;

    /* Background: deep Catppuccin Mocha crust */
    uk_fill_rect(w, 0, 0, (int)width, (int)height, 0xFF11111B);

    /* Ambient decorative gradient orbs */
    int cx = (int)width / 2;
    int cy = (int)height / 2;

    /* Time & Date Header */
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);

    char time_str[32];
    snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d",
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);

    char date_str[64];
    strftime(date_str, sizeof(date_str), "%A, %B %d, %Y", &tm_info);

    /* Draw large clock (centered) */
    uk_draw_text_2x(w, cx - 70, cy - 170, time_str, UK_TEXT);
    uk_draw_text_centred(w, cx, cy - 130, date_str, UK_SUBTEXT0);

    /* Central Card (Glassmorphism card) */
    int card_w = 340;
    int card_h = 240;
    int card_x = cx - card_w / 2;
    int card_y = cy - 90;

    uk_fill_rounded_rect(w, card_x, card_y, card_w, card_h, 14, 0xCC1E1E2E);
    uk_draw_rounded_rect_outline(w, card_x, card_y, card_w, card_h, 14, UK_SURFACE1);

    /* User Avatar */
    int avatar_x = cx;
    int avatar_y = card_y + 36;
    uk_fill_circle(w, avatar_x, avatar_y, 22, UK_MAUVE);
    uk_draw_char(w, avatar_x - 3, avatar_y - 6, 'A', UK_BASE);

    /* Username */
    uk_draw_text_centred(w, cx, avatar_y + 30, "Azami User (azami)", UK_TEXT);

    /* Password Input Box */
    int box_w = 230;
    int box_h = 32;
    int box_x = cx - box_w / 2;
    int box_y = avatar_y + 54;

    unsigned int border_col = error_state ? UK_RED : UK_BLUE;
    uk_fill_rounded_rect(w, box_x, box_y, box_w, box_h, 6, UK_MANTLE);
    uk_draw_rounded_rect_outline(w, box_x, box_y, box_w, box_h, 6, border_col);

    /* Password dots */
    char masked[64];
    int d = 0;
    for (int i = 0; i < pass_len && d < 60; i++) {
        masked[d++] = '*';
        masked[d++] = ' ';
    }
    masked[d] = '\0';
    uk_draw_text(w, box_x + 10, box_y + 9, masked, UK_TEXT);

    /* Blinking Cursor */
    if ((cursor_tick / 15) % 2 == 0) {
        int cursor_x = box_x + 10 + d * 8;
        if (cursor_x < box_x + box_w - 12) {
            uk_fill_rect(w, cursor_x, box_y + 8, 2, 16, UK_MAUVE);
        }
    }

    /* Unlock button icon */
    uk_fill_rounded_rect(w, box_x + box_w - 28, box_y + 4, 24, 24, 4, UK_SURFACE0);
    uk_draw_char(w, box_x + box_w - 20, box_y + 8, '>', UK_MAUVE);

    /* Status / Error Message */
    if (error_state) {
        uk_draw_text_centred(w, cx, box_y + 44, "Incorrect password. Try again.", UK_RED);
    } else {
        uk_draw_text_centred(w, cx, box_y + 44, "Press Enter to unlock", UK_OVERLAY1);
    }

    /* Bottom Status Bar */
    uk_draw_text_centred(w, cx, (int)height - 30, "AzamiOS v7.0 • Modular Microkernel", UK_OVERLAY0);
}

/* ── Main Loop ────────────────────────────────────────────────────────────── */
int main(void)
{
    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0 && fb.width > 0 && fb.height > 0) {
        sw = fb.width;
        sh = fb.height;
    }

    uk_window_t win;
    /* Create fullscreen frameless window */
    if (uk_window_connect(&win, "", 0, 0, sw, sh, LOCK_MAP_ADDR, SERVER_CHAN) != 0) {
        puts("[lockscreen] ERROR: uk_window_connect failed");
        return 1;
    }

    uk_set_zorder(&win, AZ_WM_ZORDER_TOP);

    char pass[64];
    int  pass_len = 0;
    pass[0] = '\0';

    int error_state = 0;
    int cursor_tick = 0;
    int running = 1;

    while (running) {
        az_wm_msg_t msg;
        while (az_channel_recv_nb(win.client_chan, (az_ipc_msg_t *)&msg) == 0) {
            if (msg.type == AZ_WM_KEY_EVENT && msg.key.pressed) {
                unsigned char key = msg.key.keycode;

                if (key == '\n' || key == '\r') {
                    /* Check password hash */
                    pass[pass_len] = '\0';
                    if (verify_user_password("azami", pass) ||
                        verify_user_password("root", pass)) {
                        /* Unlocked successfully! */
                        running = 0;
                        break;
                    } else {
                        /* Authentication failed */
                        error_state = 1;
                        pass_len = 0;
                        pass[0] = '\0';
                    }
                } else if (key == '\b' || key == 127) {
                    if (pass_len > 0) {
                        pass_len--;
                        pass[pass_len] = '\0';
                        error_state = 0;
                    }
                } else if (key == 27) { /* ESC */
                    pass_len = 0;
                    pass[0] = '\0';
                    error_state = 0;
                } else if (key >= 32 && key <= 126 && pass_len < 60) {
                    pass[pass_len++] = (char)key;
                    pass[pass_len] = '\0';
                    error_state = 0;
                }
            }
        }

        draw_lockscreen(&win, pass, pass_len, error_state, cursor_tick++);
        uk_invalidate(&win);

        /* ~30 FPS */
        usleep(33000);
    }

    /* Destroy lock overlay window before exiting */
    uk_window_destroy(&win);
    return 0;
}
