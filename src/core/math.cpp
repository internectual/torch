#include "core/math.h"
#include <cstring>
#include <cmath>
#include <algorithm>

void MatrixF::identity() {
    std::memset(m, 0, sizeof(m));
    m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}

MatrixF MatrixF::operator*(const MatrixF& o) const {
    MatrixF r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j] + m[i][3] * o.m[3][j];
    return r;
}

Point3F MatrixF::transform(const Point3F& p) const {
    return {
        m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
        m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
        m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]
    };
}

Point3F MatrixF::transformNormal(const Point3F& n) const {
    return {
        m[0][0] * n.x + m[0][1] * n.y + m[0][2] * n.z,
        m[1][0] * n.x + m[1][1] * n.y + m[1][2] * n.z,
        m[2][0] * n.x + m[2][1] * n.y + m[2][2] * n.z
    };
}

MatrixF MatrixF::inverse() const {
    float A[4][8];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (!std::isfinite(m[i][j])) { MatrixF id; return id; }
            A[i][j] = m[i][j];
        }
        for (int j = 0; j < 4; j++) A[i][4 + j] = (i == j) ? 1.0f : 0.0f;
    }
    for (int col = 0; col < 4; col++) {
        int piv = col;
        for (int r = col + 1; r < 4; r++)
            if (std::fabs(A[r][col]) > std::fabs(A[piv][col])) piv = r;
        if (!std::isfinite(A[piv][col]) || std::fabs(A[piv][col]) < 1e-8f) {
            MatrixF id;
            return id;
        }
        if (piv != col) for (int j = 0; j < 8; j++) std::swap(A[col][j], A[piv][j]);
        float d = A[col][col];
        for (int j = 0; j < 8; j++) A[col][j] /= d;
        for (int r = 0; r < 4; r++) {
            if (r == col) continue;
            float f = A[r][col];
            if (f == 0.0f) continue;
            for (int j = 0; j < 8; j++) A[r][j] -= f * A[col][j];
        }
    }
    MatrixF res;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            if (!std::isfinite(A[i][4 + j])) { MatrixF id; return id; }
            res.m[i][j] = A[i][4 + j];
        }
    return res;
}

void MatrixF::setRotationX(float a) {
    identity();
    float c = std::cos(a), s = std::sin(a);
    m[1][1] = c; m[1][2] = -s;
    m[2][1] = s; m[2][2] = c;
}

void MatrixF::setRotationY(float a) {
    identity();
    float c = std::cos(a), s = std::sin(a);
    m[0][0] = c; m[0][2] = s;
    m[2][0] = -s; m[2][2] = c;
}

void MatrixF::setRotationZ(float a) {
    identity();
    float c = std::cos(a), s = std::sin(a);
    m[0][0] = c; m[0][1] = -s;
    m[1][0] = s; m[1][1] = c;
}

void MatrixF::setRotationAxis(const Point3F& axis, float angle) {
    identity();
    const float length = std::hypot(std::hypot(axis.x, axis.y), axis.z);
    if (!std::isfinite(length) || length < 1e-6f || !std::isfinite(angle)) return;
    float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    float ax = axis.x / length, ay = axis.y / length, az = axis.z / length;
    m[0][0] = t * ax * ax + c;     m[0][1] = t * ax * ay - s * az;  m[0][2] = t * ax * az + s * ay;
    m[1][0] = t * ax * ay + s * az; m[1][1] = t * ay * ay + c;      m[1][2] = t * ay * az - s * ax;
    m[2][0] = t * ax * az - s * ay; m[2][1] = t * ay * az + s * ax;  m[2][2] = t * az * az + c;
}

void MatrixF::setTranslation(const Point3F& t) {
    m[0][3] = t.x; m[1][3] = t.y; m[2][3] = t.z;
}

void MatrixF::setScale(const Point3F& s) {
    identity();
    m[0][0] = s.x; m[1][1] = s.y; m[2][2] = s.z;
}

void MatrixF::perspective(float fov, float aspect, float near, float far) {
    std::memset(m, 0, sizeof(m));
    float f = 1.0f / std::tan(fov * 0.5f);
    m[0][0] = f / aspect;
    m[1][1] = f;
    m[2][2] = (far + near) / (near - far);
    m[2][3] = 2.0f * far * near / (near - far);
    m[3][2] = -1.0f;
}

void MatrixF::orthographic(float left, float right, float bottom, float top, float near, float far) {
    std::memset(m, 0, sizeof(m));
    m[0][0] = 2.0f / (right - left);
    m[1][1] = 2.0f / (top - bottom);
    m[2][2] = -2.0f / (far - near);
    m[0][3] = -(right + left) / (right - left);
    m[1][3] = -(top + bottom) / (top - bottom);
    m[2][3] = -(far + near) / (far - near);
    m[3][3] = 1.0f;
}

