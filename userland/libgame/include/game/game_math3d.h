/* ============================================================================
 * AzamiOS Game Framework — 3D Math & Linear Algebra Library
 * File: userland/libgame/include/game/game_math3d.h
 *
 * Full-featured 3D linear algebra library:
 *   • vec3_t, vec4_t: vectors, dot, cross, length, normalize, reflection
 *   • mat4_t: 4x4 matrices, MVP transforms, perspective & ortho projections
 *   • quat_t: quaternions, Euler angle conversions, slerp interpolation
 *   • Hardware ISA acceleration: x86_64 SSE/AVX vectorization when supported
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "game_math.h"

#if defined(__x86_64__) && defined(__SSE__)
#include <xmmintrin.h>
#endif

#ifndef DEG2RAD
#define DEG2RAD(d) ((d) * 0.017453292519943295f)
#endif
#ifndef RAD2DEG
#define RAD2DEG(r) ((r) * 57.29577951308232f)
#endif

/* ── 3D Vector ───────────────────────────────────────────────────────────── */

typedef struct {
    float x, y, z;
} vec3_t;

#define VEC3_ZERO   ((vec3_t){ 0.0f, 0.0f, 0.0f })
#define VEC3_ONE    ((vec3_t){ 1.0f, 1.0f, 1.0f })
#define VEC3_UP     ((vec3_t){ 0.0f, 1.0f, 0.0f })
#define VEC3_DOWN   ((vec3_t){ 0.0f,-1.0f, 0.0f })
#define VEC3_LEFT   ((vec3_t){-1.0f, 0.0f, 0.0f })
#define VEC3_RIGHT  ((vec3_t){ 1.0f, 0.0f, 0.0f })
#define VEC3_FORWARD ((vec3_t){0.0f, 0.0f,-1.0f })
#define VEC3_BACK   ((vec3_t){ 0.0f, 0.0f, 1.0f })

static inline vec3_t vec3(float x, float y, float z)
{
    return (vec3_t){ x, y, z };
}

static inline vec3_t vec3_add(vec3_t a, vec3_t b)
{
    return (vec3_t){ a.x + b.x, a.y + b.y, a.z + b.z };
}

static inline vec3_t vec3_sub(vec3_t a, vec3_t b)
{
    return (vec3_t){ a.x - b.x, a.y - b.y, a.z - b.z };
}

static inline vec3_t vec3_scale(vec3_t v, float s)
{
    return (vec3_t){ v.x * s, v.y * s, v.z * s };
}

static inline vec3_t vec3_mul(vec3_t a, vec3_t b)
{
    return (vec3_t){ a.x * b.x, a.y * b.y, a.z * b.z };
}

