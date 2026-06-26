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
    Point3F transform(const Point3F& p) const;
    void setRotationX(float a);
    void setRotationY(float a);
    void setRotationZ(float a);
    void setRotationAxis(const Point3F& axis, float angle);
    void setTranslation(const Point3F& t);
    void setScale(const Point3F& s);
    void perspective(float fov, float aspect, float near, float far);
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
}
