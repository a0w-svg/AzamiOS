/* ============================================================================
 * AzamiOS Game Framework — Physics Implementation
 * File: userland/libgame/game_physics.c
 * ============================================================================ */

#include "include/game/game_physics.h"
#include <string.h>

/* ── Initialization ───────────────────────────────────────────────────────── */

void phys_init(phys_world_t *pw, float gravity_x, float gravity_y)
{
    memset(pw, 0, sizeof(*pw));
    pw->gravity_x = gravity_x;
    pw->gravity_y = gravity_y;
}

void phys_set_collision_callback(phys_world_t *pw, phys_collision_fn fn, void *userdata)
{
    pw->on_collision = fn;
    pw->on_collision_data = userdata;
}

void phys_set_trigger_callback(phys_world_t *pw, phys_collision_fn fn, void *userdata)
{
    pw->on_trigger = fn;
    pw->on_trigger_data = userdata;
}

/* ── Spatial Hash Helpers ─────────────────────────────────────────────────── */

static unsigned int phys_hash_key(int cx, int cy)
{
    unsigned int h = (unsigned int)(cx * 73856093u) ^ (unsigned int)(cy * 19349663u);
    return h % PHYS_HASH_SIZE;
}

static void phys_hash_clear(phys_world_t *pw)
{
    for (int i = 0; i < PHYS_HASH_SIZE; i++)
        pw->hash[i].count = 0;
}

static void phys_hash_insert(phys_world_t *pw, ecs_entity_t e, int cx, int cy)
{
    unsigned int key = phys_hash_key(cx, cy);
    phys_bucket_t *b = &pw->hash[key];
    if (b->count < PHYS_BUCKET_CAP) {
        /* Avoid duplicates */
        for (int i = 0; i < b->count; i++) {
            if (b->entities[i] == e) return;
        }
        b->entities[b->count++] = e;
    }
}

/* ── Collision Detection Helpers ──────────────────────────────────────────── */

static void get_world_aabb(const comp_transform_t *t, const comp_collider_t *c, aabb_t *out)
{
    if (c->type == COLLIDER_AABB) {
        out->x = t->position.x + c->box.off_x - c->box.w * 0.5f;
        out->y = t->position.y + c->box.off_y - c->box.h * 0.5f;
        out->w = c->box.w;
        out->h = c->box.h;
    } else {
        float r = c->circle.r;
        out->x = t->position.x + c->circle.off_x - r;
        out->y = t->position.y + c->circle.off_y - r;
        out->w = r * 2.0f;
        out->h = r * 2.0f;
    }
}