static inline float vec3_dot(vec3_t a, vec3_t b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline vec3_t vec3_cross(vec3_t a, vec3_t b)
{
    return (vec3_t){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

static inline float vec3_length_sq(vec3_t v)
{
    return vec3_dot(v, v);
}

static inline float vec3_length(vec3_t v)
{
    return sqrtf(vec3_length_sq(v));
}

static inline vec3_t vec3_normalize(vec3_t v)
{
    float len = vec3_length(v);
    if (len > 1e-6f) {
        float inv = 1.0f / len;
        return vec3_scale(v, inv);
    }
    return VEC3_ZERO;
}

static inline float vec3_distance(vec3_t a, vec3_t b)
{
    return vec3_length(vec3_sub(a, b));
}

static inline vec3_t vec3_lerp(vec3_t a, vec3_t b, float t)
{
    return (vec3_t){
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t
    };
}

static inline vec3_t vec3_reflect(vec3_t v, vec3_t normal)
{
    float d = vec3_dot(v, normal);
    return vec3_sub(v, vec3_scale(normal, 2.0f * d));
}

/* ── 4D Vector ───────────────────────────────────────────────────────────── */

typedef struct {
    float x, y, z, w;
} vec4_t;

#define VEC4_ZERO ((vec4_t){ 0.0f, 0.0f, 0.0f, 0.0f })
#define VEC4_ONE  ((vec4_t){ 1.0f, 1.0f, 1.0f, 1.0f })

static inline vec4_t vec4(float x, float y, float z, float w)
{
    return (vec4_t){ x, y, z, w };
}

static inline vec4_t vec4_from_vec3(vec3_t v, float w)
{
    return (vec4_t){ v.x, v.y, v.z, w };
}

static inline vec4_t vec4_add(vec4_t a, vec4_t b)
{
    return (vec4_t){ a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w };
}

static inline vec4_t vec4_sub(vec4_t a, vec4_t b)
{
    return (vec4_t){ a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w };
}

static inline vec4_t vec4_scale(vec4_t v, float s)
{
    return (vec4_t){ v.x * s, v.y * s, v.z * s, v.w * s };
}

static inline float vec4_dot(vec4_t a, vec4_t b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

/* ── 4x4 Matrix (Row-major: m[row * 4 + col]) ────────────────────────────── */

typedef struct {
    float m[16];
} mat4_t;

static inline mat4_t mat4_zero(void)
{
    mat4_t out;
    for (int i = 0; i < 16; i++) out.m[i] = 0.0f;
    return out;
}

static inline mat4_t mat4_identity(void)
{
    mat4_t out = mat4_zero();
    out.m[0]  = 1.0f;
    out.m[5]  = 1.0f;
    out.m[10] = 1.0f;
    out.m[15] = 1.0f;
    return out;
}

static inline mat4_t mat4_translation(float tx, float ty, float tz)
{
    mat4_t out = mat4_identity();
    out.m[3]  = tx;
    out.m[7]  = ty;
    out.m[11] = tz;
    return out;
}

static inline mat4_t mat4_scale(float sx, float sy, float sz)
{
    mat4_t out = mat4_identity();
    out.m[0]  = sx;
    out.m[5]  = sy;
    out.m[10] = sz;
    return out;
}

static inline mat4_t mat4_rotation_x(float rad)
{
    mat4_t out = mat4_identity();
    float c = cosf(rad);
    float s = sinf(rad);
    out.m[5]  = c;  out.m[6]  = -s;
    out.m[9]  = s;  out.m[10] = c;
    return out;
}

static inline mat4_t mat4_rotation_y(float rad)
{
    mat4_t out = mat4_identity();
    float c = cosf(rad);
    float s = sinf(rad);
    out.m[0]  = c;  out.m[2]  = s;
    out.m[8]  = -s; out.m[10] = c;
    return out;
}

static inline mat4_t mat4_rotation_z(float rad)
{
    mat4_t out = mat4_identity();
    float c = cosf(rad);
    float s = sinf(rad);
    out.m[0] = c;  out.m[1] = -s;
    out.m[4] = s;  out.m[5] = c;
    return out;
}

static inline mat4_t mat4_rotation_axis(vec3_t axis, float rad)
{
    mat4_t out = mat4_identity();
    axis = vec3_normalize(axis);
    float c = cosf(rad);
    float s = sinf(rad);
    float t = 1.0f - c;

    out.m[0] = t * axis.x * axis.x + c;
    out.m[1] = t * axis.x * axis.y - s * axis.z;
    out.m[2] = t * axis.x * axis.z + s * axis.y;

    out.m[4] = t * axis.x * axis.y + s * axis.z;
    out.m[5] = t * axis.y * axis.y + c;
    out.m[6] = t * axis.y * axis.z - s * axis.x;

    out.m[8]  = t * axis.x * axis.z - s * axis.y;
    out.m[9]  = t * axis.y * axis.z + s * axis.x;
    out.m[10] = t * axis.z * axis.z + c;

    return out;
}

/* 4x4 Matrix Multiplication with optional x86 SSE SIMD acceleration */
static inline mat4_t mat4_mul(mat4_t a, mat4_t b)
{
    mat4_t out;
#if defined(__x86_64__) && defined(__SSE__)
    for (int i = 0; i < 4; i++) {
        __m128 sum = _mm_setzero_ps();
        for (int j = 0; j < 4; j++) {
            __m128 e = _mm_set1_ps(a.m[i * 4 + j]);
            __m128 brow = _mm_loadu_ps(&b.m[j * 4]);
            sum = _mm_add_ps(sum, _mm_mul_ps(e, brow));
        }
        _mm_storeu_ps(&out.m[i * 4], sum);
    }
#else
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out.m[r * 4 + c] = a.m[r * 4 + 0] * b.m[0 * 4 + c] +
                              a.m[r * 4 + 1] * b.m[1 * 4 + c] +
                              a.m[r * 4 + 2] * b.m[2 * 4 + c] +
                              a.m[r * 4 + 3] * b.m[3 * 4 + c];
        }
    }
#endif
    return out;
}

static inline vec4_t mat4_mul_vec4(mat4_t m, vec4_t v)
{
    return (vec4_t){
        m.m[0]  * v.x + m.m[1]  * v.y + m.m[2]  * v.z + m.m[3]  * v.w,
        m.m[4]  * v.x + m.m[5]  * v.y + m.m[6]  * v.z + m.m[7]  * v.w,
        m.m[8]  * v.x + m.m[9]  * v.y + m.m[10] * v.z + m.m[11] * v.w,
        m.m[12] * v.x + m.m[13] * v.y + m.m[14] * v.z + m.m[15] * v.w
    };
}

/* Transform 3D point (assumes w = 1.0f) and performs perspective divide */
static inline vec3_t mat4_mul_point(mat4_t m, vec3_t p)
{
    vec4_t res = mat4_mul_vec4(m, vec4_from_vec3(p, 1.0f));
    if (fabsf(res.w) > 1e-6f) {
        float inv = 1.0f / res.w;
        return (vec3_t){ res.x * inv, res.y * inv, res.z * inv };
    }
    return (vec3_t){ res.x, res.y, res.z };
}

/* Transform 3D direction vector (w = 0.0f, ignores translation) */
static inline vec3_t mat4_mul_dir(mat4_t m, vec3_t d)
{
    return (vec3_t){
        m.m[0] * d.x + m.m[1] * d.y + m.m[2] * d.z,
        m.m[4] * d.x + m.m[5] * d.y + m.m[6] * d.z,
        m.m[8] * d.x + m.m[9] * d.y + m.m[10] * d.z
    };
}

static inline mat4_t mat4_transpose(mat4_t m)
{
    mat4_t out;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out.m[r * 4 + c] = m.m[c * 4 + r];
        }
    }
    return out;
}

