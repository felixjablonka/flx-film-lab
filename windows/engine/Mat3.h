#pragma once

#include <algorithm>
#include <cmath>

// The float vectors and 3x3 matrices of the Mac engine's CPU side (simd's
// SIMD3<Float> and simd_float3x3): column-major, float throughout, each
// operation in the order simd's own takes it.

struct float2 {
    float x = 0, y = 0;
    float &operator[](int i) { return i == 0 ? x : y; }
    float operator[](int i) const { return i == 0 ? x : y; }
};

struct float3 {
    float x = 0, y = 0, z = 0;
    float &operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
    float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    static float3 all(float v) { return {v, v, v}; }
};

inline float3 operator+(float3 a, float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline float3 operator-(float3 a, float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float3 operator*(float3 a, float3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float3 operator/(float3 a, float3 b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
inline float3 operator*(float3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float3 operator*(float s, float3 a) { return {s * a.x, s * a.y, s * a.z}; }
inline float3 operator/(float3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }
inline float3 &operator+=(float3 &a, float3 b) { return a = a + b; }
inline float3 &operator*=(float3 &a, float3 b) { return a = a * b; }
inline float3 &operator*=(float3 &a, float s) { return a = a * s; }
inline float3 &operator/=(float3 &a, float s) { return a = a / s; }
inline float3 max(float3 a, float3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
inline float length(float2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

struct Mat3 {
    float3 c[3];                    // columns

    static Mat3 identity() { return diagonal({1, 1, 1}); }
    static Mat3 diagonal(float3 d) { return {{{d.x, 0, 0}, {0, d.y, 0}, {0, 0, d.z}}}; }
    static Mat3 columns(float3 a, float3 b, float3 d) { return {{a, b, d}}; }
    static Mat3 rows(float3 a, float3 b, float3 d) { return {{{a.x, b.x, d.x}, {a.y, b.y, d.y}, {a.z, b.z, d.z}}}; }
    // A 3x3 matrix written row-major, as the packs store them.
    static Mat3 rowMajor(const float *m) { return rows({m[0], m[1], m[2]}, {m[3], m[4], m[5]}, {m[6], m[7], m[8]}); }

    float &at(int row, int column) { return c[column][row]; }
    float at(int row, int column) const { return c[column][row]; }

    float3 operator*(float3 v) const { return c[0] * v.x + c[1] * v.y + c[2] * v.z; }
    Mat3 operator*(const Mat3 &b) const { return {{*this * b.c[0], *this * b.c[1], *this * b.c[2]}}; }

    float determinant() const
    {
        const float3 &a = c[0], &b = c[1], &d = c[2];
        return a.x * (b.y * d.z - b.z * d.y) - b.x * (a.y * d.z - a.z * d.y) + d.x * (a.y * b.z - a.z * b.y);
    }

    Mat3 inverse() const
    {
        const float3 &a = c[0], &b = c[1], &d = c[2];
        // the adjugate's columns: cross products of the columns' pairs, as rows
        const float3 r0{b.y * d.z - b.z * d.y, b.z * d.x - b.x * d.z, b.x * d.y - b.y * d.x};
        const float3 r1{d.y * a.z - d.z * a.y, d.z * a.x - d.x * a.z, d.x * a.y - d.y * a.x};
        const float3 r2{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
        const float det = a.x * r0.x + a.y * r0.y + a.z * r0.z;
        return rows(r0 / det, r1 / det, r2 / det);
    }
};