static bool test_collision(const comp_transform_t *ta, const comp_collider_t *ca,
                           const comp_transform_t *tb, const comp_collider_t *cb,
                           vec2_t *normal, float *depth)
{
    if (ca->type == COLLIDER_AABB && cb->type == COLLIDER_AABB) {
        /* AABB vs AABB */
        aabb_t a, b;
        get_world_aabb(ta, ca, &a);
        get_world_aabb(tb, cb, &b);

        if (!aabb_overlaps(a, b)) return false;

        vec2_t mtv = aabb_mtv(a, b);
        float len_sq = vec2_length_sq(mtv);
        if (len_sq < 0.00001f) return false;

        float len = vec2_length(mtv);
        *normal = vec2_scale(mtv, 1.0f / len);
        *depth = len;
        return true;
    }

    if (ca->type == COLLIDER_CIRCLE && cb->type == COLLIDER_CIRCLE) {
        /* Circle vs Circle */
        float ax = ta->position.x + ca->circle.off_x;
        float ay = ta->position.y + ca->circle.off_y;
        float bx = tb->position.x + cb->circle.off_x;
        float by = tb->position.y + cb->circle.off_y;

        float dx = bx - ax;
        float dy = by - ay;
        float dist_sq = dx * dx + dy * dy;
        float rsum = ca->circle.r + cb->circle.r;

        if (dist_sq >= rsum * rsum) return false;

        float dist = dist_sq * game_inv_sqrt(dist_sq); /* approximate sqrt */
        if (dist < 0.001f) {
            *normal = vec2(1.0f, 0.0f);
            *depth = rsum;
        } else {
            *normal = vec2(dx / dist, dy / dist);
            *depth = rsum - dist;
        }
        return true;
    }

    /* AABB vs Circle (or vice versa) */
    const comp_transform_t *box_t, *circ_t;
    const comp_collider_t *box_c, *circ_c;
    float sign = 1.0f;

    if (ca->type == COLLIDER_AABB) {
        box_t = ta; box_c = ca;
        circ_t = tb; circ_c = cb;
    } else {
        box_t = tb; box_c = cb;
        circ_t = ta; circ_c = ca;
        sign = -1.0f;
    }

    aabb_t box;
    get_world_aabb(box_t, box_c, &box);

    float cx = circ_t->position.x + circ_c->circle.off_x;
    float cy = circ_t->position.y + circ_c->circle.off_y;
    float cr = circ_c->circle.r;

    /* Find closest point on AABB to circle center */
    float nearest_x = game_clampf(cx, box.x, box.x + box.w);
    float nearest_y = game_clampf(cy, box.y, box.y + box.h);

    float dx = cx - nearest_x;
    float dy = cy - nearest_y;
    float dist_sq = dx * dx + dy * dy;

    if (dist_sq >= cr * cr) return false;

    if (dist_sq < 0.001f) {
        /* Circle center is inside box — find shortest push-out axis */
        float push_left  = cx - box.x;
        float push_right = (box.x + box.w) - cx;
        float push_up    = cy - box.y;
        float push_down  = (box.y + box.h) - cy;

        float min_push = push_left;
        *normal = vec2(-1.0f * sign, 0.0f);
        if (push_right < min_push) { min_push = push_right; *normal = vec2(1.0f * sign, 0.0f); }
        if (push_up < min_push)    { min_push = push_up;    *normal = vec2(0.0f, -1.0f * sign); }
        if (push_down < min_push)  { min_push = push_down;  *normal = vec2(0.0f, 1.0f * sign); }
        *depth = min_push + cr;
    } else {
        float dist = dist_sq * game_inv_sqrt(dist_sq);
        *normal = vec2(dx / dist * sign, dy / dist * sign);
        *depth = cr - dist;
    }
    return true;
}

/* ── Add collision pair ───────────────────────────────────────────────────── */

static void add_pair(phys_world_t *pw, ecs_entity_t a, ecs_entity_t b,
                     vec2_t normal, float depth)
{
    if (pw->pair_count >= PHYS_MAX_PAIRS) return;

    /* Avoid duplicates */
    for (int i = 0; i < pw->pair_count; i++) {
        if ((pw->pairs[i].a == a && pw->pairs[i].b == b) ||
            (pw->pairs[i].a == b && pw->pairs[i].b == a))
            return;
    }

    phys_collision_t *p = &pw->pairs[pw->pair_count++];
    p->a = a;
    p->b = b;
    p->normal = normal;
    p->depth = depth;
}

/* ── Physics Step ─────────────────────────────────────────────────────────── */

