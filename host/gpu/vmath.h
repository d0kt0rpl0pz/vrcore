/*
 * vmath.h -- the little linear algebra the host needs. Column-major 4x4 (m[col * 4 + row]),
 * right-handed view space looking down -z, Vulkan clip space (y down, depth 0..1).
 */
#pragma once

#include <cmath>
#include <cstring>

namespace vrcore {

struct Quat { float x = 0, y = 0, z = 0, w = 1; };
struct Mat4 { float m[16]; };

inline Mat4 mat4_identity()
{
    Mat4 r;
    std::memset(r.m, 0, sizeof r.m);
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f;
    return r;
}

inline Mat4 mat4_mul(const Mat4 &a, const Mat4 &b)   // a * b
{
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

inline Mat4 mat4_translate(float x, float y, float z)
{
    Mat4 r = mat4_identity();
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}

inline Mat4 mat4_scale(float x, float y, float z)
{
    Mat4 r = mat4_identity();
    r.m[0] = x; r.m[5] = y; r.m[10] = z;
    return r;
}

inline Mat4 mat4_rotate(const Quat &q)               // q is unit length
{
    Mat4 r = mat4_identity();
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    r.m[0] = 1 - 2 * (yy + zz); r.m[1] = 2 * (xy + wz);     r.m[2] = 2 * (xz - wy);
    r.m[4] = 2 * (xy - wz);     r.m[5] = 1 - 2 * (xx + zz); r.m[6] = 2 * (yz + wx);
    r.m[8] = 2 * (xz + wy);     r.m[9] = 2 * (yz - wx);     r.m[10] = 1 - 2 * (xx + yy);
    return r;
}

inline Quat quat_mul(const Quat &a, const Quat &b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline Quat quat_axis_angle(float x, float y, float z, float rad)
{
    float s = std::sin(rad * 0.5f);
    return {x * s, y * s, z * s, std::cos(rad * 0.5f)};
}

// The 3x3 part transposed (a rotation's inverse), no translation.
inline Mat4 mat4_transpose3(const Mat4 &a)
{
    Mat4 r = mat4_identity();
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row) r.m[c * 4 + row] = a.m[row * 4 + c];
    return r;
}

// The view matrix (world to eye) of an eye at `pos` with orientation `q`: the inverse of
// T(pos) * R(q), without a general inverse.
inline Mat4 mat4_view_from_pose(const Quat &q, float px, float py, float pz)
{
    Mat4 r = mat4_rotate(q);
    // transpose the rotation, then t' = -R^T * pos
    Mat4 v = mat4_identity();
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row) v.m[c * 4 + row] = r.m[row * 4 + c];
    v.m[12] = -(v.m[0] * px + v.m[4] * py + v.m[8] * pz);
    v.m[13] = -(v.m[1] * px + v.m[5] * py + v.m[9] * pz);
    v.m[14] = -(v.m[2] * px + v.m[6] * py + v.m[10] * pz);
    return v;
}

// Asymmetric perspective from the four half-angles (radians; left and down negative, as
// OpenXR's XrFovf gives them), reverse-Z: depth 1 at the near plane, 0 at the far plane.
inline Mat4 mat4_projection_reverse_z(float angle_left, float angle_right, float angle_up, float angle_down,
                                      float znear, float zfar)
{
    float tl = std::tan(angle_left), tr = std::tan(angle_right);
    float tu = std::tan(angle_up), td = std::tan(angle_down);
    Mat4 r;
    std::memset(r.m, 0, sizeof r.m);
    r.m[0] = 2.f / (tr - tl);
    r.m[5] = 2.f / (td - tu);                       // y down
    r.m[8] = (tr + tl) / (tr - tl);
    r.m[9] = (tu + td) / (td - tu);
    r.m[10] = znear / (zfar - znear);
    r.m[11] = -1.f;
    r.m[14] = znear * zfar / (zfar - znear);
    return r;
}

}
