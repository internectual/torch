#pragma once
#include <cmath>
#include <cstdint>

struct Point2I { int32_t x, y; };
struct Point2F { float x, y; };
struct Point3F { float x, y, z; };
struct Point4F { float x, y, z, w; };
struct RectI { int32_t x, y, w, h; };
struct RectF { float x, y, w, h; };

struct ColorF { float r, g, b, a; };

struct MatrixF {
    float m[4][4]{};

    MatrixF() { identity(); }
    void identity();
    MatrixF operator*(const MatrixF& o) const;
    MatrixF inverse() const;
    Point3F transform(const Point3F& p) const;
    Point3F transformNormal(const Point3F& n) const;
    void setRotationX(float a);
    void setRotationY(float a);
    void setRotationZ(float a);
    void setRotationAxis(const Point3F& axis, float angle);
    void setTranslation(const Point3F& t);
    void setScale(const Point3F& s);
    void perspective(float fov, float aspect, float near, float far);
    void orthographic(float left, float right, float bottom, float top, float near, float far);
    void lookAt(const Point3F& eye, const Point3F& center, const Point3F& up);
    const float* data() const { return &m[0][0]; }
};

struct QuatF {
    float x{}, y{}, z{}, w{1};
    QuatF() = default;
    QuatF(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
    MatrixF toMatrix() const;
    static QuatF fromEuler(const Point3F& euler);
};

struct Box3F {
    Point3F min, max;
    bool contains(const Point3F& p) const;
};

struct PlaneF {
    float x, y, z, d;
    float distanceTo(const Point3F& p) const;
};

namespace Math {
    constexpr float PI = 3.14159265358979323846f;
    constexpr float DEG2RAD(float d) { return d * PI / 180.0f; }
    constexpr float RAD2DEG(float r) { return r * 180.0f / PI; }
    constexpr float lerp(float a, float b, float t) { return a + (b - a) * t; }
    inline float clamp(float v, float mn, float mx) { return v < mn ? mn : (v > mx ? mx : v); }
    inline float min(float a, float b) { return a < b ? a : b; }
    inline float max(float a, float b) { return a > b ? a : b; }
    inline QuatF quatSlerp(const QuatF& a, const QuatF& b, float t) {
        float dot = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
        float sign = 1.0f;
        if (dot < 0) { dot = -dot; sign = -1.0f; }
        if (dot > 0.9999f) {
            // Almost parallel - use NLERP
            QuatF r = {Math::lerp(a.x, b.x * sign, t),
                       Math::lerp(a.y, b.y * sign, t),
                       Math::lerp(a.z, b.z * sign, t),
                       Math::lerp(a.w, b.w * sign, t)};
            float len = sqrtf(r.x*r.x + r.y*r.y + r.z*r.z + r.w*r.w);
            if (len > 1e-6f) { r.x /= len; r.y /= len; r.z /= len; r.w /= len; }
            return r;
        }
        float theta = acosf(clamp(dot, -1.0f, 1.0f));
        float sinTheta = sinf(theta);
        if (fabsf(sinTheta) < 1e-6f) return a;
        float ka = sinf((1-t)*theta) / sinTheta;
        float kb = sinf(t*theta) / sinTheta * sign;
        return {a.x*ka + b.x*kb, a.y*ka + b.y*kb, a.z*ka + b.z*kb, a.w*ka + b.w*kb};
    }

    // Z-up (T2) to Y-up (OpenGL) conversion matrix.
    // Maps (x,y,z) -> (x, z, -y).
    // det = +1 (rotation, no handedness flip).
    inline MatrixF czUpToYUp() {
        MatrixF c;
        c.m[0][0] = 1; c.m[0][1] = 0; c.m[0][2] = 0;  c.m[0][3] = 0;
        c.m[1][0] = 0; c.m[1][1] = 0; c.m[1][2] = 1;  c.m[1][3] = 0;
        c.m[2][0] = 0; c.m[2][1] =-1; c.m[2][2] = 0;  c.m[2][3] = 0;
        c.m[3][0] = 0; c.m[3][1] = 0; c.m[3][2] = 0;  c.m[3][3] = 1;
        return c;
    }
}