void phys_step(phys_world_t *pw, ecs_world_t *ew, float dt)
{
    ecs_comp_mask_t phys_mask = ECS_COMP_BIT(COMP_TRANSFORM) |
                                ECS_COMP_BIT(COMP_RIGIDBODY);
    ecs_comp_mask_t coll_mask = phys_mask | ECS_COMP_BIT(COMP_COLLIDER);

    /* ── 1. Integrate velocities ──────────────────────────────────────────── */
    for (uint16_t i = 0; i < ECS_MAX_ENTITIES; i++) {
        if (!ew->alive[i]) continue;
        if ((ew->masks[i] & phys_mask) != phys_mask) continue;

        ecs_entity_t e = ecs_entity_make(i, ew->generation[i]);
        comp_transform_t *t = ecs_transform(ew, e);
        comp_rigidbody_t *rb = ecs_rigidbody(ew, e);
        if (!t || !rb) continue;

        /* Skip static bodies */
        if (rb->mass <= 0.0f && !rb->is_kinematic) continue;

        /* Apply gravity */
        if (rb->mass > 0.0f && !rb->is_kinematic) {
            rb->velocity.x += (rb->acceleration.x + pw->gravity_x) * dt;
            rb->velocity.y += (rb->acceleration.y + pw->gravity_y) * dt;
        } else {
            rb->velocity.x += rb->acceleration.x * dt;
            rb->velocity.y += rb->acceleration.y * dt;
        }

        /* Apply drag */
        if (rb->drag > 0.0f) {
            float d = 1.0f - rb->drag * dt;
            if (d < 0) d = 0;
            rb->velocity.x *= d;
            rb->velocity.y *= d;
        }

        /* Integrate position */
        t->position.x += rb->velocity.x * dt;
        t->position.y += rb->velocity.y * dt;
    }

    /* ── 2. Broadphase: populate spatial hash ─────────────────────────────── */
    phys_hash_clear(pw);

    for (uint16_t i = 0; i < ECS_MAX_ENTITIES; i++) {
        if (!ew->alive[i]) continue;
        if ((ew->masks[i] & coll_mask) != coll_mask) continue;

        ecs_entity_t e = ecs_entity_make(i, ew->generation[i]);
        comp_transform_t *t = ecs_transform(ew, e);
        comp_collider_t *c = ecs_collider(ew, e);
        if (!t || !c) continue;

        aabb_t bounds;
        get_world_aabb(t, c, &bounds);

        int min_cx = (int)(bounds.x) / PHYS_HASH_CELL_SIZE;
        int max_cx = (int)(bounds.x + bounds.w) / PHYS_HASH_CELL_SIZE;
        int min_cy = (int)(bounds.y) / PHYS_HASH_CELL_SIZE;
        int max_cy = (int)(bounds.y + bounds.h) / PHYS_HASH_CELL_SIZE;

        for (int cy = min_cy; cy <= max_cy; cy++) {
            for (int cx = min_cx; cx <= max_cx; cx++) {
                phys_hash_insert(pw, e, cx, cy);
            }
        }
    }

    /* ── 3. Save previous pairs and clear current ─────────────────────────── */
    memcpy(pw->prev_pairs, pw->pairs, sizeof(phys_collision_t) * (size_t)pw->pair_count);
    pw->prev_pair_count = pw->pair_count;
    pw->pair_count = 0;

    /* ── 4. Narrowphase: test pairs from spatial hash ─────────────────────── */
    for (int b_idx = 0; b_idx < PHYS_HASH_SIZE; b_idx++) {
        phys_bucket_t *bucket = &pw->hash[b_idx];
        for (int i = 0; i < bucket->count; i++) {
            for (int j = i + 1; j < bucket->count; j++) {
                ecs_entity_t ea = bucket->entities[i];
                ecs_entity_t eb = bucket->entities[j];

                comp_transform_t *ta = ecs_transform(ew, ea);
                comp_collider_t  *ca = ecs_collider(ew, ea);
                comp_transform_t *tb = ecs_transform(ew, eb);
                comp_collider_t  *cb = ecs_collider(ew, eb);

                if (!ta || !ca || !tb || !cb) continue;

                /* Check layer mask compatibility */
                if (!(ca->layer & cb->mask) && !(cb->layer & ca->mask)) continue;

                vec2_t normal;
                float depth;
                if (test_collision(ta, ca, tb, cb, &normal, &depth)) {
                    add_pair(pw, ea, eb, normal, depth);
                }
            }
        }
    }

    /* ── 5. Collision response ─────────────────────────────────────────────── */
    for (int p = 0; p < pw->pair_count; p++) {
        phys_collision_t *col = &pw->pairs[p];

        comp_collider_t *ca = ecs_collider(ew, col->a);
        comp_collider_t *cb = ecs_collider(ew, col->b);

        /* Trigger collision: generate callback but no physical response */
        if ((ca && ca->is_trigger) || (cb && cb->is_trigger)) {
            if (pw->on_trigger)
                pw->on_trigger(col, pw->on_trigger_data);
            continue;
        }

        comp_transform_t *ta = ecs_transform(ew, col->a);
        comp_rigidbody_t *ra = ecs_rigidbody(ew, col->a);
        comp_transform_t *tb = ecs_transform(ew, col->b);
        comp_rigidbody_t *rb = ecs_rigidbody(ew, col->b);

        if (!ta || !tb) continue;

        bool a_static = !ra || ra->mass <= 0.0f;
        bool b_static = !rb || rb->mass <= 0.0f;

        /* If both are static, skip */
        if (a_static && b_static) continue;

        /* Separation */
        if (a_static) {
            tb->position.x += col->normal.x * col->depth;
            tb->position.y += col->normal.y * col->depth;
        } else if (b_static) {
            ta->position.x -= col->normal.x * col->depth;
            ta->position.y -= col->normal.y * col->depth;
        } else {
            float total_mass = ra->mass + rb->mass;
            float ratio_a = rb->mass / total_mass;
            float ratio_b = ra->mass / total_mass;
            ta->position.x -= col->normal.x * col->depth * ratio_a;
            ta->position.y -= col->normal.y * col->depth * ratio_a;
            tb->position.x += col->normal.x * col->depth * ratio_b;
            tb->position.y += col->normal.y * col->depth * ratio_b;
        }

        /* Velocity response (elastic/inelastic bounce) */
        if (ra && rb && !a_static && !b_static) {
            vec2_t rel_vel = vec2_sub(rb->velocity, ra->velocity);
            float vel_along_normal = vec2_dot(rel_vel, col->normal);

            /* Don't resolve if separating */
            if (vel_along_normal > 0) continue;

            float e = (ra->restitution + rb->restitution) * 0.5f;
            float j = -(1.0f + e) * vel_along_normal;
            j /= (1.0f / ra->mass + 1.0f / rb->mass);

            vec2_t impulse = vec2_scale(col->normal, j);
            ra->velocity.x -= impulse.x / ra->mass;
            ra->velocity.y -= impulse.y / ra->mass;
            rb->velocity.x += impulse.x / rb->mass;
            rb->velocity.y += impulse.y / rb->mass;

            /* Apply friction */
            float friction = (ra->friction + rb->friction) * 0.5f;
            if (friction > 0) {
                vec2_t tangent = { -col->normal.y, col->normal.x };
                float vel_along_tangent = vec2_dot(rel_vel, tangent);
                float jt = -vel_along_tangent;
                jt /= (1.0f / ra->mass + 1.0f / rb->mass);

                /* Clamp friction impulse to Coulomb's law */
                if (jt > j * friction) jt = j * friction;
                if (jt < -j * friction) jt = -j * friction;

                vec2_t friction_impulse = vec2_scale(tangent, jt);
                ra->velocity.x -= friction_impulse.x / ra->mass;
                ra->velocity.y -= friction_impulse.y / ra->mass;
                rb->velocity.x += friction_impulse.x / rb->mass;
                rb->velocity.y += friction_impulse.y / rb->mass;
            }
        } else if (ra && !a_static && b_static) {
            /* Bounce off static body */
            float vel_along_normal = vec2_dot(ra->velocity, col->normal);
            if (vel_along_normal < 0) {
                float e = ra->restitution;
                if (cb && rb) e = (e + rb->restitution) * 0.5f;
                ra->velocity.x -= (1.0f + e) * vel_along_normal * col->normal.x;
                ra->velocity.y -= (1.0f + e) * vel_along_normal * col->normal.y;
            }
        } else if (rb && !b_static && a_static) {
            float vel_along_normal = vec2_dot(rb->velocity, col->normal);
            if (vel_along_normal > 0) {
                float e = rb->restitution;
                if (ca && ra) e = (e + ra->restitution) * 0.5f;
                rb->velocity.x -= (1.0f + e) * vel_along_normal * col->normal.x;
                rb->velocity.y -= (1.0f + e) * vel_along_normal * col->normal.y;
            }
        }

        /* Fire collision callback */
        if (pw->on_collision)
            pw->on_collision(col, pw->on_collision_data);
    }
}