static inline mat4_t mat4_inverse(mat4_t m)
{
    mat4_t inv;
    float *s = m.m;
    float *d = inv.m;

    d[0] = s[5]  * s[10] * s[15] - s[5]  * s[11] * s[14] - s[9]  * s[6]  * s[15] +
           s[9]  * s[7]  * s[14] + s[13] * s[6]  * s[11] - s[13] * s[7]  * s[10];

    d[4] = -s[4]  * s[10] * s[15] + s[4]  * s[11] * s[14] + s[8]  * s[6]  * s[15] -
            s[8]  * s[7]  * s[14] - s[12] * s[6]  * s[11] + s[12] * s[7]  * s[10];

    d[8] = s[4]  * s[9] * s[15] - s[4]  * s[11] * s[13] - s[8]  * s[5] * s[15] +
           s[8]  * s[7] * s[13] + s[12] * s[5]  * s[11] - s[12] * s[7] * s[9];

    d[12] = -s[4]  * s[9] * s[14] + s[4]  * s[10] * s[13] + s[8]  * s[5] * s[14] -
             s[8]  * s[6] * s[13] - s[12] * s[5]  * s[10] + s[12] * s[6] * s[9];

    d[1] = -s[1]  * s[10] * s[15] + s[1]  * s[11] * s[14] + s[9]  * s[2] * s[15] -
            s[9]  * s[3]  * s[14] - s[13] * s[2]  * s[11] + s[13] * s[3] * s[10];

    d[5] = s[0]  * s[10] * s[15] - s[0]  * s[11] * s[14] - s[8]  * s[2] * s[15] +
           s[8]  * s[3]  * s[14] + s[12] * s[2]  * s[11] - s[12] * s[3] * s[10];

    d[9] = -s[0]  * s[9] * s[15] + s[0]  * s[11] * s[13] + s[8]  * s[1] * s[15] -
            s[8]  * s[3] * s[13] - s[12] * s[1]  * s[11] + s[12] * s[3] * s[9];

    d[13] = s[0]  * s[9] * s[14] - s[0]  * s[10] * s[13] - s[8]  * s[1] * s[14] +
            s[8]  * s[2] * s[13] + s[12] * s[1]  * s[10] - s[12] * s[2] * s[9];

    d[2] = s[1]  * s[6] * s[15] - s[1]  * s[7] * s[14] - s[5]  * s[2] * s[15] +
           s[5]  * s[3] * s[14] + s[13] * s[2] * s[7]  - s[13] * s[3] * s[6];

    d[6] = -s[0]  * s[6] * s[15] + s[0]  * s[7] * s[14] + s[4]  * s[2] * s[15] -
            s[4]  * s[3] * s[14] - s[12] * s[2] * s[7]  + s[12] * s[3] * s[6];

    d[10] = s[0]  * s[5] * s[15] - s[0]  * s[7] * s[13] - s[4]  * s[1] * s[15] +
            s[4]  * s[3] * s[13] + s[12] * s[1] * s[7]  - s[12] * s[3] * s[5];

    d[14] = -s[0]  * s[5] * s[14] + s[0]  * s[6] * s[13] + s[4]  * s[1] * s[14] -
             s[4]  * s[2] * s[13] - s[12] * s[1] * s[6]  + s[12] * s[2] * s[5];

    d[3] = -s[1] * s[6] * s[11] + s[1] * s[7] * s[10] + s[5] * s[2] * s[11] -
            s[5] * s[3] * s[10] - s[9] * s[2] * s[7]  + s[9] * s[3] * s[6];

    d[7] = s[0] * s[6] * s[11] - s[0] * s[7] * s[10] - s[4] * s[2] * s[11] +
           s[4] * s[3] * s[10] + s[8] * s[2] * s[7]  - s[8] * s[3] * s[6];

    d[11] = -s[0] * s[5] * s[11] + s[0] * s[7] * s[9] + s[4] * s[1] * s[11] -
             s[4] * s[3] * s[9]  - s[8] * s[1] * s[7] + s[8] * s[3] * s[5];

    d[15] = s[0] * s[5] * s[10] - s[0] * s[6] * s[9] - s[4] * s[1] * s[10] +
            s[4] * s[2] * s[9]  + s[8] * s[1] * s[6] - s[8] * s[2] * s[5];

    float det = s[0] * d[0] + s[1] * d[4] + s[2] * d[8] + s[3] * d[12];
    if (fabsf(det) < 1e-8f) return mat4_identity();

    float inv_det = 1.0f / det;
    for (int i = 0; i < 16; i++) d[i] *= inv_det;
    return inv;
}

