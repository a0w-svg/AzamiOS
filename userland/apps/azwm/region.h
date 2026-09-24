/* ============================================================================
 * AzamiOS — Display Server Damage Regions
 * File: userland/apps/azwm/region.h
 *
 * A damage region is a small, bounded set of screen rectangles. It exists
 * because a single bounding box is a bad model for what actually changes on
 * a desktop: the taskbar clock ticks in one corner while a caret blinks in
 * the other, and their bounding box is the whole screen. Every frame then
 * recomposites 4 MiB and, on a host-backed scanout, ships 4 MiB across the
 * virtqueue — to repaint two rectangles totalling a few KiB.
 *
 * The region keeps those two rectangles apart. It is deliberately bounded
 * (AZWM_REGION_MAX): an unbounded list would turn the per-rect overhead of
 * compose/copy/transfer into its own cost, and the whole point is to do less
 * work, not more bookkeeping. When the list is full the two rectangles whose
 * union wastes the least area are merged, so the region degrades gracefully
 * towards the bounding box it replaced rather than failing.
 *
 * The invariant callers rely on: the union of the rectangles always covers
 * every pixel ever added. Coalescing may make the region cover *more* than
 * was added (that only costs redundant repaint), never less (that would
 * leave stale pixels on screen).
 *
 * Header-only so it can be unit tested on the host without linking the
 * compositor. Rectangles are half-open: x1/y1 are exclusive.
 * ============================================================================ */
#pragma once

/* A screen-space rectangle; x1/y1 are exclusive. `valid` is 0 for "empty",
 * which is not the same as a zero-sized rect at the origin. Defined here
 * rather than in compositor.h so the region algebra below can be compiled
 * and tested on its own. */
typedef struct {
    int x0, y0, x1, y1;
    int valid;
} azwm_rect_t;

/* Eight is enough for the cases that matter — a pointer trail, a couple of
 * animating windows, a clock and a caret — while keeping the per-frame
 * loops short. Beyond that, merging is cheaper than tracking. */
#define AZWM_REGION_MAX 8

typedef struct {
    azwm_rect_t r[AZWM_REGION_MAX];
    int         count;
} azwm_region_t;

static inline void region_clear(azwm_region_t *rg)
{
    rg->count = 0;
}

static inline int region_empty(const azwm_region_t *rg)
{
    return rg->count == 0;
}

static inline long region_rect_area(const azwm_rect_t *a)
{
    if (a->x1 <= a->x0 || a->y1 <= a->y0) return 0;
    return (long)(a->x1 - a->x0) * (long)(a->y1 - a->y0);
}

static inline void region_rect_merge(azwm_rect_t *dst, const azwm_rect_t *src)
{
    if (src->x0 < dst->x0) dst->x0 = src->x0;
    if (src->y0 < dst->y0) dst->y0 = src->y0;
    if (src->x1 > dst->x1) dst->x1 = src->x1;
    if (src->y1 > dst->y1) dst->y1 = src->y1;
}

static inline long region_merged_area(const azwm_rect_t *a, const azwm_rect_t *b)
{
    azwm_rect_t m = *a;
    region_rect_merge(&m, b);
    return region_rect_area(&m);
}

static inline int region_rect_contains(const azwm_rect_t *outer, const azwm_rect_t *inner)
{
    return inner->x0 >= outer->x0 && inner->y0 >= outer->y0 &&
           inner->x1 <= outer->x1 && inner->y1 <= outer->y1;
}

static inline int region_rect_overlaps(const azwm_rect_t *a, const azwm_rect_t *b)
{
    return a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1;
}

/*
 * Merge the cheapest pair in the list, freeing one slot. "Cheapest" is the
 * pair whose union adds the least area that neither rectangle covered — the
 * usual region-merge heuristic, and the reason a clock and a caret at
 * opposite corners stay separate while two rectangles on the same window
 * collapse into one.
 */
