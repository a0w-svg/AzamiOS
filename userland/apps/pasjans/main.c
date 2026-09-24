/* ============================================================================
 * AzamiOS Desktop Environment — Pasjans (Klondike Solitaire)
 * File: userland/apps/pasjans/main.c
 *
 * A complete Klondike. Draw-one or draw-three stock with unlimited redeals,
 * four foundations, seven tableau columns, drag-and-drop of multi-card runs,
 * click-to-place, unlimited undo, Microsoft-style scoring, a hint finder,
 * auto-finish, and the bouncing-card win animation.
 *
 * Everything is integer maths: the userland toolchain builds freestanding
 * with no soft-float runtime, so the pip geometry, the card art and the
 * win animation's ballistics all run in fixed point.
 * ============================================================================ */

#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/time.h"
#include "../../libc/include/unistd.h"
#include <stdbool.h>
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN   1
#define WIN_W         920
#define WIN_H         680
#define MAP_ADDR      ((void *)0x6D000000)

/* Animation/UI clock. Everything time-based counts ticks; the play clock
 * itself comes from time(2) so a busy compositor cannot slow it down. */
#define TICK_MS       40

/* ── Palette ─────────────────────────────────────────────────────────────── */
#define COL_FELT_TOP  0xFF1B6340
#define COL_FELT_BOT  0xFF0D3A24
#define COL_CARD      0xFFFAF7F1
#define COL_CARD_EDGE 0xFF3A3A48
#define COL_RED       0xFFC62F3B
#define COL_BLACK     0xFF1E1E2A
#define COL_BACK_A    0xFF2B4A8F
#define COL_BACK_B    0xFF4C6DC4
#define COL_BACK_C    0xFFE8EDFA
#define COL_GOLD      0xFFE3B341
#define COL_SKIN      0xFFF0CFA8
#define COL_HINT      0xFFF9E2AF
#define COL_SLOT_LINE 0x66FFFFFF

/* ============================================================================
 * Random numbers
 * ==========================================================================*/

static unsigned int g_seed_used = 0;   /* the deal number, for "Replay" */
static unsigned int g_rng = 0x5EED1337u;

static unsigned int rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static unsigned int entropy_seed(void)
{
    unsigned int s = 0;
    int fd = sys_open("/dev/hwrng", 0, 0);
    if (fd < 0) fd = sys_open("/dev/urandom", 0, 0);
    if (fd >= 0) {
        sys_read(fd, &s, sizeof(s));
        sys_close(fd);
    }
    if (s == 0) s = (unsigned int)time(NULL) * 2654435761u + 1u;
    /* Deal numbers are shown to the player, so keep them human-sized. */
    return (s % 999999u) + 1u;
}

/* ============================================================================
 * Card and pile model
 *
 * Every pile — stock, waste, the four foundations and the seven tableau
 * columns — is the same pile_t addressed by a small integer id, so the move
 * engine, the undo log and the hit tester all speak one language instead of
 * one special case per pile kind.
 * ==========================================================================*/

enum { SUIT_CLUB = 0, SUIT_DIAMOND, SUIT_HEART, SUIT_SPADE };

typedef struct {
    unsigned char suit;
    unsigned char rank;      /* 1 = Ace .. 13 = King */
    bool          face_up;
} card_t;

#define P_STOCK  0
#define P_WASTE  1
#define P_FOUND  2           /* 2 .. 5  */
#define P_TAB    6           /* 6 .. 12 */
#define NPILES   13

typedef struct {
    card_t *c[52];
    int     n;
} pile_t;

static card_t g_deck[52];
static pile_t g_pile[NPILES];

static bool card_is_red(const card_t *c)
{
    return c->suit == SUIT_DIAMOND || c->suit == SUIT_HEART;
}

static card_t *pile_top(pile_t *p)
{
    return p->n > 0 ? p->c[p->n - 1] : NULL;
}

static const char *rank_str(int rank)
{
    static const char *r[] = { "", "A", "2", "3", "4", "5", "6", "7",
                               "8", "9", "10", "J", "Q", "K" };
    return (rank >= 1 && rank <= 13) ? r[rank] : "?";
}

/* ============================================================================
 * Small raster primitives the ui_kit does not carry
 *
 * ui_kit's fills are opaque, so the shadows, the dimmed modal backdrops and
 * the empty-slot wells need their own alpha-blended versions. Likewise the
 * bottom-right card index has to be drawn upside down, which means reading
 * the font's glyph bits directly.
 * ==========================================================================*/