/* Perspective Projection Matrix */
static inline mat4_t mat4_perspective(float fov_rad, float aspect, float near_z, float far_z)
{
    mat4_t out = mat4_zero();
    float tan_half_fov = tanf(fov_rad * 0.5f);

    out.m[0]  = 1.0f / (aspect * tan_half_fov);
    out.m[5]  = 1.0f / tan_half_fov;
    out.m[10] = -(far_z + near_z) / (far_z - near_z);
    out.m[11] = -(2.0f * far_z * near_z) / (far_z - near_z);
    out.m[14] = -1.0f;
    return out;
}

/* Orthographic Projection Matrix */
static inline mat4_t mat4_ortho(float left, float right, float bottom, float top, float near_z, float far_z)
{
    mat4_t out = mat4_identity();
    out.m[0]  = 2.0f / (right - left);
    out.m[5]  = 2.0f / (top - bottom);
    out.m[10] = -2.0f / (far_z - near_z);
    out.m[3]  = -(right + left) / (right - left);
    out.m[7]  = -(top + bottom) / (top - bottom);
    out.m[11] = -(far_z + near_z) / (far_z - near_z);
    return out;
}

/* Look-At View Matrix */
static inline mat4_t mat4_lookat(vec3_t eye, vec3_t target, vec3_t up)
{
    vec3_t f = vec3_normalize(vec3_sub(target, eye));
    vec3_t s = vec3_normalize(vec3_cross(f, up));
    vec3_t u = vec3_cross(s, f);

    mat4_t out = mat4_identity();
    out.m[0] = s.x;  out.m[1] = s.y;  out.m[2] = s.z;  out.m[3] = -vec3_dot(s, eye);
    out.m[4] = u.x;  out.m[5] = u.y;  out.m[6] = u.z;  out.m[7] = -vec3_dot(u, eye);
    out.m[8] =-f.x;  out.m[9] =-f.y;  out.m[10]=-f.z;  out.m[11]= vec3_dot(f, eye);
    return out;
}