void MatrixF::lookAt(const Point3F& eye, const Point3F& center, const Point3F& up) {
    // Demo and observer camera state can be incomplete for one frame while a
    // target is created or removed. Keep the view usable instead of allowing
    // a non-finite basis to poison every vertex submitted that frame.
    const auto finite = [](const Point3F& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z);
    };
    if (!finite(eye) || !finite(center) || !finite(up)) {
        identity();
        return;
    }
    Point3F f = { center.x - eye.x, center.y - eye.y, center.z - eye.z };
    float flen = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
    if (flen < 1e-6f) { f = { 0, 0, -1 }; flen = 1.0f; }
    f.x /= flen; f.y /= flen; f.z /= flen;

    Point3F s = { f.y * up.z - f.z * up.y, f.z * up.x - f.x * up.z, f.x * up.y - f.y * up.x };
    float slen = std::sqrt(s.x * s.x + s.y * s.y + s.z * s.z);
    if (slen < 1e-6f) { s = { 1, 0, 0 }; slen = 1.0f; }
    s.x /= slen; s.y /= slen; s.z /= slen;
    // Re-orthogonalize s against f (in case the fallback was parallel to f).
    float sd = s.x * f.x + s.y * f.y + s.z * f.z;
    s.x -= f.x * sd; s.y -= f.y * sd; s.z -= f.z * sd;
    float slen2 = std::sqrt(s.x * s.x + s.y * s.y + s.z * s.z);
    if (slen2 > 1e-6f) { s.x /= slen2; s.y /= slen2; s.z /= slen2; }
    else { s = { 0, 1, 0 }; }

    Point3F u = { s.y * f.z - s.z * f.y, s.z * f.x - s.x * f.z, s.x * f.y - s.y * f.x };

    m[0][0] = s.x; m[0][1] = s.y; m[0][2] = s.z; m[0][3] = -(s.x * eye.x + s.y * eye.y + s.z * eye.z);
    m[1][0] = u.x; m[1][1] = u.y; m[1][2] = u.z; m[1][3] = -(u.x * eye.x + u.y * eye.y + u.z * eye.z);
    m[2][0] = -f.x; m[2][1] = -f.y; m[2][2] = -f.z; m[2][3] = f.x * eye.x + f.y * eye.y + f.z * eye.z;
    m[3][0] = 0; m[3][1] = 0; m[3][2] = 0; m[3][3] = 1;
}

MatrixF QuatF::toMatrix() const {
    MatrixF m;
    const float length = std::hypot(std::hypot(x, y), std::hypot(z, w));
    if (!std::isfinite(length) || length < 1e-6f) return m;
    const float nx = x / length, ny = y / length, nz = z / length, nw = w / length;
    float x2 = nx * nx, y2 = ny * ny, z2 = nz * nz;
    float xy = nx * ny, xz = nx * nz, yz = ny * nz;
    float wx = nw * nx, wy = nw * ny, wz = nw * nz;
    m.m[0][0] = 1 - 2 * (y2 + z2); m.m[0][1] = 2 * (xy - wz);      m.m[0][2] = 2 * (xz + wy);
    m.m[1][0] = 2 * (xy + wz);      m.m[1][1] = 1 - 2 * (x2 + z2);  m.m[1][2] = 2 * (yz - wx);
    m.m[2][0] = 2 * (xz - wy);      m.m[2][1] = 2 * (yz + wx);      m.m[2][2] = 1 - 2 * (x2 + y2);
    return m;
}

QuatF QuatF::fromMatrix(const MatrixF& m) {
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            if (!std::isfinite(m.m[row][col])) return {};
    float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
    QuatF q;
    if (trace > 0) {
        const float root = trace + 1.0f;
        if (root <= 1e-8f) return {};
        float s = 0.5f / std::sqrt(root);
        q.w = 0.25f / s;
        q.x = (m.m[2][1] - m.m[1][2]) * s;
        q.y = (m.m[0][2] - m.m[2][0]) * s;
        q.z = (m.m[1][0] - m.m[0][1]) * s;
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        float s = 2.0f * std::sqrt(std::max(0.0f, 1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]));
        if (s < 1e-8f) return {};
        q.w = (m.m[2][1] - m.m[1][2]) / s;
        q.x = 0.25f * s;
        q.y = (m.m[0][1] + m.m[1][0]) / s;
        q.z = (m.m[0][2] + m.m[2][0]) / s;
    } else if (m.m[1][1] > m.m[2][2]) {
        float s = 2.0f * std::sqrt(std::max(0.0f, 1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]));
        if (s < 1e-8f) return {};
        q.w = (m.m[0][2] - m.m[2][0]) / s;
        q.x = (m.m[0][1] + m.m[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (m.m[1][2] + m.m[2][1]) / s;
    } else {
        float s = 2.0f * std::sqrt(std::max(0.0f, 1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]));
        if (s < 1e-8f) return {};
        q.w = (m.m[1][0] - m.m[0][1]) / s;
        q.x = (m.m[0][2] + m.m[2][0]) / s;
        q.y = (m.m[1][2] + m.m[2][1]) / s;
        q.z = 0.25f * s;
    }
    const float length = std::hypot(std::hypot(q.x, q.y), std::hypot(q.z, q.w));
    if (!std::isfinite(length) || length < 1e-6f) return {};
    q.x /= length; q.y /= length; q.z /= length; q.w /= length;
    return q;
}

QuatF QuatF::fromEuler(const Point3F& e) {
    float cx = std::cos(e.x * 0.5f), sx = std::sin(e.x * 0.5f);
    float cy = std::cos(e.y * 0.5f), sy = std::sin(e.y * 0.5f);
    float cz = std::cos(e.z * 0.5f), sz = std::sin(e.z * 0.5f);
    return {
        sx * cy * cz + cx * sy * sz,
        cx * sy * cz - sx * cy * sz,
        cx * cy * sz + sx * sy * cz,
        cx * cy * cz - sx * sy * sz
    };
}

bool Box3F::contains(const Point3F& p) const {
    return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
}

float PlaneF::distanceTo(const Point3F& p) const {
    return x * p.x + y * p.y + z * p.z + d;
}
