#pragma once
// The spikes a LinearFlareProjectile (plasma bolt) keeps around itself
// (Tribes2.exe LinearFlareProjectile::advanceTime / ring builder / fan
// renderer, as ported by t2-mapper particles/flareSpikes.ts): each spike
// has a random direction, an opening angle animating from 87 degrees to
// 65-85, a base scale size[0] and a tip scale growing from size[1] +
// [0, 0.25) to size[2] + [0, 1) over 0.15-0.375 s, and a 0.4-0.995 s life,
// after which it respawns. Drawn as four six-vertex fans between two
// eight-point rings, additively, base ring at flareColor x brightness, tip
// ring at half, fan centre at a quarter.
#include <array>
#include <cmath>
#include <vector>

namespace FlareSpikes {

struct Spike {
    float angle0 = 1.518f, angle1 = 1.2f;
    float baseScale = 1.0f, tip0 = 1.0f, tip1 = 1.0f;
    float growSec = 0.2f, lifetimeSec = 0.5f, ageSec = 0.0f;
    float dir[3] = {1, 0, 0};
};

// One vertex of a spike fan (shape-local offsets) with its colour scale.
struct Vertex { float x, y, z, u, v, shade; };

template <typename Random>
inline Spike spawn(const std::array<float, 3>& sizes, Random&& random) {
    Spike s;
    float dx = 1.0f - 2.0f * random(), dy = 1.0f - 2.0f * random(), dz = 1.0f - 2.0f * random();
    float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-6f) len = 1.0f;
    s.dir[0] = dx / len; s.dir[1] = dy / len; s.dir[2] = dz / len;
    s.angle1 = (65.0f + 20.0f * random()) * 3.14159265f / 180.0f;
    s.baseScale = sizes[0];
    s.tip0 = sizes[1] + 0.25f * random();
    s.tip1 = sizes[2] + random();
    s.growSec = 0.15f + 0.225f * random();
    s.lifetimeSec = 0.4f + 0.595f * random();
    return s;
}

// advanceTime: age every spike; one past its lifetime respawns at once.
template <typename Random>
inline void advance(std::vector<Spike>& spikes, float dt, const std::array<float, 3>& sizes,
                    Random&& random) {
    for (auto& s : spikes) {
        s.ageSec += dt;
        if (s.ageSec > s.lifetimeSec) s = spawn(sizes, random);
    }
}

// Triangles (three vertices each) for every spike.
inline std::vector<Vertex> triangles(const std::vector<Spike>& spikes, float brightness = 1.0f) {
    static constexpr int Ring[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}};
    // ring (0 base, 1 tip), ring offset, u, v, shade index
    static constexpr float Fan[6][5] = {{0, 1, 0.5f, 0.9f, 0}, {0, 0, 0, 0.9f, 0}, {1, 0, 0, 0.1f, 1},
                                        {1, 1, 0.5f, 0.1f, 2}, {1, 2, 1, 0.1f, 1}, {0, 2, 1, 0.9f, 0}};
    std::vector<Vertex> out;
    out.reserve(spikes.size() * 48);
    for (const auto& s : spikes) {
        const float dx = s.dir[0], dy = s.dir[1], dz = s.dir[2];
        float p1x, p1y, p1z;
        if (std::fabs(dz) < 0.9f) { p1x = 0; p1y = dz; p1z = -dy; }
        else { p1x = -dz; p1y = 0; p1z = dx; }
        float l1 = std::sqrt(p1x * p1x + p1y * p1y + p1z * p1z); if (l1 < 1e-6f) l1 = 1;
        p1x /= l1; p1y /= l1; p1z /= l1;
        float p2x = -(dy * p1z - dz * p1y), p2y = -(dz * p1x - dx * p1z), p2z = -(dx * p1y - dy * p1x);
        float l2 = std::sqrt(p2x * p2x + p2y * p2y + p2z * p2z); if (l2 < 1e-6f) l2 = 1;
        p2x /= l2; p2y /= l2; p2z /= l2;
        float angle, tip, bright;
        if (s.ageSec < s.growSec) {
            const float t = s.ageSec / s.growSec;
            angle = s.angle0; tip = s.tip0 + (s.tip1 - s.tip0) * t; bright = t;
        } else {
            const float t = (s.ageSec - s.growSec) / (s.lifetimeSec - s.growSec);
            angle = s.angle0 + (s.angle1 - s.angle0) * t; tip = s.tip1; bright = 1.0f - t;
        }
        const float c = std::cos(angle), pull = 1.0f - std::sin(angle);
        const float ax = p2x * c, ay = p2y * c, az = p2z * c;
        const float bx = p1x * c, by = p1y * c, bz = p1z * c;
        const float f = brightness * std::max(0.0f, bright);
        const float shade[3] = {f * brightness, f * 0.5f, f * 0.25f};
        for (int fan = 0; fan < 8; fan += 2) {
            Vertex verts[6];
            for (int k = 0; k < 6; ++k) {
                const bool tipRing = Fan[k][0] != 0.0f;
                const int ring = (fan + (int)Fan[k][1]) & 7;
                const float ox = dx + Ring[ring][0] * ax + Ring[ring][1] * bx;
                const float oy = dy + Ring[ring][0] * ay + Ring[ring][1] * by;
                const float oz = dz + Ring[ring][0] * az + Ring[ring][1] * bz;
                const float scale = tipRing ? tip : s.baseScale, back = tipRing ? pull : 0.0f;
                verts[k] = {ox * scale - dx * back, oy * scale - dy * back, oz * scale - dz * back,
                            Fan[k][2], Fan[k][3], shade[(int)Fan[k][4]]};
            }
            for (int tri = 1; tri < 5; ++tri) {
                out.push_back(verts[0]);
                out.push_back(verts[tri]);
                out.push_back(verts[tri + 1]);
            }
        }
    }
    return out;
}

} // namespace FlareSpikes
