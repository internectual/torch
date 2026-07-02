#include "core/math.h"
#include <cstring>
#include <cmath>

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
    float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    float ax = axis.x, ay = axis.y, az = axis.z;
    m[0][0] = t * ax * ax + c;     m[0][1] = t * ax * ay - s * az;  m[0][2] = t * ax * az + s * ay;
    m[1][0] = t * ax * ay + s * az; m[1][1] = t * ay * ay + c;      m[1][2] = t * ay * az - s * ax;
    m[2][0] = t * ax * az - s * ay; m[2][1] = t * ay * az + s * ax;  m[2][2] = t * az * az + c;
}

void MatrixF::setTranslation(const Point3F& t) {
    m[3][0] = t.x; m[3][1] = t.y; m[3][2] = t.z;
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
    m[2][3] = -1.0f;
    m[3][2] = 2.0f * far * near / (near - far);
}

void MatrixF::orthographic(float left, float right, float bottom, float top, float near, float far) {
    std::memset(m, 0, sizeof(m));
    m[0][0] = 2.0f / (right - left);
    m[1][1] = 2.0f / (top - bottom);
    m[2][2] = -2.0f / (far - near);
    m[3][0] = -(right + left) / (right - left);
    m[3][1] = -(top + bottom) / (top - bottom);
    m[3][2] = -(far + near) / (far - near);
    m[3][3] = 1.0f;
}

void MatrixF::lookAt(const Point3F& eye, const Point3F& center, const Point3F& up) {
    Point3F f = { center.x - eye.x, center.y - eye.y, center.z - eye.z };
    float flen = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
    f.x /= flen; f.y /= flen; f.z /= flen;

    Point3F s = { f.y * up.z - f.z * up.y, f.z * up.x - f.x * up.z, f.x * up.y - f.y * up.x };
    float slen = std::sqrt(s.x * s.x + s.y * s.y + s.z * s.z);
    s.x /= slen; s.y /= slen; s.z /= slen;

    Point3F u = { s.y * f.z - s.z * f.y, s.z * f.x - s.x * f.z, s.x * f.y - s.y * f.x };

    m[0][0] = s.x; m[0][1] = s.y; m[0][2] = s.z; m[0][3] = 0;
    m[1][0] = u.x; m[1][1] = u.y; m[1][2] = u.z; m[1][3] = 0;
    m[2][0] = -f.x; m[2][1] = -f.y; m[2][2] = -f.z; m[2][3] = 0;
    m[3][0] = -(s.x * eye.x + s.y * eye.y + s.z * eye.z);
    m[3][1] = -(u.x * eye.x + u.y * eye.y + u.z * eye.z);
    m[3][2] = f.x * eye.x + f.y * eye.y + f.z * eye.z;
    m[3][3] = 1;
}

MatrixF QuatF::toMatrix() const {
    MatrixF m;
    float x2 = x * x, y2 = y * y, z2 = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;
    m.m[0][0] = 1 - 2 * (y2 + z2); m.m[0][1] = 2 * (xy - wz);      m.m[0][2] = 2 * (xz + wy);
    m.m[1][0] = 2 * (xy + wz);      m.m[1][1] = 1 - 2 * (x2 + z2);  m.m[1][2] = 2 * (yz - wx);
    m.m[2][0] = 2 * (xz - wy);      m.m[2][1] = 2 * (yz + wx);      m.m[2][2] = 1 - 2 * (x2 + y2);
    return m;
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