/* ── Quaternion ──────────────────────────────────────────────────────────── */

typedef struct {
    float x, y, z, w;
} quat_t;

#define QUAT_IDENTITY ((quat_t){ 0.0f, 0.0f, 0.0f, 1.0f })

static inline quat_t quat(float x, float y, float z, float w)
{
    return (quat_t){ x, y, z, w };
}

static inline quat_t quat_from_axis_angle(vec3_t axis, float rad)
{
    float half = rad * 0.5f;
    float s = sinf(half);
    vec3_t norm = vec3_normalize(axis);
    return (quat_t){ norm.x * s, norm.y * s, norm.z * s, cosf(half) };
}

static inline quat_t quat_from_euler(float pitch, float yaw, float roll)
{
    float p = pitch * 0.5f, y = yaw * 0.5f, r = roll * 0.5f;
    float cp = cosf(p), sp = sinf(p);
    float cy = cosf(y), sy = sinf(y);
    float cr = cosf(r), sr = sinf(r);

    return (quat_t){
        sp * cy * cr - cp * sy * sr,
        cp * sy * cr + sp * cy * sr,
        cp * cy * sr - sp * sy * cr,
        cp * cy * cr + sp * sy * sr
    };
}

static inline quat_t quat_normalize(quat_t q)
{
    float len = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len > 1e-6f) {
        float inv = 1.0f / len;
        return (quat_t){ q.x * inv, q.y * inv, q.z * inv, q.w * inv };
    }
    return QUAT_IDENTITY;
}

static inline quat_t quat_mul(quat_t a, quat_t b)
{
    return (quat_t){
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
    };
}

static inline quat_t quat_slerp(quat_t a, quat_t b, float t)
{
    float cos_half = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (cos_half < 0.0f) {
        b = (quat_t){ -b.x, -b.y, -b.z, -b.w };
        cos_half = -cos_half;
    }

    if (cos_half >= 0.999f) {
        /* Linearly interpolate if angle is tiny */
        return quat_normalize((quat_t){
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t
        });
    }

    float half_theta = acosf(cos_half);
    float sin_half = sqrtf(1.0f - cos_half * cos_half);
    float rA = sinf((1.0f - t) * half_theta) / sin_half;
    float rB = sinf(t * half_theta) / sin_half;

    return (quat_t){
        a.x * rA + b.x * rB,
        a.y * rA + b.y * rB,
        a.z * rA + b.z * rB,
        a.w * rA + b.w * rB
    };
}

static inline mat4_t quat_to_mat4(quat_t q)
{
    mat4_t out = mat4_identity();
    q = quat_normalize(q);

    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    out.m[0] = 1.0f - 2.0f * (yy + zz);
    out.m[1] = 2.0f * (xy - wz);
    out.m[2] = 2.0f * (xz + wy);

    out.m[4] = 2.0f * (xy + wz);
    out.m[5] = 1.0f - 2.0f * (xx + zz);
    out.m[6] = 2.0f * (yz - wx);

    out.m[8] = 2.0f * (xz - wy);
    out.m[9] = 2.0f * (yz + wx);
    out.m[10]= 1.0f - 2.0f * (xx + yy);

    return out;
}