static int isqrt_i(int v)
{
    if (v <= 0) return 0;
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

static void blend_rect(uk_window_t *w, int rx, int ry, int rw, int rh,
                       unsigned int col, unsigned int a)
{
    if (rw <= 0 || rh <= 0 || !w || !w->pixels) return;
    int x0 = rx < w->clip_x0 ? w->clip_x0 : rx;
    int y0 = ry < w->clip_y0 ? w->clip_y0 : ry;
    int x1 = rx + rw, y1 = ry + rh;
    if (x1 > w->clip_x1) x1 = w->clip_x1;
    if (y1 > w->clip_y1) y1 = w->clip_y1;
    for (int y = y0; y < y1; y++) {
        unsigned int *d = &w->pixels[(unsigned int)y * w->width];
        for (int x = x0; x < x1; x++)
            d[x] = uk_blend(d[x], col, a);
    }
}

static void blend_rrect(uk_window_t *w, int rx, int ry, int rw, int rh,
                        int r, unsigned int col, unsigned int a)
{
    if (rw <= 0 || rh <= 0) return;
    if (r * 2 > rw) r = rw / 2;
    if (r * 2 > rh) r = rh / 2;
    if (r < 0) r = 0;
    for (int y = 0; y < rh; y++) {
        int dy = -1;
        if (y < r)            dy = r - 1 - y;
        else if (y >= rh - r) dy = y - (rh - r);
        int inset = (dy >= 0) ? r - isqrt_i(r * r - dy * dy) : 0;
        blend_rect(w, rx + inset, ry + y, rw - 2 * inset, 1, col, a);
    }
}

/* Filled triangle: per scanline, the span between the outermost edge
 * crossings. Used for every pip that is not a circle. */
static void fill_tri(uk_window_t *w, int x0, int y0, int x1, int y1,
                     int x2, int y2, unsigned int col)
{
    int xs[3] = { x0, x1, x2 };
    int ys[3] = { y0, y1, y2 };
    int ymin = y0, ymax = y0;
    if (y1 < ymin) ymin = y1;
    if (y2 < ymin) ymin = y2;
    if (y1 > ymax) ymax = y1;
    if (y2 > ymax) ymax = y2;

    for (int y = ymin; y <= ymax; y++) {
        int lo = 0x7FFFFFFF, hi = -0x7FFFFFFF;
        bool any = false;
        for (int e = 0; e < 3; e++) {
            int a = e, b = (e + 1) % 3;
            int ya = ys[a], yb = ys[b];
            if (ya == yb) {
                if (ya != y) continue;
                int l = xs[a] < xs[b] ? xs[a] : xs[b];
                int h = xs[a] > xs[b] ? xs[a] : xs[b];
                if (l < lo) lo = l;
                if (h > hi) hi = h;
                any = true;
                continue;
            }
            int ylo = ya < yb ? ya : yb, yhi = ya > yb ? ya : yb;
            if (y < ylo || y > yhi) continue;
            int x = xs[a] + (xs[b] - xs[a]) * (y - ya) / (yb - ya);
            if (x < lo) lo = x;
            if (x > hi) hi = x;
            any = true;
        }
        if (any && hi >= lo)
            uk_fill_rect(w, lo, y, hi - lo + 1, 1, col);
    }
}

/* One glyph rotated 180°, for the index in a card's bottom-right corner. */
static void draw_char_rot180(uk_window_t *w, int x, int y, char c,
                             unsigned int col, const de_font_t *f, int scale)
{
    unsigned char idx = (unsigned char)c;
    if (idx < f->first_char || idx > f->last_char) return;
    const unsigned char *g = f->glyphs + (size_t)(idx - f->first_char) * f->glyph_h;
    int gw = f->glyph_w, gh = f->glyph_h;
    for (int dy = 0; dy < gh; dy++) {
        unsigned char bits = g[gh - 1 - dy];
        if (!bits) continue;
        for (int dx = 0; dx < gw; dx++) {
            if (!(bits & (0x80 >> (gw - 1 - dx)))) continue;
            uk_fill_rect(w, x + dx * scale, y + dy * scale, scale, scale, col);
        }
    }
}

/* ============================================================================
 * Suit pips
 *
 * Drawn as vector-ish primitives rather than a bitmap so they stay clean at
 * every card size the responsive layout picks, from a 40px corner pip to the
 * half-card pip on an Ace. `dir` is +1 upright, -1 rotated 180° — the lower
 * half of a card's pip grid is inverted the way a real deck is.
 * ==========================================================================*/

static void draw_suit(uk_window_t *w, int cx, int cy, int s, int suit,
                      int dir, unsigned int col)
{
    if (s < 5) s = 5;
    int half = s / 2;

    switch (suit) {
    case SUIT_HEART: {
        int r   = (s * 4) / 15;  if (r < 1) r = 1;
        int off = (s * 7) / 30;  if (off < 1) off = 1;
        int yc  = -half + r;
        uk_fill_circle(w, cx - off, cy + dir * yc, r, col);
        uk_fill_circle(w, cx + off, cy + dir * yc, r, col);
        fill_tri(w, cx - half, cy + dir * yc,
                    cx + half, cy + dir * yc,
                    cx,        cy + dir * half, col);
        break;
    }
    case SUIT_DIAMOND: {
        int hw = (s * 2) / 5;
        fill_tri(w, cx, cy - half, cx - hw, cy, cx, cy + half, col);
        fill_tri(w, cx, cy - half, cx + hw, cy, cx, cy + half, col);
        break;
    }
    case SUIT_SPADE: {
        int stem = s / 5;
        int bb   = half - stem;
        int r    = (s * 4) / 15;  if (r < 1) r = 1;
        int off  = (s * 7) / 30;  if (off < 1) off = 1;
        int yc   = bb - r;
        uk_fill_circle(w, cx - off, cy + dir * yc, r, col);
        uk_fill_circle(w, cx + off, cy + dir * yc, r, col);
        fill_tri(w, cx - half, cy + dir * yc,
                    cx + half, cy + dir * yc,
                    cx,        cy - dir * half, col);
        int sw = s / 6; if (sw < 1) sw = 1;
        fill_tri(w, cx - sw, cy + dir * half,
                    cx + sw, cy + dir * half,
                    cx,      cy + dir * (bb - r / 2), col);
        break;
    }
    default: {   /* SUIT_CLUB */
        int stem = s / 5;
        int bb   = half - stem;
        int r    = (s * 13) / 50; if (r < 1) r = 1;
        int lob  = (r * 5) / 4;
        uk_fill_circle(w, cx,       cy + dir * (-half + r), r, col);
        uk_fill_circle(w, cx - lob, cy + dir * (bb - r),    r, col);
        uk_fill_circle(w, cx + lob, cy + dir * (bb - r),    r, col);
        int sw = s / 6; if (sw < 1) sw = 1;
        fill_tri(w, cx - sw, cy + dir * half,
                    cx + sw, cy + dir * half,
                    cx,      cy + dir * (bb - r), col);
        break;
    }
    }
}

/* ============================================================================
 * Card faces
 * ==========================================================================*/

/* Pip grid for ranks 2..10, as (column, row-per-mille) pairs. Column 0/1/2
 * is left/centre/right; the row runs 0 (top of the pip area) to 1000
 * (bottom). Anything below the halfway line is drawn inverted. */
typedef struct { unsigned char col; unsigned short row; } pip_t;

static const pip_t k_pips2[]  = { {1,0},{1,1000} };
static const pip_t k_pips3[]  = { {1,0},{1,500},{1,1000} };
static const pip_t k_pips4[]  = { {0,0},{2,0},{0,1000},{2,1000} };
static const pip_t k_pips5[]  = { {0,0},{2,0},{1,500},{0,1000},{2,1000} };
static const pip_t k_pips6[]  = { {0,0},{2,0},{0,500},{2,500},{0,1000},{2,1000} };
static const pip_t k_pips7[]  = { {0,0},{2,0},{1,250},{0,500},{2,500},{0,1000},{2,1000} };
static const pip_t k_pips8[]  = { {0,0},{2,0},{1,250},{0,500},{2,500},{1,750},
                                  {0,1000},{2,1000} };
static const pip_t k_pips9[]  = { {0,0},{2,0},{0,333},{2,333},{1,500},
                                  {0,667},{2,667},{0,1000},{2,1000} };
static const pip_t k_pips10[] = { {0,0},{2,0},{1,167},{0,333},{2,333},
                                  {0,667},{2,667},{1,833},{0,1000},{2,1000} };

static const pip_t *pip_layout(int rank, int *count)
{
    switch (rank) {
    case 2:  *count = 2;  return k_pips2;
    case 3:  *count = 3;  return k_pips3;
    case 4:  *count = 4;  return k_pips4;
    case 5:  *count = 5;  return k_pips5;
    case 6:  *count = 6;  return k_pips6;
    case 7:  *count = 7;  return k_pips7;
    case 8:  *count = 8;  return k_pips8;
    case 9:  *count = 9;  return k_pips9;
    case 10: *count = 10; return k_pips10;
    default: *count = 0;  return NULL;
    }
}

/* The corner index. `rot` draws it upside down for the bottom-right corner;
 * a two-character "10" is set on a tighter advance so it still clears the
 * pip grid on the narrowest cards the layout produces. */
static void draw_index(uk_window_t *w, int x, int y, const char *s,
                       unsigned int col, int scale, bool rot)
{
    int n = uk_strlen(s);
    int adv = (n > 1 ? 7 : 8) * scale;
    for (int i = 0; i < n; i++) {
        if (rot)
            draw_char_rot180(w, x + i * adv, y, s[n - 1 - i], col,
                             &de_font_regular, scale);
        else
            uk_draw_text_ex(w, x + i * adv, y, (char[]){ s[i], 0 }, col,
                            &de_font_regular, scale, true);
    }
}

static int index_width(const char *s, int scale)
{
    int n = uk_strlen(s);
    return n * (n > 1 ? 7 : 8) * scale;
}

/* Court cards: a framed panel with a hatched ground, a small figure and a
 * pip in two opposite corners. Not a real Jack, but it reads as one at a
 * glance, which is all the tableau needs. */
static void draw_court(uk_window_t *w, int x, int y, int cw, int ch,
                       int rank, int suit, unsigned int col)
{
    int ix = x + (cw * 22) / 100, iy = y + (ch * 15) / 100;
    int iw = cw - 2 * ((cw * 22) / 100);
    int ih = ch - 2 * ((ch * 15) / 100);
    if (iw < 10 || ih < 14) return;

    unsigned int tint = (suit == SUIT_DIAMOND || suit == SUIT_HEART)
                        ? 0xFFF8E4E7 : 0xFFE9E9F1;
    uk_fill_rounded_rect(w, ix, iy, iw, ih, 3, tint);

    uk_push_clip(w, ix + 1, iy + 1, iw - 2, ih - 2);
    unsigned int hatch = uk_blend(tint, col, 36);
    for (int d = -ih; d < iw + ih; d += 6)
        uk_line(w, ix + d, iy, ix + d + ih, iy + ih, hatch);

    int cxp  = ix + iw / 2;
    int headr = (iw * 18) / 100; if (headr < 3) headr = 3;
    int headcy = iy + (ih * 46) / 100;

    /* Robe: shoulders flaring from the neck to the foot of the panel. */
    fill_tri(w, cxp, headcy, ix + (iw * 6) / 100, iy + ih,
                 ix + iw - (iw * 6) / 100, iy + ih, col);

    /* Head */
    uk_fill_circle(w, cxp, headcy, headr, COL_SKIN);
    int er = headr / 4; if (er < 1) er = 1;
    uk_fill_circle(w, cxp - headr / 2, headcy - headr / 5, er, COL_BLACK);
    uk_fill_circle(w, cxp + headr / 2, headcy - headr / 5, er, COL_BLACK);

    /* Headgear — band thickness and spike height track the head, so the
     * crown reads the same on a 40px card as on a 104px one. */
    int hy  = headcy - headr;
    int hwd = headr + headr / 2;
    int bt  = headr / 3; if (bt < 2) bt = 2;
    int sp  = headr;     if (sp < 3) sp = 3;
    int by  = hy - bt;

    if (rank == 13) {                       /* King: three-point crown */
        fill_tri(w, cxp - hwd,         by, cxp - hwd / 3,     by,
                    cxp - hwd * 2 / 3, by - sp, COL_GOLD);
        fill_tri(w, cxp - hwd / 3,     by, cxp + hwd / 3,     by,
                    cxp,               by - sp - sp / 3, COL_GOLD);
        fill_tri(w, cxp + hwd / 3,     by, cxp + hwd,         by,
                    cxp + hwd * 2 / 3, by - sp, COL_GOLD);
        uk_fill_rect(w, cxp - hwd, by, hwd * 2, bt, COL_GOLD);
    } else if (rank == 12) {                /* Queen: jewelled tiara */
        int dr = headr / 3; if (dr < 2) dr = 2;
        uk_fill_circle(w, cxp - hwd / 2, by - dr,          dr, COL_GOLD);
        uk_fill_circle(w, cxp,           by - dr - dr / 2, dr, COL_GOLD);
        uk_fill_circle(w, cxp + hwd / 2, by - dr,          dr, COL_GOLD);
        uk_fill_rect(w, cxp - hwd, by, hwd * 2, bt, COL_GOLD);
    } else {                                /* Jack: banded cap */
        fill_tri(w, cxp - hwd, by, cxp + hwd, by, cxp, by - sp - sp / 2, col);
        uk_fill_rect(w, cxp - hwd, by, hwd * 2, bt, COL_GOLD);
    }
    uk_pop_clip(w);

    uk_draw_rounded_rect_outline(w, ix, iy, iw, ih, 3, col);

    int ps = iw / 4; if (ps < 6) ps = 6;
    draw_suit(w, ix + ps, iy + ps, ps, suit,  1, col);
    draw_suit(w, ix + iw - ps, iy + ih - ps, ps, suit, -1, col);
}

static void draw_card_face(uk_window_t *w, int x, int y, int cw, int ch,
                           const card_t *c)
{
    int rad = cw / 9; if (rad < 3) rad = 3;

    blend_rrect(w, x + 2, y + 3, cw, ch, rad, 0xFF000000, 64);
    uk_fill_rounded_rect(w, x, y, cw, ch, rad, COL_CARD);
    uk_draw_rounded_rect_outline(w, x, y, cw, ch, rad, COL_CARD_EDGE);

    unsigned int col = card_is_red(c) ? COL_RED : COL_BLACK;
    const char *rs   = rank_str(c->rank);
    int is   = (cw >= 84 && c->rank != 10) ? 2 : 1;
    int pad  = cw / 13; if (pad < 3) pad = 3;
    int iwd  = index_width(rs, is);
    int cps  = cw / 7; if (cps < 6) cps = 6;

    draw_index(w, x + pad, y + pad, rs, col, is, false);
    draw_suit(w, x + pad + 4 * is, y + pad + 16 * is + cps / 2 + 1,
              cps, c->suit, 1, col);

    draw_index(w, x + cw - pad - iwd, y + ch - pad - 16 * is, rs, col, is, true);
    draw_suit(w, x + cw - pad - 4 * is, y + ch - pad - 16 * is - cps / 2 - 1,
              cps, c->suit, -1, col);

    if (c->rank >= 11) {
        draw_court(w, x, y, cw, ch, c->rank, c->suit, col);
        return;
    }

    if (c->rank == 1) {
        draw_suit(w, x + cw / 2, y + ch / 2, (cw * 3) / 5, c->suit, 1, col);
        return;
    }

    int npip = 0;
    const pip_t *pl = pip_layout(c->rank, &npip);
    if (!pl) return;

    int ps   = cw / 5; if (ps < 7) ps = 7;
    int colx[3] = { x + (cw * 32) / 100, x + cw / 2, x + (cw * 68) / 100 };
    int ytop = y + (ch * 21) / 100;
    int ybot = y + ch - (ch * 21) / 100;
    for (int i = 0; i < npip; i++) {
        int py = ytop + ((ybot - ytop) * pl[i].row) / 1000;
        int dir = (pl[i].row > 500) ? -1 : 1;
        draw_suit(w, colx[pl[i].col], py, ps, c->suit, dir, col);
    }
}

static void draw_card_back(uk_window_t *w, int x, int y, int cw, int ch)
{
    int rad = cw / 9; if (rad < 3) rad = 3;

    blend_rrect(w, x + 2, y + 3, cw, ch, rad, 0xFF000000, 64);
    uk_fill_rounded_rect(w, x, y, cw, ch, rad, 0xFFF3F1EC);
    uk_fill_rounded_rect(w, x + 3, y + 3, cw - 6, ch - 6, rad - 1, COL_BACK_A);

    uk_push_clip(w, x + 4, y + 4, cw - 8, ch - 8);
    for (int d = -ch; d < cw + ch; d += 8) {
        uk_line(w, x + d, y,      x + d + ch, y + ch, COL_BACK_B);
        uk_line(w, x + d, y + ch, x + d + ch, y,      COL_BACK_B);
    }
    uk_pop_clip(w);

    int mr = cw / 6; if (mr < 5) mr = 5;
    uk_fill_circle(w, x + cw / 2, y + ch / 2, mr, COL_BACK_A);
    uk_fill_circle(w, x + cw / 2, y + ch / 2, mr - 2, COL_BACK_C);
    uk_fill_circle(w, x + cw / 2, y + ch / 2, mr - 4, COL_BACK_A);
    uk_draw_rounded_rect_outline(w, x, y, cw, ch, rad, COL_CARD_EDGE);
}

static void draw_card(uk_window_t *w, int x, int y, int cw, int ch,
                      const card_t *c)
{
    if (c->face_up) draw_card_face(w, x, y, cw, ch, c);
    else            draw_card_back(w, x, y, cw, ch);
}

/* An empty pile: a dark well in the felt. `glyph` is 0 for a plain slot,
 * 'A' for a foundation, or 'R' for the stock's redeal arrow. */
static void draw_slot(uk_window_t *w, int x, int y, int cw, int ch, char glyph)
{
    int rad = cw / 9; if (rad < 3) rad = 3;
    blend_rrect(w, x, y, cw, ch, rad, 0xFF000000, 60);

    for (int i = 0; i < 2; i++)
        uk_draw_rounded_rect_outline(w, x + i, y + i, cw - 2 * i, ch - 2 * i,
                                     rad, uk_blend(COL_FELT_BOT, 0xFFFFFFFF, 40));

    if (glyph == 'A') {
        uk_draw_text_ex(w, x + cw / 2 - 8, y + ch / 2 - 16, "A",
                        COL_SLOT_LINE, &de_font_regular, 2, true);
    } else if (glyph == 'R') {
        int r = cw / 4;
        int cx = x + cw / 2, cy = y + ch / 2;
        for (int a = 0; a < 2; a++)
            uk_draw_rounded_rect_outline(w, cx - r + a, cy - r + a,
                                         2 * r - 2 * a, 2 * r - 2 * a,
                                         r - a, COL_SLOT_LINE);
        fill_tri(w, cx + r - 4, cy - r / 2, cx + r + 4, cy - r / 2,
                    cx + r, cy + 2, COL_SLOT_LINE);
    }
}

/* ============================================================================
 * Game state
 * ==========================================================================*/

enum { ST_PLAY = 0, ST_WIN_ANIM, ST_WIN_PANEL };

static int      g_state      = ST_PLAY;
static int      g_draw_count = 3;      /* 1 or 3 cards per stock click */
static int      g_score      = 0;
static int      g_moves      = 0;
static int      g_passes     = 0;      /* stock redeals used */
static long     g_started_at = 0;      /* time(2) at first move */
static long     g_elapsed    = 0;      /* seconds, frozen once won */
static bool     g_clock_run  = false;
static int      g_bonus      = 0;

/* Undo log. Every state change the player can make is one entry, so undo is
 * a plain pop rather than a re-simulation of the whole deal. */
enum { MV_MOVE = 0, MV_DRAW, MV_REDEAL };

typedef struct {
    unsigned char kind;
    unsigned char from, to;
    unsigned char count;
    bool          flipped;   /* the move exposed and turned a tableau card */
    int           score_delta;
} move_t;

#define UNDO_MAX 512
static move_t g_undo[UNDO_MAX];
static int    g_undo_n = 0;

/* Push a move onto the undo log. A full log drops its *oldest* entry rather
 * than refusing the newest one: undo has to stay truthful about the move
 * just made, and only the far history is expendable. */
static void undo_push(int kind, int from, int to, int count, bool flipped,
                      int score_delta)
{
    if (g_undo_n == UNDO_MAX) {
        memmove(&g_undo[0], &g_undo[1], sizeof(move_t) * (UNDO_MAX - 1));
        g_undo_n--;
    }
    move_t *m = &g_undo[g_undo_n++];
    m->kind = (unsigned char)kind;
    m->from = (unsigned char)from;
    m->to = (unsigned char)to;
    m->count = (unsigned char)count;
    m->flipped = flipped;
    m->score_delta = score_delta;
}

/* Hint highlight */
static int g_hint_pile = -1, g_hint_idx = -1, g_hint_to = -1;
static int g_hint_ticks = 0;

/* Status line message ("No moves left", "Deal 12345 replayed", ...) */
static char g_msg[64] = { 0 };
static int  g_msg_ticks = 0;

static void say(const char *m)
{
    snprintf(g_msg, sizeof(g_msg), "%s", m);
    g_msg_ticks = 75;        /* ~3 s at TICK_MS */
}

static void score_add(int d)
{
    g_score += d;
    if (g_score < 0) g_score = 0;
}

static void clock_start(void)
{
    if (!g_clock_run) {
        g_clock_run = true;
        g_started_at = (long)time(NULL);
    }
}

/* ============================================================================
 * Dealing
 * ==========================================================================*/

static void deal(unsigned int seed)
{
    g_seed_used = seed;
    g_rng = seed * 2654435761u + 0x9E3779B9u;
    for (int i = 0; i < 7; i++) rnd();          /* warm up the xorshift */

    for (int i = 0; i < NPILES; i++) g_pile[i].n = 0;

    int idx = 0;
    for (int s = 0; s < 4; s++)
        for (int r = 1; r <= 13; r++) {
            g_deck[idx].suit = (unsigned char)s;
            g_deck[idx].rank = (unsigned char)r;
            g_deck[idx].face_up = false;
            idx++;
        }

    for (int i = 51; i > 0; i--) {
        int j = (int)(rnd() % (unsigned int)(i + 1));
        card_t t = g_deck[i];
        g_deck[i] = g_deck[j];
        g_deck[j] = t;
    }

    idx = 0;
    for (int row = 0; row < 7; row++)
        for (int col = row; col < 7; col++) {
            card_t *c = &g_deck[idx++];
            c->face_up = (row == col);
            pile_t *p = &g_pile[P_TAB + col];
            p->c[p->n++] = c;
        }
    while (idx < 52) {
        card_t *c = &g_deck[idx++];
        c->face_up = false;
        g_pile[P_STOCK].c[g_pile[P_STOCK].n++] = c;
    }

    g_state = ST_PLAY;
    g_score = g_moves = g_passes = 0;
    g_elapsed = 0;
    g_bonus = 0;
    g_clock_run = false;
    g_undo_n = 0;
    g_hint_pile = g_hint_to = -1;
    g_hint_ticks = 0;
    g_msg[0] = 0;
    g_msg_ticks = 0;
}

/* ============================================================================
 * Rules
 * ==========================================================================*/

static bool is_won(void)
{
    for (int i = 0; i < 4; i++)
        if (g_pile[P_FOUND + i].n != 13) return false;
    return true;
}

/* Can `first` (and the count-1 cards riding on it) land on pile `to`? */
static bool can_drop(int to, const card_t *first, int count)
{
    if (to >= P_FOUND && to < P_FOUND + 4) {
        if (count != 1) return false;
        card_t *t = pile_top(&g_pile[to]);
        if (!t) return first->rank == 1;
        return t->suit == first->suit && first->rank == t->rank + 1;
    }
    if (to >= P_TAB && to < P_TAB + 7) {
        card_t *t = pile_top(&g_pile[to]);
        if (!t) return first->rank == 13;
        if (!t->face_up) return false;
        return card_is_red(t) != card_is_red(first) && first->rank + 1 == t->rank;
    }
    return false;
}

/* A tableau run is grabbable only if it is a face-up, descending,
 * alternating-colour sequence. Legal play can never build anything else,
 * but the check keeps a corrupted state from becoming a corrupted move. */
static bool run_is_valid(int from, int idx)
{
    pile_t *p = &g_pile[from];
    if (idx < 0 || idx >= p->n) return false;
    for (int k = idx; k < p->n; k++) {
        if (!p->c[k]->face_up) return false;
        if (k > idx) {
            card_t *a = p->c[k - 1], *b = p->c[k];
            if (card_is_red(a) == card_is_red(b)) return false;
            if (b->rank + 1 != a->rank) return false;
        }
    }
    return true;
}

static bool grabbable(int from, int idx)
{
    if (from == P_WASTE)
        return idx == g_pile[P_WASTE].n - 1 && g_pile[P_WASTE].n > 0;
    if (from >= P_FOUND && from < P_FOUND + 4)
        return idx == g_pile[from].n - 1 && g_pile[from].n > 0;
    if (from >= P_TAB && from < P_TAB + 7)
        return run_is_valid(from, idx);
    return false;
}

/* Microsoft Solitaire's standard scoring, so a score here means what a
 * player expects it to mean. */
static int score_for(int from, int to)
{
    bool to_found = (to >= P_FOUND && to < P_FOUND + 4);
    bool to_tab   = (to >= P_TAB);
    if (from == P_WASTE && to_tab)   return 5;
    if (from == P_WASTE && to_found) return 10;
    if (from >= P_TAB && to_found)   return 10;
    if (from >= P_FOUND && from < P_FOUND + 4 && to_tab) return -15;
    return 0;
}

/* Move the top `count` cards of `from` onto `to`, scoring, flipping the card
 * it uncovers and logging the whole thing for undo. Legality is the
 * caller's job — every caller has already asked can_drop(). */
static void do_move(int from, int to, int count)
{
    pile_t *src = &g_pile[from], *dst = &g_pile[to];
    int base = src->n - count;

    for (int k = 0; k < count; k++)
        dst->c[dst->n++] = src->c[base + k];
    src->n = base;

    int delta = score_for(from, to);
    bool flipped = false;
    if (from >= P_TAB && src->n > 0 && !src->c[src->n - 1]->face_up) {
        src->c[src->n - 1]->face_up = true;
        flipped = true;
        delta += 5;
    }
    score_add(delta);
    g_moves++;
    clock_start();

    undo_push(MV_MOVE, from, to, count, flipped, delta);
}

static void do_draw(void)
{
    pile_t *s = &g_pile[P_STOCK], *w = &g_pile[P_WASTE];

    if (s->n == 0) {
        if (w->n == 0) return;
        /* Redeal: the waste goes back under the stock in reverse, so the
         * next pass shows the same cards in the same order. */
        int n = w->n;
        while (w->n > 0) {
            card_t *c = w->c[--w->n];
            c->face_up = false;
            s->c[s->n++] = c;
        }
        g_passes++;
        int delta = (g_draw_count == 1) ? -100 : -20;
        if (g_score + delta < 0) delta = -g_score;
        score_add(delta);
        g_moves++;
        clock_start();
        undo_push(MV_REDEAL, P_WASTE, P_STOCK, n, false, delta);
        return;
    }

    int take = g_draw_count;
    if (take > s->n) take = s->n;
    for (int i = 0; i < take; i++) {
        card_t *c = s->c[--s->n];
        c->face_up = true;
        w->c[w->n++] = c;
    }
    g_moves++;
    clock_start();
    undo_push(MV_DRAW, P_STOCK, P_WASTE, take, false, 0);
}

static void do_undo(void)
{
    if (g_undo_n == 0) { say("Nothing to undo"); return; }
    move_t *m = &g_undo[--g_undo_n];

    if (m->kind == MV_DRAW) {
        pile_t *s = &g_pile[P_STOCK], *w = &g_pile[P_WASTE];
        for (int i = 0; i < m->count && w->n > 0; i++) {
            card_t *c = w->c[--w->n];
            c->face_up = false;
            s->c[s->n++] = c;
        }
    } else if (m->kind == MV_REDEAL) {
        pile_t *s = &g_pile[P_STOCK], *w = &g_pile[P_WASTE];
        while (s->n > 0) {
            card_t *c = s->c[--s->n];
            c->face_up = true;
            w->c[w->n++] = c;
        }
        if (g_passes > 0) g_passes--;
    } else {
        pile_t *src = &g_pile[m->from], *dst = &g_pile[m->to];
        if (m->flipped && src->n > 0)
            src->c[src->n - 1]->face_up = false;
        int base = dst->n - m->count;
        if (base < 0) base = 0;
        for (int k = base; k < dst->n; k++)
            src->c[src->n++] = dst->c[k];
        dst->n = base;
    }

    score_add(-m->score_delta);
    if (g_moves > 0) g_moves--;
    g_state = ST_PLAY;
    g_hint_pile = g_hint_to = -1;
}

/* ============================================================================
 * Assisted play: click-to-place, hints, auto-finish
 * ==========================================================================*/

/* Where would this card like to go? Foundations first, then a tableau that
 * is not the column it already sits at the bottom of (which would be a
 * no-op shuffle between empty columns). */
static int best_target(int from, int idx)
{
    pile_t *p = &g_pile[from];
    int count = p->n - idx;
    card_t *first = p->c[idx];

    if (count == 1)
        for (int f = 0; f < 4; f++)
            if (can_drop(P_FOUND + f, first, 1)) return P_FOUND + f;

    for (int t = 0; t < 7; t++) {
        int to = P_TAB + t;
        if (to == from) continue;
        if (g_pile[to].n == 0 && from >= P_TAB && idx == 0) continue;
        if (can_drop(to, first, count)) return to;
    }
    return -1;
}

static bool try_auto_place(int from, int idx)
{
    if (!grabbable(from, idx)) return false;
    int to = best_target(from, idx);
    if (to < 0) return false;
    do_move(from, to, g_pile[from].n - idx);
    return true;
}

/* First useful move, in the order a decent player would consider them.
 * Returns false when only drawing (or nothing at all) is left. */
static bool find_hint(int *sp, int *si, int *dp)
{
    /* 1. Anything to a foundation. */
    if (g_pile[P_WASTE].n > 0) {
        int i = g_pile[P_WASTE].n - 1;
        for (int f = 0; f < 4; f++)
            if (can_drop(P_FOUND + f, g_pile[P_WASTE].c[i], 1)) {
                *sp = P_WASTE; *si = i; *dp = P_FOUND + f; return true;
            }
    }
    for (int t = 0; t < 7; t++) {
        pile_t *p = &g_pile[P_TAB + t];
        if (p->n == 0) continue;
        for (int f = 0; f < 4; f++)
            if (can_drop(P_FOUND + f, p->c[p->n - 1], 1)) {
                *sp = P_TAB + t; *si = p->n - 1; *dp = P_FOUND + f; return true;
            }
    }

    /* 2. A tableau run that uncovers a face-down card. */
    for (int t = 0; t < 7; t++) {
        pile_t *p = &g_pile[P_TAB + t];
        for (int i = 0; i < p->n; i++) {
            if (!p->c[i]->face_up) continue;
            if (i == 0 || p->c[i - 1]->face_up) break;
            if (!run_is_valid(P_TAB + t, i)) break;
            for (int d = 0; d < 7; d++) {
                if (d == t) continue;
                if (can_drop(P_TAB + d, p->c[i], p->n - i)) {
                    *sp = P_TAB + t; *si = i; *dp = P_TAB + d; return true;
                }
            }
            break;
        }
    }

    /* 3. The waste onto a tableau. */
    if (g_pile[P_WASTE].n > 0) {
        int i = g_pile[P_WASTE].n - 1;
        for (int d = 0; d < 7; d++)
            if (can_drop(P_TAB + d, g_pile[P_WASTE].c[i], 1)) {
                *sp = P_WASTE; *si = i; *dp = P_TAB + d; return true;
            }
    }

    /* 4. A King out of a pile that has something under it. */
    for (int t = 0; t < 7; t++) {
        pile_t *p = &g_pile[P_TAB + t];
        for (int i = 1; i < p->n; i++) {
            if (!p->c[i]->face_up) continue;
            if (!run_is_valid(P_TAB + t, i)) break;
            for (int d = 0; d < 7; d++) {
                if (d == t) continue;
                if (g_pile[P_TAB + d].n != 0) continue;
                if (can_drop(P_TAB + d, p->c[i], p->n - i)) {
                    *sp = P_TAB + t; *si = i; *dp = P_TAB + d; return true;
                }
            }
            break;
        }
    }
    return false;
}

/* Auto-finish is offered once nothing is face-down any more: from there the
 * deal is solved and clicking it out card by card is busywork. */
static bool auto_available(void)
{
    if (g_state != ST_PLAY || is_won()) return false;
    for (int t = 0; t < 7; t++) {
        pile_t *p = &g_pile[P_TAB + t];
        for (int i = 0; i < p->n; i++)
            if (!p->c[i]->face_up) return false;
    }
    return true;
}

static bool g_auto_running = false;
static int  g_auto_stall = 0;

/* One auto-finish step per tick, so the cards visibly walk to the
 * foundations instead of teleporting there. */
static bool auto_step(void)
{
    if (g_pile[P_WASTE].n > 0) {
        card_t *c = pile_top(&g_pile[P_WASTE]);
        for (int f = 0; f < 4; f++)
            if (can_drop(P_FOUND + f, c, 1)) {
                do_move(P_WASTE, P_FOUND + f, 1);
                g_auto_stall = 0;
                return true;
            }
    }
    for (int t = 0; t < 7; t++) {
        pile_t *p = &g_pile[P_TAB + t];
        if (p->n == 0) continue;
        for (int f = 0; f < 4; f++)
            if (can_drop(P_FOUND + f, p->c[p->n - 1], 1)) {
                do_move(P_TAB + t, P_FOUND + f, 1);
                g_auto_stall = 0;
                return true;
            }
    }
    if (g_pile[P_STOCK].n > 0 || g_pile[P_WASTE].n > 0) {
        if (g_auto_stall++ > 60) return false;   /* nothing playable left */
        do_draw();
        return true;
    }
    return false;
}

/* ============================================================================
 * Layout
 *
 * Recomputed every frame from the window size and the current tallest
 * column, so the board fills whatever the compositor gives us and a long
 * tableau tightens its fan instead of running off the bottom edge.
 * ==========================================================================*/

typedef struct {
    int cw, ch, gap;
    int tool_h, stat_h;
    int top_y, tab_y;
    int col_x[7];
    int stock_x, waste_x, waste_fan;
    int found_x[4];
    int fan_up, fan_down;
} layout_t;

static uk_window_t g_win;
static layout_t    L;

enum { BTN_NEW = 0, BTN_REPLAY, BTN_UNDO, BTN_HINT, BTN_AUTO, BTN_DRAW,
       BTN_HELP, BTN_COUNT };

static uk_rect_t g_btn_r[BTN_COUNT];
static int       g_hover_btn = -1;

static void compute_layout(void)
{
    int W = (int)g_win.width, H = (int)g_win.height;
    L.tool_h = 34;
    L.stat_h = 22;

    int margin = 12;
    int avail_w = W - 2 * margin;
    if (avail_w < 260) avail_w = 260;

    int gap = avail_w / 70;
    if (gap < 6) gap = 6;
    if (gap > 18) gap = 18;

    int cw = (avail_w - 6 * gap) / 7;
    if (cw > 104) cw = 104;
    if (cw < 38) cw = 38;
    int ch = (cw * 7) / 5;

    /* Two full card heights plus a little fan is the floor for a playable
     * board; shrink the cards rather than clip the tableau. */
    int avail_h = H - L.tool_h - L.stat_h - 3 * margin;
    while (cw > 38 && ch * 2 + 40 > avail_h) {
        cw -= 2;
        ch = (cw * 7) / 5;
    }

    L.cw = cw; L.ch = ch; L.gap = gap;

    int total = 7 * cw + 6 * gap;
    int x0 = (W - total) / 2;
    if (x0 < 6) x0 = 6;
    for (int i = 0; i < 7; i++) L.col_x[i] = x0 + i * (cw + gap);

    L.top_y = L.tool_h + margin;
    L.tab_y = L.top_y + ch + gap + 6;
    L.stock_x = L.col_x[0];
    L.waste_x = L.col_x[1];
    L.waste_fan = cw / 3;
    for (int i = 0; i < 4; i++) L.found_x[i] = L.col_x[3 + i];

    int fu = ch / 4;  if (fu < 10) fu = 10;
    int fd = ch / 10; if (fd < 4)  fd = 4;
    int col_h = H - L.stat_h - L.tab_y - 8;
    if (col_h < ch + 24) col_h = ch + 24;

    for (;;) {
        int need = ch;
        for (int i = 0; i < 7; i++) {
            pile_t *p = &g_pile[P_TAB + i];
            int nd = 0, nu = 0;
            for (int k = 0; k < p->n; k++) {
                if (p->c[k]->face_up) nu++;
                else                  nd++;
            }
            int hgt = (p->n == 0) ? ch
                                  : nd * fd + (nu > 0 ? (nu - 1) * fu : 0) + ch;
            if (hgt > need) need = hgt;
        }
        if (need <= col_h) break;
        if (fu > 8)      fu--;
        else if (fd > 3) fd--;
        else             break;
    }
    L.fan_up = fu;
    L.fan_down = fd;

    /* Toolbar */
    static const char *labels[BTN_COUNT] =
        { "New", "Replay", "Undo", "Hint", "Auto", "Draw 3", "?" };
    int bx = 10, by = 5, bh = 24;
    for (int i = 0; i < BTN_HELP; i++) {
        int bw = uk_strlen(labels[i]) * 8 + 18;
        g_btn_r[i] = (uk_rect_t){ bx, by, bw, bh };
        bx += bw + 6;
    }
    g_btn_r[BTN_HELP] = (uk_rect_t){ W - 34, by, 26, bh };
}

static int tab_card_y(int col, int idx)
{
    pile_t *p = &g_pile[P_TAB + col];
    int y = L.tab_y;
    for (int k = 0; k < idx && k < p->n; k++)
        y += p->c[k]->face_up ? L.fan_up : L.fan_down;
    return y;
}

/* Where the top `n` waste cards are fanned. Index is 0..min(n,3)-1. */
static int waste_visible(void)
{
    int n = g_pile[P_WASTE].n;
    int vis = (g_draw_count == 1) ? 1 : 3;
    return n < vis ? n : vis;
}

/* ============================================================================
 * Drag state
 *
 * A drag never removes cards from their pile — it only marks where the
 * lifted run starts. Keeping the pile intact means the fan geometry stays
 * put under the cursor and the move engine still sees one atomic do_move()
 * when the run lands.
 * ==========================================================================*/

static int  g_press_pile = -1, g_press_idx = -1;
static int  g_press_x, g_press_y;
static int  g_grab_ox, g_grab_oy;     /* cursor offset inside the grabbed card */
static bool g_drag_active = false;
static int  g_mx = 0, g_my = 0;
static bool g_help = false;

static bool card_hidden_by_drag(int pile, int idx)
{
    return g_drag_active && pile == g_press_pile && idx >= g_press_idx;
}

/* ============================================================================
 * Hit testing
 * ==========================================================================*/

/* Topmost card under (mx,my). Returns the pile id, or -1. `*idx` is the card
 * index, or -1 when an empty pile's slot was hit. */
static int hit_test(int mx, int my, int *idx)
{
    *idx = -1;
    int cw = L.cw, ch = L.ch;

    if (mx >= L.stock_x && mx < L.stock_x + cw &&
        my >= L.top_y && my < L.top_y + ch) {
        *idx = g_pile[P_STOCK].n - 1;
        return P_STOCK;
    }

    int vis = waste_visible();
    for (int i = vis - 1; i >= 0; i--) {
        int x = L.waste_x + i * L.waste_fan;
        if (mx >= x && mx < x + cw && my >= L.top_y && my < L.top_y + ch) {
            *idx = g_pile[P_WASTE].n - vis + i;
            return P_WASTE;
        }
    }

    for (int f = 0; f < 4; f++)
        if (mx >= L.found_x[f] && mx < L.found_x[f] + cw &&
            my >= L.top_y && my < L.top_y + ch) {
            *idx = g_pile[P_FOUND + f].n - 1;
            return P_FOUND + f;
        }

    for (int t = 0; t < 7; t++) {
        if (mx < L.col_x[t] || mx >= L.col_x[t] + cw) continue;
        pile_t *p = &g_pile[P_TAB + t];
        if (p->n == 0) {
            if (my >= L.tab_y && my < L.tab_y + ch) return P_TAB + t;
            return -1;
        }
        for (int i = p->n - 1; i >= 0; i--) {
            int cy = tab_card_y(t, i);
            int bot = (i == p->n - 1) ? cy + ch
                                      : cy + (p->c[i]->face_up ? L.fan_up : L.fan_down);
            if (my >= cy && my < bot) { *idx = i; return P_TAB + t; }
        }
        return -1;
    }
    return -1;
}

/* Which pile would a card dropped with its centre at (px,py) land on? */
static int drop_target(int px, int py)
{
    int cw = L.cw, ch = L.ch;
    for (int f = 0; f < 4; f++)
        if (px >= L.found_x[f] && px < L.found_x[f] + cw &&
            py >= L.top_y && py < L.top_y + ch)
            return P_FOUND + f;
    for (int t = 0; t < 7; t++)
        if (px >= L.col_x[t] && px < L.col_x[t] + cw && py >= L.tab_y)
            return P_TAB + t;
    return -1;
}

/* ============================================================================
 * Win animation — the cards pour off the foundations and bounce away.
 *
 * The frame is deliberately not cleared while this runs, so each card
 * smears a trail across the felt exactly like the original.
 * ==========================================================================*/

#define MAX_FALL 10
typedef struct {
    int    x16, y16, vx16, vy16;
    card_t card;
    bool   live;
} fall_t;

static fall_t g_fall[MAX_FALL];
static int    g_fall_timer = 0;

static void win_anim_start(void)
{
    for (int i = 0; i < MAX_FALL; i++) g_fall[i].live = false;
    g_fall_timer = 0;
    g_state = ST_WIN_ANIM;
    g_clock_run = false;
    g_bonus = (g_elapsed > 30) ? (int)(700000L / g_elapsed) : 20000;
    score_add(g_bonus);
}

static bool win_anim_step(void)
{
    int W = (int)g_win.width, H = (int)g_win.height;
    bool any = false;

    if (--g_fall_timer <= 0) {
        g_fall_timer = 5;
        for (int f = 3; f >= 0; f--) {
            pile_t *p = &g_pile[P_FOUND + f];
            if (p->n == 0) continue;
            for (int i = 0; i < MAX_FALL; i++) {
                if (g_fall[i].live) continue;
                card_t *c = p->c[--p->n];
                g_fall[i].card = *c;
                g_fall[i].x16 = L.found_x[f] * 16;
                g_fall[i].y16 = L.top_y * 16;
                int dir = (rnd() & 1) ? 1 : -1;
                g_fall[i].vx16 = dir * (int)(32 + (rnd() % 72));
                g_fall[i].vy16 = -(int)(rnd() % 80);
                g_fall[i].live = true;
                break;
            }
            break;
        }
    }

    for (int i = 0; i < MAX_FALL; i++) {
        if (!g_fall[i].live) continue;
        any = true;
        g_fall[i].vy16 += 14;
        g_fall[i].x16 += g_fall[i].vx16;
        g_fall[i].y16 += g_fall[i].vy16;

        int floor16 = (H - L.ch) * 16;
        if (g_fall[i].y16 >= floor16 && g_fall[i].vy16 > 0) {
            g_fall[i].y16 = floor16;
            g_fall[i].vy16 = -(g_fall[i].vy16 * 7) / 10;
            if (g_fall[i].vy16 > -40) g_fall[i].vy16 = -40;
        }
        int px = g_fall[i].x16 / 16;
        if (px + L.cw < 0 || px > W) { g_fall[i].live = false; continue; }
        draw_card(&g_win, px, g_fall[i].y16 / 16, L.cw, L.ch, &g_fall[i].card);
    }

    int left = 0;
    for (int f = 0; f < 4; f++) left += g_pile[P_FOUND + f].n;
    return any || left > 0;
}

/* ============================================================================
 * Rendering
 * ==========================================================================*/

static void card_rect(int pile, int idx, int *x, int *y)
{
    if (pile == P_STOCK)              { *x = L.stock_x; *y = L.top_y; return; }
    if (pile >= P_FOUND && pile < P_FOUND + 4) {
        *x = L.found_x[pile - P_FOUND]; *y = L.top_y; return;
    }
    if (pile == P_WASTE) {
        int vis = waste_visible();
        int slot = idx - (g_pile[P_WASTE].n - vis);
        if (slot < 0) slot = 0;
        *x = L.waste_x + slot * L.waste_fan;
        *y = L.top_y;
        return;
    }
    int t = pile - P_TAB;
    *x = L.col_x[t];
    *y = (idx < 0) ? L.tab_y : tab_card_y(t, idx);
}

static void outline_thick(int x, int y, int w, int h, unsigned int col)
{
    int rad = L.cw / 9; if (rad < 3) rad = 3;
    for (int i = 0; i < 3; i++)
        uk_draw_rounded_rect_outline(&g_win, x - i, y - i, w + 2 * i, h + 2 * i,
                                     rad + i, col);
}

static void draw_toolbar(void)
{
    int W = (int)g_win.width;
    uk_gradient_v(&g_win, 0, 0, W, L.tool_h, UK_SURFACE0, UK_MANTLE);
    uk_fill_rect(&g_win, 0, L.tool_h - 1, W, 1, UK_CRUST);

    static const char *labels[BTN_COUNT] =
        { "New", "Replay", "Undo", "Hint", "Auto", "Draw 3", "?" };
    char drawlbl[8];
    snprintf(drawlbl, sizeof(drawlbl), "Draw %d", g_draw_count);

    for (int i = 0; i < BTN_COUNT; i++) {
        const char *lab = (i == BTN_DRAW) ? drawlbl : labels[i];
        bool on = true;
        if (i == BTN_UNDO) on = (g_undo_n > 0);
        if (i == BTN_HINT) on = (g_state == ST_PLAY);
        if (i == BTN_AUTO) on = auto_available();
        uk_btn_state_t st = !on ? UK_BTN_DISABLED
                          : (g_hover_btn == i ? UK_BTN_HOVER : UK_BTN_NORMAL);
        uk_rect_t r = g_btn_r[i];
        uk_draw_button(&g_win, r.x, r.y, r.w, r.h, lab, st);
    }
}

static void draw_status(void)
{
    int W = (int)g_win.width, H = (int)g_win.height;
    int y = H - L.stat_h;
    uk_fill_rect(&g_win, 0, y, W, L.stat_h, UK_CRUST);
    uk_fill_rect(&g_win, 0, y, W, 1, UK_SURFACE0);

    int msg_w = (g_msg_ticks > 0 && g_msg[0]) ? uk_strlen(g_msg) * 8 + 16 : 0;

    char buf[128];
    long secs = g_elapsed;
    snprintf(buf, sizeof(buf),
             "Score %d   Time %02d:%02d   Moves %d   Deal #%u   Redeals %d   Stock %d",
             g_score, (int)(secs / 60), (int)(secs % 60), g_moves,
             g_seed_used, g_passes, g_pile[P_STOCK].n);
    if (uk_strlen(buf) * 8 + 16 + msg_w > W)
        snprintf(buf, sizeof(buf), "%d   %02d:%02d   #%u",
                 g_score, (int)(secs / 60), (int)(secs % 60), g_seed_used);
    uk_push_clip(&g_win, 0, y, W - msg_w, L.stat_h);
    uk_draw_text_ex(&g_win, 8, y + 7, buf, UK_SUBTEXT0, &de_font_small, 1, false);
    uk_pop_clip(&g_win);

    if (msg_w) {
        uk_draw_text_ex(&g_win, W - msg_w + 8, y + 7, g_msg, UK_YELLOW,
                        &de_font_small, 1, false);
    }
}

static void draw_help(void)
{
    int W = (int)g_win.width, H = (int)g_win.height;
    blend_rect(&g_win, 0, 0, W, H, 0xFF000000, 150);

    static const char *lines[] = {
        "Klondike Solitaire",
        "",
        "Build the four foundations up from Ace to King in one suit.",
        "Tableau columns build down, alternating colours; an empty",
        "column takes a King. Click the stock to turn cards over.",
        "",
        "Drag        move a card or a whole run",
        "Click       send a card wherever it will legally go",
        "Right-click undo the last move",
        "",
        "N  new deal            R  replay this deal",
        "U  undo                H  hint",
        "A  auto-finish         D  toggle draw 1 / draw 3",
        "?  this help           Q / Esc  quit",
    };
    int n = (int)(sizeof(lines) / sizeof(lines[0]));
    int pw = 520, ph = n * 18 + 58;
    if (pw > W - 40) pw = W - 40;
    if (ph > H - 40) ph = H - 40;
    int px = (W - pw) / 2, py = (H - ph) / 2;

    blend_rrect(&g_win, px + 4, py + 6, pw, ph, 10, 0xFF000000, 90);
    uk_fill_rounded_rect(&g_win, px, py, pw, ph, 10, UK_BASE);
    uk_draw_rounded_rect_outline(&g_win, px, py, pw, ph, 10, UK_LAVENDER);

    uk_push_clip(&g_win, px + 8, py + 8, pw - 16, ph - 16);
    int ty = py + 18;
    for (int i = 0; i < n; i++) {
        unsigned int col = (i == 0) ? UK_LAVENDER
                         : (i >= 6) ? UK_TEXT : UK_SUBTEXT1;
        uk_draw_text_ex(&g_win, px + 20, ty, lines[i], col,
                        &de_font_regular, 1, i == 0);
        ty += 18;
    }
    uk_pop_clip(&g_win);
    uk_draw_text_ex(&g_win, px + 20, py + ph - 24, "Click anywhere to close",
                    UK_OVERLAY1, &de_font_small, 1, false);
}

static uk_rect_t g_win_btn[2];

static void draw_win_panel(void)
{
    int W = (int)g_win.width, H = (int)g_win.height;
    blend_rect(&g_win, 0, 0, W, H, 0xFF000000, 130);

    int pw = 380, ph = 208;
    if (pw > W - 40) pw = W - 40;
    int px = (W - pw) / 2, py = (H - ph) / 2;

    blend_rrect(&g_win, px + 4, py + 6, pw, ph, 12, 0xFF000000, 100);
    uk_fill_rounded_rect(&g_win, px, py, pw, ph, 12, UK_BASE);
    uk_draw_rounded_rect_outline(&g_win, px, py, pw, ph, 12, UK_YELLOW);

    const char *title = "You won!";
    uk_draw_text_ex(&g_win, px + pw / 2 - uk_strlen(title) * 8, py + 22, title,
                    UK_YELLOW, &de_font_regular, 2, true);

    char buf[64];
    int ty = py + 76;
    snprintf(buf, sizeof(buf), "Final score   %d", g_score);
    uk_draw_text(&g_win, px + 28, ty, buf, UK_TEXT); ty += 20;
    snprintf(buf, sizeof(buf), "Time bonus    %d", g_bonus);
    uk_draw_text(&g_win, px + 28, ty, buf, UK_SUBTEXT0); ty += 20;
    snprintf(buf, sizeof(buf), "Time          %02d:%02d",
             (int)(g_elapsed / 60), (int)(g_elapsed % 60));
    uk_draw_text(&g_win, px + 28, ty, buf, UK_SUBTEXT0); ty += 20;
    snprintf(buf, sizeof(buf), "Moves         %d   (deal #%u)", g_moves, g_seed_used);
    uk_draw_text(&g_win, px + 28, ty, buf, UK_SUBTEXT0);

    int bw = (pw - 3 * 20) / 2, bh = 28;
    g_win_btn[0] = (uk_rect_t){ px + 20, py + ph - bh - 18, bw, bh };
    g_win_btn[1] = (uk_rect_t){ px + 20 + bw + 20, py + ph - bh - 18, bw, bh };
    uk_draw_button(&g_win, g_win_btn[0].x, g_win_btn[0].y, bw, bh, "New deal",
                   UK_BTN_NORMAL);
    uk_draw_button(&g_win, g_win_btn[1].x, g_win_btn[1].y, bw, bh, "Replay",
                   UK_BTN_NORMAL);
}

static void render(void)
{
    compute_layout();
    int W = (int)g_win.width, H = (int)g_win.height;
    int cw = L.cw, ch = L.ch;

    uk_gradient_v(&g_win, 0, 0, W, H, COL_FELT_TOP, COL_FELT_BOT);

    /* Stock */
    if (g_pile[P_STOCK].n > 0) {
        int layers = g_pile[P_STOCK].n / 12;
        for (int i = layers; i > 0; i--)
            draw_card_back(&g_win, L.stock_x + i, L.top_y - i, cw, ch);
        draw_card_back(&g_win, L.stock_x, L.top_y, cw, ch);
    } else {
        draw_slot(&g_win, L.stock_x, L.top_y, cw, ch,
                  g_pile[P_WASTE].n > 0 ? 'R' : 0);
    }

    /* Waste */
    int vis = waste_visible();
    if (vis == 0) {
        draw_slot(&g_win, L.waste_x, L.top_y, cw, ch, 0);
    } else {
        for (int i = 0; i < vis; i++) {
            int idx = g_pile[P_WASTE].n - vis + i;
            if (card_hidden_by_drag(P_WASTE, idx)) continue;
            draw_card(&g_win, L.waste_x + i * L.waste_fan, L.top_y, cw, ch,
                      g_pile[P_WASTE].c[idx]);
        }
    }

    /* Foundations */
    for (int f = 0; f < 4; f++) {
        pile_t *p = &g_pile[P_FOUND + f];
        int shown = p->n;
        if (card_hidden_by_drag(P_FOUND + f, p->n - 1)) shown--;
        if (shown <= 0) draw_slot(&g_win, L.found_x[f], L.top_y, cw, ch, 'A');
        else            draw_card(&g_win, L.found_x[f], L.top_y, cw, ch,
                                  p->c[shown - 1]);
    }

    /* Tableau */
    for (int t = 0; t < 7; t++) {
        pile_t *p = &g_pile[P_TAB + t];
        if (p->n == 0 ||
            (g_drag_active && g_press_pile == P_TAB + t && g_press_idx == 0)) {
            draw_slot(&g_win, L.col_x[t], L.tab_y, cw, ch, 0);
        }
        int cy = L.tab_y;
        for (int i = 0; i < p->n; i++) {
            if (card_hidden_by_drag(P_TAB + t, i)) break;
            int step = p->c[i]->face_up ? L.fan_up : L.fan_down;
            bool covered = (i + 1 < p->n) && !card_hidden_by_drag(P_TAB + t, i + 1);
            if (covered) uk_push_clip(&g_win, L.col_x[t] - 4, cy, cw + 10, step + 8);
            draw_card(&g_win, L.col_x[t], cy, cw, ch, p->c[i]);
            if (covered) uk_pop_clip(&g_win);
            cy += step;
        }
    }

    /* Hint highlight */
    if (g_hint_ticks > 0 && g_hint_pile >= 0) {
        int hx, hy;
        if (g_hint_idx >= 0) {
            card_rect(g_hint_pile, g_hint_idx, &hx, &hy);
            outline_thick(hx, hy, cw, ch, COL_HINT);
        }
        if (g_hint_to >= 0) {
            pile_t *d = &g_pile[g_hint_to];
            card_rect(g_hint_to, d->n - 1, &hx, &hy);
            outline_thick(hx, hy, cw, ch, UK_GREEN);
        }
    }

    /* The run being dragged, floating under the cursor. */
    if (g_drag_active && g_press_pile >= 0) {
        pile_t *p = &g_pile[g_press_pile];
        int dx = g_mx - g_grab_ox;
        int dy = g_my - g_grab_oy;
        for (int i = g_press_idx; i < p->n; i++) {
            draw_card(&g_win, dx, dy, cw, ch, p->c[i]);
            dy += L.fan_up;
        }
    }

    draw_toolbar();
    draw_status();
    if (g_help) draw_help();
    if (g_state == ST_WIN_PANEL) draw_win_panel();

    uk_invalidate(&g_win);
}

/* ============================================================================
 * Actions
 * ==========================================================================*/

static void new_deal(unsigned int seed)
{
    g_auto_running = false;
    g_drag_active = false;
    g_press_pile = -1;
    deal(seed ? seed : entropy_seed());
}

static void after_change(void)
{
    g_hint_pile = g_hint_to = -1;
    g_hint_ticks = 0;
    if (g_state == ST_PLAY && is_won()) {
        g_auto_running = false;
        if (g_clock_run) g_elapsed = (long)time(NULL) - g_started_at;
        win_anim_start();
        render();          /* the last full board, for the trails to smear */
    }
}

static void do_hint(void)
{
    int sp, si, dp;
    if (find_hint(&sp, &si, &dp)) {
        g_hint_pile = sp; g_hint_idx = si; g_hint_to = dp;
        g_hint_ticks = 50;
        say("");
        g_msg_ticks = 0;
    } else if (g_pile[P_STOCK].n > 0 || g_pile[P_WASTE].n > 1) {
        g_hint_pile = P_STOCK; g_hint_idx = g_pile[P_STOCK].n - 1;
        g_hint_to = -1;
        g_hint_ticks = 50;
        say("Turn the stock over");
    } else {
        say("No moves left");
    }
}

static void toggle_auto(void)
{
    if (!auto_available()) { say("Auto needs every card face up"); return; }
    g_auto_running = !g_auto_running;
    g_auto_stall = 0;
}

/* ============================================================================
 * Input
 * ==========================================================================*/

static bool toolbar_click(int mx, int my)
{
    for (int i = 0; i < BTN_COUNT; i++) {
        if (!uk_hit_rect(g_btn_r[i], mx, my)) continue;
        switch (i) {
        case BTN_NEW:    new_deal(0); break;
        case BTN_REPLAY: new_deal(g_seed_used); say("Deal replayed"); break;
        case BTN_UNDO:   do_undo(); break;
        case BTN_HINT:   do_hint(); break;
        case BTN_AUTO:   toggle_auto(); break;
        case BTN_DRAW:
            g_draw_count = (g_draw_count == 3) ? 1 : 3;
            say(g_draw_count == 1 ? "Draw one" : "Draw three");
            break;
        case BTN_HELP:   g_help = !g_help; break;
        }
        return true;
    }
    return false;
}

static void on_press(int mx, int my, int btn)
{
    if (g_help) { g_help = false; return; }

    if (g_state == ST_WIN_ANIM) {
        g_state = ST_WIN_PANEL;
        return;
    }
    if (g_state == ST_WIN_PANEL) {
        if (uk_hit_rect(g_win_btn[0], mx, my)) new_deal(0);
        else if (uk_hit_rect(g_win_btn[1], mx, my)) new_deal(g_seed_used);
        return;
    }

    /* Any press ends whatever drag was in flight: a release delivered
     * outside the window (or onto the toolbar) must never leave a run
     * stuck to the cursor. */
    g_drag_active = false;
    g_press_pile = -1;

    if (my < L.tool_h) { toolbar_click(mx, my); return; }
    if (my >= (int)g_win.height - L.stat_h) return;

    g_auto_running = false;

    if (btn == 2) { do_undo(); return; }

    int idx;
    int pile = hit_test(mx, my, &idx);
    if (pile < 0) return;

    if (pile == P_STOCK) {
        do_draw();
        after_change();
        return;
    }

    if (idx < 0 || !grabbable(pile, idx)) return;

    int cx, cy;
    card_rect(pile, idx, &cx, &cy);
    g_press_pile = pile;
    g_press_idx  = idx;
    g_press_x = mx; g_press_y = my;
    g_grab_ox = mx - cx;
    g_grab_oy = my - cy;
}

static void on_release(int mx, int my)
{
    if (g_press_pile < 0) return;

    if (!g_drag_active) {
        /* A click, not a drag: send the card wherever it will go. */
        if (try_auto_place(g_press_pile, g_press_idx)) after_change();
        g_press_pile = -1;
        return;
    }

    int cx = mx - g_grab_ox + L.cw / 2;
    int cy = my - g_grab_oy + L.ch / 2;
    int to = drop_target(cx, cy);
    pile_t *p = &g_pile[g_press_pile];
    int count = p->n - g_press_idx;

    if (to >= 0 && to != g_press_pile && can_drop(to, p->c[g_press_idx], count)) {
        do_move(g_press_pile, to, count);
        after_change();
    }

    g_drag_active = false;
    g_press_pile = -1;
}

static void on_key(unsigned char k)
{
    /* While help is up, any key just dismisses it. */
    if (g_help) { g_help = false; return; }

    switch (k) {
    case 'n': case 'N': new_deal(0); break;
    case 'r': case 'R': new_deal(g_seed_used); say("Deal replayed"); break;
    case 'u': case 'U': case 'z': case 'Z': do_undo(); break;
    case 'h': case 'H': do_hint(); break;
    case 'a': case 'A': toggle_auto(); break;
    case 'd': case 'D':
        g_draw_count = (g_draw_count == 3) ? 1 : 3;
        say(g_draw_count == 1 ? "Draw one" : "Draw three");
        break;
    case ' ':
        if (g_state == ST_PLAY) { do_draw(); after_change(); }
        else if (g_state == ST_WIN_ANIM) g_state = ST_WIN_PANEL;
        break;
    case '?': case '/': case KEY_F1: g_help = !g_help; break;
    default: break;
    }
}

/* ============================================================================
 * Entry point
 * ==========================================================================*/

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0 && fb.width > 0 && fb.height > 0) {
        sw = fb.width;
        sh = fb.height;
    }

    int win_w = WIN_W, win_h = WIN_H;
    if ((int)sw - 80 < win_w) win_w = (int)sw - 80;
    if ((int)sh - 80 < win_h) win_h = (int)sh - 80;
    if (win_w < 560) win_w = 560;
    if (win_h < 460) win_h = 460;

    if (uk_window_connect(&g_win, "Pasjans",
                          (int)(sw / 2) - win_w / 2,
                          (int)(sh / 2) - win_h / 2,
                          win_w, win_h, MAP_ADDR, SERVER_CHAN) < 0)
        return -1;

    new_deal(0);
    compute_layout();
    render();

    az_set_timer(g_win.client_chan, TICK_MS, 0);

    unsigned int prev_btn = 0;

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) break;

        if (msg.type == AZ_WM_WINDOW_RESIZED) {
            if (!uk_handle_resize(&g_win, &msg)) break;
            compute_layout();
            render();
            continue;
        }

        if (msg.type == AZ_WM_TIMER_TICK) {
            bool dirty = false;

            if (g_state == ST_WIN_ANIM) {
                if (!win_anim_step()) g_state = ST_WIN_PANEL;
                if (g_state == ST_WIN_PANEL) render();
                else                          uk_invalidate(&g_win);
                continue;
            }

            if (g_clock_run && g_state == ST_PLAY) {
                long now = (long)time(NULL) - g_started_at;
                if (now != g_elapsed) { g_elapsed = now; dirty = true; }
            }
            if (g_auto_running) {
                if (auto_step()) { after_change(); dirty = true; }
                else             { g_auto_running = false; dirty = true; }
                if (g_state != ST_PLAY) continue;
            }
            if (g_hint_ticks > 0 && --g_hint_ticks == 0) dirty = true;
            if (g_msg_ticks > 0 && --g_msg_ticks == 0) dirty = true;

            if (dirty) render();
            continue;
        }

        if (msg.type == AZ_WM_KEY_EVENT) {
            if (!msg.key.pressed) continue;
            unsigned char k = msg.key.keycode;
            if (k == 'q' || k == 'Q') break;
            if (k == 27) {
                if (g_help) { g_help = false; render(); continue; }
                break;
            }
            on_key(k);
            render();
            continue;
        }

        if (msg.type == AZ_WM_MOUSE_EVENT) {
            int mx = msg.mouse.abs_x;
            int my = msg.mouse.abs_y;
            unsigned int btn = msg.mouse.buttons;
            bool moved = (mx != g_mx || my != g_my);
            g_mx = mx; g_my = my;

            bool lpress   =  (btn & 1) && !(prev_btn & 1);
            bool lrelease = !(btn & 1) &&  (prev_btn & 1);
            bool rpress   =  (btn & 2) && !(prev_btn & 2);
            prev_btn = btn;

            bool dirty = false;

            int hov = -1;
            if (my < L.tool_h)
                for (int i = 0; i < BTN_COUNT; i++)
                    if (uk_hit_rect(g_btn_r[i], mx, my)) { hov = i; break; }
            if (hov != g_hover_btn) { g_hover_btn = hov; dirty = true; }

            if (lpress)      { on_press(mx, my, 1); dirty = true; }
            else if (rpress) { on_press(mx, my, 2); dirty = true; }
            else if (lrelease) { on_release(mx, my); dirty = true; }
            else if (moved && (btn & 1) && g_press_pile >= 0) {
                if (!g_drag_active) {
                    int ddx = mx - g_press_x, ddy = my - g_press_y;
                    if (ddx * ddx + ddy * ddy > 16) g_drag_active = true;
                }
                if (g_drag_active) dirty = true;
            }

            if (dirty) render();
        }
    }

    return 0;
}