static inline void region_merge_cheapest_pair(azwm_region_t *rg)
{
    if (rg->count < 2) return;

    int bi = 0, bj = 1;
    long best = -1;
    for (int i = 0; i < rg->count; i++) {
        for (int j = i + 1; j < rg->count; j++) {
            long waste = region_merged_area(&rg->r[i], &rg->r[j])
                       - region_rect_area(&rg->r[i])
                       - region_rect_area(&rg->r[j]);
            if (waste < 0) waste = 0;     /* overlapping pair: free to merge */
            if (best < 0 || waste < best) {
                best = waste;
                bi = i; bj = j;
            }
        }
    }

    region_rect_merge(&rg->r[bi], &rg->r[bj]);
    rg->r[bj] = rg->r[rg->count - 1];
    rg->count--;
}

/*
 * Add one rectangle.
 *
 * A rectangle already covered by the region is dropped. One that overlaps or
 * nearly touches an existing rectangle is merged into it, and the merge
 * cascades: absorbing a rectangle can bring the grown rectangle within reach
 * of another, and leaving those unmerged is how a region slowly fills with
 * near-duplicates. Anything else takes its own slot, and if there is none,
 * the cheapest pair in the list makes room.
 */
static inline void region_add(azwm_region_t *rg, int x0, int y0, int x1, int y1)
{
    if (x1 <= x0 || y1 <= y0) return;

    azwm_rect_t nr = { x0, y0, x1, y1, 1 };

    for (;;) {
        int merged_into = -1;
        for (int i = 0; i < rg->count; i++) {
            if (region_rect_contains(&rg->r[i], &nr)) return;

            long waste = region_merged_area(&rg->r[i], &nr)
                       - region_rect_area(&rg->r[i])
                       - region_rect_area(&nr);
            /* Overlapping rectangles double-count their intersection, so
             * `waste` goes negative — those are strictly worth merging. So
             * is a merge that adds no more than a quarter again on top of
             * what the two rectangles already cost to repaint. */
            if (waste <= 0 ||
                waste * 4 <= region_rect_area(&rg->r[i]) + region_rect_area(&nr)) {
                merged_into = i;
                break;
            }
        }

        if (merged_into < 0) break;

        /* Absorb the slot and retry with the grown rectangle, so one add can
         * collapse a whole run of adjacent rectangles. */
        region_rect_merge(&nr, &rg->r[merged_into]);
        rg->r[merged_into] = rg->r[rg->count - 1];
        rg->count--;
    }

    if (rg->count >= AZWM_REGION_MAX)
        region_merge_cheapest_pair(rg);

    rg->r[rg->count++] = nr;
}

static inline void region_add_rect(azwm_region_t *rg, const azwm_rect_t *a)
{
    if (!a->valid) return;
    region_add(rg, a->x0, a->y0, a->x1, a->y1);
}

static inline void region_union(azwm_region_t *dst, const azwm_region_t *src)
{
    for (int i = 0; i < src->count; i++)
        region_add(dst, src->r[i].x0, src->r[i].y0, src->r[i].x1, src->r[i].y1);
}

/* Replace the region with a single rectangle. */
static inline void region_set(azwm_region_t *rg, int x0, int y0, int x1, int y1)
{
    rg->count = 0;
    region_add(rg, x0, y0, x1, y1);
}

/* The bounding box of everything in the region; `valid` is 0 when empty. */
static inline void region_bounds(const azwm_region_t *rg, azwm_rect_t *out)
{
    if (rg->count == 0) {
        out->x0 = out->y0 = out->x1 = out->y1 = 0;
        out->valid = 0;
        return;
    }
    *out = rg->r[0];
    out->valid = 1;
    for (int i = 1; i < rg->count; i++)
        region_rect_merge(out, &rg->r[i]);
}

static inline long region_area(const azwm_region_t *rg)
{
    long a = 0;
    for (int i = 0; i < rg->count; i++)
        a += region_rect_area(&rg->r[i]);
    return a;
}

/* Clamp every rectangle to [0,w) x [0,h), dropping any that fall outside. */
static inline void region_clip(azwm_region_t *rg, int w, int h)
{
    int out = 0;
    for (int i = 0; i < rg->count; i++) {
        azwm_rect_t c = rg->r[i];
        if (c.x0 < 0) c.x0 = 0;
        if (c.y0 < 0) c.y0 = 0;
        if (c.x1 > w) c.x1 = w;
        if (c.y1 > h) c.y1 = h;
        if (c.x1 <= c.x0 || c.y1 <= c.y0) continue;
        c.valid = 1;
        rg->r[out++] = c;
    }
    rg->count = out;
}
