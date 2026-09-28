#pragma once
// The engine's math conversions (math/mQuat.cc, mMath_C.cc): MatrixF is
// row-major with the translation in column 3.
#include <array>
#include <cmath>

namespace TorqueMath {

using Matrix = std::array<float, 16>;
struct Quat { float x = 0, y = 0, z = 0, w = 1; };
struct AngAxis { float x = 1, y = 0, z = 0, angle = 0; };

// QuatF::set(AngAxisF).
inline Quat quat(const AngAxis& a) {
    const float s = std::sin(a.angle * 0.5f), c = std::cos(a.angle * 0.5f);
    return {a.x * s, a.y * s, a.z * s, c};
}

// QuatF::set(EulerF): Qyaw * Qpitch * Qroll (ZXY), angles negated.
inline Quat quat(float ex, float ey, float ez) {
    const float sx = std::sin(-ex * 0.5f), cx = std::cos(-ex * 0.5f);
    const float sy = std::sin(-ey * 0.5f), cy = std::cos(-ey * 0.5f);
    const float sz = std::sin(-ez * 0.5f), cz = std::cos(-ez * 0.5f);
    const float cycz = cy * cz, sysz = sy * sz, sycz = sy * cz, cysz = cy * sz;
    return {cycz * sx + sysz * cx, sycz * cx - cysz * sx, cysz * cx - sycz * sx, cycz * cx + sysz * sx};
}

// QuatF::setMatrix (m_quatF_set_matF), translation zero.
inline Matrix matrix(const Quat& q) {
    Matrix m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (q.x * q.x + q.y * q.y + q.z * q.z < 10E-20f) return m;
    const float xs = q.x * 2, ys = q.y * 2, zs = q.z * 2;
    const float wx = q.w * xs, wy = q.w * ys, wz = q.w * zs;
    const float xx = q.x * xs, xy = q.x * ys, xz = q.x * zs;
    const float yy = q.y * ys, yz = q.y * zs, zz = q.z * zs;
    m[0] = 1 - (yy + zz); m[4] = xy - wz; m[8] = xz + wy;
    m[1] = xy + wz; m[5] = 1 - (xx + zz); m[9] = yz - wx;
    m[2] = xz - wy; m[6] = yz + wx; m[10] = 1 - (xx + yy);
    return m;
}

// AngAxisF::setMatrix, then setColumn(3, pos).
inline Matrix matrix(const float pos[3], const AngAxis& a) {
    Matrix m = matrix(quat(a));
    m[3] = pos[0]; m[7] = pos[1]; m[11] = pos[2];
    return m;
}

// QuatF::set(MatrixF).
inline Quat quat(const Matrix& m) {
    auto at = [&](int r, int c) { return m[r * 4 + c]; };
    Quat q;
    const float trace = at(0, 0) + at(1, 1) + at(2, 2);
    if (trace > 0.0f) {
        float s = std::sqrt(trace + 1.0f);
        q.w = s * 0.5f;
        s = 0.5f / s;
        q.x = (at(1, 2) - at(2, 1)) * s;
        q.y = (at(2, 0) - at(0, 2)) * s;
        q.z = (at(0, 1) - at(1, 0)) * s;
    } else {
        float* v = &q.x;
        int i = 0;
        if (at(1, 1) > at(0, 0)) i = 1;
        if (at(2, 2) > at(i, i)) i = 2;
        const int j = (i + 1) % 3, k = (j + 1) % 3;
        float s = std::sqrt((at(i, i) - (at(j, j) + at(k, k))) + 1.0f);
        v[i] = s * 0.5f;
        s = 0.5f / s;
        v[j] = (at(i, j) + at(j, i)) * s;
        v[k] = (at(i, k) + at(k, i)) * s;
        q.w = (at(j, k) - at(k, j)) * s;
    }
    return q;
}

// AngAxisF::set(QuatF).
inline AngAxis angAxis(const Quat& q) {
    AngAxis a;
    a.angle = std::acos(q.w) * 2;
    const float s = std::sqrt(1 - q.w * q.w);
    if (s != 0) { a.x = q.x / s; a.y = q.y / s; a.z = q.z / s; }
    else { a.x = 1; a.y = 0; a.z = 0; }
    return a;
}

inline Matrix mul(const Matrix& a, const Matrix& b) {
    Matrix r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float sum = 0;
            for (int k = 0; k < 4; ++k) sum += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = sum;
        }
    return r;
}

inline void mulP(const Matrix& m, const float p[3], float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = m[i * 4] * p[0] + m[i * 4 + 1] * p[1] + m[i * 4 + 2] * p[2] + m[i * 4 + 3];
}

inline void mulV(const Matrix& m, const float v[3], float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = m[i * 4] * v[0] + m[i * 4 + 1] * v[1] + m[i * 4 + 2] * v[2];
}

} // namespace TorqueMath
