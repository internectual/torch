#include "sim/server_container.h"
#include "sim/net_object.h"
#include "render/renderer.h"
#include "render/dif_loader.h"
#include "core/engine.h"
#include "core/console.h"
#include "core/timer.h"
#include "script/script_engine.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <strings.h>

namespace ServerContainer {
namespace {

struct Terrain {
    std::unique_ptr<TerrainBlock> block; // Y-up heightfield (render/renderer.h)
    const ScriptObject* object = nullptr;
};

struct Water {
    float x0, y0, x1, y1, bottom, level;
    float density, viscosity;
    int liquidType;
};

// Interior hull triangles on a uniform grid over x/y.
struct Interiors {
    std::vector<PlayerPrediction::Triangle> triangles;
    std::vector<const ScriptObject*> owners; // the InteriorInstance of each triangle
    float cell = 16.0f;
    float minX = 0, minY = 0;
    int resX = 0, resY = 0;
    std::vector<std::vector<int>> cells;
};

struct State {
    std::vector<Terrain> terrains;
    std::vector<Water> water;
    Interiors interiors;
    std::string signature;
    double lastCheck = -1.0;
};

State& state() {
    static State s;
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::vector<uint8_t> readAsset(const std::vector<std::string>& candidates) {
    if (!Engine::instance().filesys) return {};
    for (const auto& path : candidates) {
        auto bytes = Engine::instance().fs().read(path.c_str());
        if (!bytes.empty()) return bytes;
    }
    return {};
}

std::vector<uint32_t> emptySquareRuns(const std::string& value) {
    std::vector<uint32_t> runs;
    const char* cursor = value.c_str();
    while (*cursor) {
        char* end = nullptr;
        const unsigned long long packed = std::strtoull(cursor, &end, 0);
        if (end == cursor) { ++cursor; continue; }
        if (packed <= 0xffffffffull) runs.push_back((uint32_t)packed);
        cursor = end;
    }
    return runs;
}

// The mission objects the container is built from, as "class:id;...".
std::string currentSignature() {
    std::string sig;
    for (auto& [name, object] : ScriptEngine::instance().objects) {
        if (!object) continue;
        const std::string& cls = object->className;
        if (strcasecmp(cls.c_str(), "TerrainBlock") && strcasecmp(cls.c_str(), "InteriorInstance") &&
            strcasecmp(cls.c_str(), "WaterBlock"))
            continue;
        sig += cls + ":" + std::to_string(ScriptEngine::instance().objectId(object)) + ";";
    }
    return sig;
}

void addTerrain(const ScriptObject* object) {
    std::string file = Fields::string(object, "terrainFile");
    for (char& c : file) if (c == '\\') c = '/';
    if (file.empty()) return;
    if (!lower(file).ends_with(".ter")) file += ".ter";
    std::vector<std::string> paths{file};
    if (!lower(file).starts_with("terrains/")) paths.push_back("terrains/" + file);
    const auto bytes = readAsset(paths);
    if (bytes.empty()) {
        Console::instance().printf(LogLevel::Warn, "Server container: terrain '%s' not found", file.c_str());
        return;
    }
    Terrain terrain;
    terrain.object = object;
    terrain.block = std::make_unique<TerrainBlock>();
    auto& b = *terrain.block;
    b.squareSize = Fields::f32(object, "squareSize", b.squareSize);
    b.heightScale = Fields::f32(object, "heightScale", b.heightScale);
    b.setEmptySquareRuns(emptySquareRuns(Fields::string(object, "emptySquares")));
    const auto p = Fields::point(object, "position", {-1024, -1024, 0});
    b.worldOffset = {p[0], p[2], -p[1]};
    if (!b.load(bytes.data(), bytes.size())) return;
    state().terrains.push_back(std::move(terrain));
}

void addInterior(const ScriptObject* object, std::vector<PlayerPrediction::Triangle>& out) {
    std::string file = Fields::string(object, "interiorFile");
    for (char& c : file) if (c == '\\') c = '/';
    if (file.empty()) return;
    std::vector<std::string> paths{file};
    if (!lower(file).starts_with("interiors/")) paths.push_back("interiors/" + file);
    const auto bytes = readAsset(paths);
    if (bytes.empty()) {
        Console::instance().printf(LogLevel::Warn, "Server container: interior '%s' not found", file.c_str());
        return;
    }
    const DIFLoadResult dif = loadDIF(bytes.data(), bytes.size(), file.c_str(), true);
    if (!dif.loaded) return;
    auto* scene = dynamic_cast<SceneObject*>(object->engine.get());
    if (!scene) return;
    const auto& m = scene->transform;
    const float* s = scene->scale;
    auto world = [&](size_t i) {
        const float x = dif.hullCollisionVerts[i * 3] * s[0], y = dif.hullCollisionVerts[i * 3 + 1] * s[1],
                    z = dif.hullCollisionVerts[i * 3 + 2] * s[2];
        return Point3F{m[0] * x + m[1] * y + m[2] * z + m[3], m[4] * x + m[5] * y + m[6] * z + m[7],
                       m[8] * x + m[9] * y + m[10] * z + m[11]};
    };
    const size_t verts = dif.hullCollisionVerts.size() / 3;
    for (size_t k = 0; k + 2 < dif.hullCollisionIndices.size(); k += 3) {
        const uint32_t ia = dif.hullCollisionIndices[k], ib = dif.hullCollisionIndices[k + 1],
                       ic = dif.hullCollisionIndices[k + 2];
        if (ia >= verts || ib >= verts || ic >= verts) continue;
        PlayerPrediction::Triangle t{world(ia), world(ib), world(ic), {}};
        Point3F n = PlayerPrediction::cross(PlayerPrediction::sub(t.b, t.a), PlayerPrediction::sub(t.c, t.a));
        const float len = PlayerPrediction::length(n);
        if (len < 1e-12f) continue;
        t.n = PlayerPrediction::mul(n, 1.0f / len);
        out.push_back(t);
    }
}

// WaterBlock's liquidType enum (eOceanWater when unnamed).
int liquidType(const std::string& name) {
    static const char* types[] = {"Water", "OceanWater", "RiverWater", "StagnantWater",
                                  "Lava", "HotLava", "CrustyLava", "Quicksand"};
    for (int i = 0; i < 8; ++i) if (strcasecmp(name.c_str(), types[i]) == 0) return i;
    return 1;
}

void addWater(const ScriptObject* object) {
    const auto p = Fields::point(object, "position", {0, 0, 0});
    const auto sc = Fields::point(object, "scale", {1, 1, 1});
    if (sc[0] <= 0 || sc[1] <= 0) return;
    state().water.push_back({p[0], p[1], p[0] + sc[0], p[1] + sc[1], p[2], p[2] + sc[2],
                             Fields::f32(object, "density", 1.0f), Fields::f32(object, "viscosity", 15.0f),
                             liquidType(Fields::string(object, "liquidType"))});
}

void buildGrid(Interiors& in) {
    in.cells.clear();
    in.resX = in.resY = 0;
    if (in.triangles.empty()) return;
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (const auto& t : in.triangles)
        for (const Point3F* v : {&t.a, &t.b, &t.c}) {
            minX = std::min(minX, v->x); maxX = std::max(maxX, v->x);
            minY = std::min(minY, v->y); maxY = std::max(maxY, v->y);
        }
    in.minX = minX;
    in.minY = minY;
    in.resX = std::max(1, (int)std::ceil((maxX - minX) / in.cell) + 1);
    in.resY = std::max(1, (int)std::ceil((maxY - minY) / in.cell) + 1);
    in.cells.assign((size_t)in.resX * in.resY, {});
    for (size_t i = 0; i < in.triangles.size(); ++i) {
        const auto& t = in.triangles[i];
        const float lx = std::min({t.a.x, t.b.x, t.c.x}), hx = std::max({t.a.x, t.b.x, t.c.x});
        const float ly = std::min({t.a.y, t.b.y, t.c.y}), hy = std::max({t.a.y, t.b.y, t.c.y});
        const int x0 = (int)((lx - minX) / in.cell), x1 = (int)((hx - minX) / in.cell);
        const int y0 = (int)((ly - minY) / in.cell), y1 = (int)((hy - minY) / in.cell);
        for (int y = y0; y <= y1 && y < in.resY; ++y)
            for (int x = x0; x <= x1 && x < in.resX; ++x) in.cells[(size_t)y * in.resX + x].push_back((int)i);
    }
}

void ensureBuilt() {
    auto& s = state();
    const double now = Timer::now();
    if (s.lastCheck >= 0 && now - s.lastCheck < 0.5) return;
    s.lastCheck = now;
    if (currentSignature() != s.signature) rebuild();
}

} // namespace

void rebuild() {
    auto& s = state();
    s.terrains.clear();
    s.water.clear();
    s.interiors = {};
    s.signature = currentSignature();
    std::vector<const ScriptObject*> terrains, interiors, water;
    for (auto& [name, object] : ScriptEngine::instance().objects) {
        if (!object) continue;
        const char* cls = object->className.c_str();
        if (!strcasecmp(cls, "TerrainBlock")) terrains.push_back(object);
        else if (!strcasecmp(cls, "InteriorInstance")) interiors.push_back(object);
        else if (!strcasecmp(cls, "WaterBlock")) water.push_back(object);
    }
    for (auto* t : terrains) addTerrain(t);
    for (auto* i : interiors) {
        addInterior(i, s.interiors.triangles);
        s.interiors.owners.resize(s.interiors.triangles.size(), i);
    }
    for (auto* w : water) addWater(w);
    buildGrid(s.interiors);
    if (!terrains.empty() || !interiors.empty())
        Console::instance().printf(LogLevel::Info, "Server container: %zu terrain, %zu interior triangles, %zu water",
                                   s.terrains.size(), s.interiors.triangles.size(), s.water.size());
}

void refresh() {
    auto& s = state();
    s.lastCheck = Timer::now();
    if (currentSignature() != s.signature) rebuild();
}

void gatherTriangles(const Point3F& min, const Point3F& max, std::vector<PlayerPrediction::Triangle>& out) {
    ensureBuilt();
    gatherGeometry(min, max, true, true, out);
}

void gatherGeometry(const Point3F& min, const Point3F& max, bool withTerrain, bool withInteriors,
                    std::vector<PlayerPrediction::Triangle>& out, std::vector<const ScriptObject*>* owners) {
    ensureBuilt();
    auto& s = state();
    // Terrain: the heightfield's Y-up rect, faces up.
    for (const auto& terrain : s.terrains) {
        if (!withTerrain) break;
        std::vector<Point3F> tris;
        terrain.block->appendTrianglesInRect(min.x, -max.y, max.x, -min.y, tris);
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            const Point3F a{tris[i].x, -tris[i].z, tris[i].y}, b{tris[i + 1].x, -tris[i + 1].z, tris[i + 1].y},
                          c{tris[i + 2].x, -tris[i + 2].z, tris[i + 2].y};
            if (std::max({a.z, b.z, c.z}) < min.z || std::min({a.z, b.z, c.z}) > max.z) continue;
            Point3F n = PlayerPrediction::cross(PlayerPrediction::sub(b, a), PlayerPrediction::sub(c, a));
            if (n.z < 0) n = PlayerPrediction::mul(n, -1.0f);
            const float len = PlayerPrediction::length(n);
            if (len < 1e-12f) continue;
            out.push_back({a, b, c, PlayerPrediction::mul(n, 1.0f / len)});
            if (owners) owners->push_back(terrain.object);
        }
    }
    // Interiors from the grid cells the box touches.
    const auto& in = s.interiors;
    if (withInteriors && in.resX > 0) {
        auto cellOf = [&](float v, float lo, int res) { return std::clamp((int)std::floor((v - lo) / in.cell), 0, res - 1); };
        const int x0 = cellOf(min.x, in.minX, in.resX), x1 = cellOf(max.x, in.minX, in.resX);
        const int y0 = cellOf(min.y, in.minY, in.resY), y1 = cellOf(max.y, in.minY, in.resY);
        std::vector<char> seen(in.triangles.size(), 0);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                for (int i : in.cells[(size_t)y * in.resX + x]) {
                    if (seen[i]) continue;
                    seen[i] = 1;
                    const auto& t = in.triangles[i];
                    if (std::max({t.a.x, t.b.x, t.c.x}) < min.x || std::min({t.a.x, t.b.x, t.c.x}) > max.x ||
                        std::max({t.a.y, t.b.y, t.c.y}) < min.y || std::min({t.a.y, t.b.y, t.c.y}) > max.y ||
                        std::max({t.a.z, t.b.z, t.c.z}) < min.z || std::min({t.a.z, t.b.z, t.c.z}) > max.z)
                        continue;
                    out.push_back(t);
                    if (owners) owners->push_back(in.owners[i]);
                }
    }
}

bool terrainBlock(Point3F& origin, float& squareSize) {
    ensureBuilt();
    const auto& s = state();
    if (s.terrains.empty()) return false;
    const TerrainBlock& b = *s.terrains.front().block;
    origin = {b.worldOffset.x, -b.worldOffset.z, b.worldOffset.y};
    squareSize = b.squareSize;
    return true;
}

bool terrainHeight(const Point2F& pos, float* height, Point3F* normal, bool normalize) {
    ensureBuilt();
    const auto& s = state();
    if (s.terrains.empty()) return false;
    const TerrainBlock& b = *s.terrains.front().block;
    if (b.heights.empty() || b.size < 2 || b.squareSize <= 0.0f) return false;
    const int mask = b.size - 1;   // BlockMask
    const float invSquareSize = 1.0f / b.squareSize;
    float xp = pos.x * invSquareSize;
    float yp = pos.y * invSquareSize;
    int x = (int)std::floor(xp);
    int y = (int)std::floor(yp);
    xp -= (float)x;
    yp -= (float)y;
    x &= mask;
    y &= mask;
    // Grid rows run along Torque +y (the Y-up heightfield's -z).
    if (!b.emptySquares.empty() && b.emptySquares[(size_t)y * b.size + x]) return false;
    auto h = [&](int hx, int hy) { return b.heights[(size_t)(hy & mask) * b.size + (hx & mask)] * b.heightScale; };
    const float zBottomLeft = h(x, y), zBottomRight = h(x + 1, y);
    const float zTopLeft = h(x, y + 1), zTopRight = h(x + 1, y + 1);
    const float sq = b.squareSize;
    Point3F n;
    float z;
    if (((x ^ y) & 1) == 0) {   // Split45
        if (xp > yp) {
            n = {zBottomLeft - zBottomRight, zBottomRight - zTopRight, sq};
            z = zBottomLeft + xp * (zBottomRight - zBottomLeft) + yp * (zTopRight - zBottomRight);
        } else {
            n = {zTopLeft - zTopRight, zBottomLeft - zTopLeft, sq};
            z = zBottomLeft + xp * (zTopRight - zTopLeft) + yp * (zTopLeft - zBottomLeft);
        }
    } else {
        if (1.0f - xp > yp) {
            n = {zBottomLeft - zBottomRight, zBottomLeft - zTopLeft, sq};
            z = zBottomRight + (1.0f - xp) * (zBottomLeft - zBottomRight) + yp * (zTopLeft - zBottomLeft);
        } else {
            n = {zTopLeft - zTopRight, zBottomRight - zTopRight, sq};
            z = zBottomRight + (1.0f - xp) * (zTopLeft - zTopRight) + yp * (zTopRight - zBottomRight);
        }
    }
    if (height) *height = z;
    if (normal) {
        if (normalize) {
            const float l2 = n.x * n.x + n.y * n.y + n.z * n.z;
            if (l2 != 0.0f) {
                const float f = 1.0f / std::sqrt(l2);
                n = {n.x * f, n.y * f, n.z * f};
            } else {
                n = {0, 0, 1};
            }
        }
        *normal = n;
    }
    return true;
}

bool waterFind(const Point3F& min, const Point3F& max, WaterInfo& out) {
    ensureBuilt();
    bool found = false;
    for (const auto& w : state().water) {
        if (min.x > std::max(w.x0, w.x1) || max.x < std::min(w.x0, w.x1) || min.y > std::max(w.y0, w.y1) ||
            max.y < std::min(w.y0, w.y1) || min.z > w.level || max.z < w.bottom)
            continue;
        out.coverage = w.level < max.z ? (w.level - min.z) / (max.z - min.z) : 1.0f;
        out.liquidType = w.liquidType;
        out.density = w.density;
        out.viscosity = w.viscosity;
        out.surface = w.level;
        found = true;
    }
    return found;
}

float waterSurfaceAt(float x, float y) {
    ensureBuilt();
    float best = std::numeric_limits<float>::quiet_NaN();
    for (const auto& w : state().water)
        if (x >= std::min(w.x0, w.x1) && x <= std::max(w.x0, w.x1) && y >= std::min(w.y0, w.y1) &&
            y <= std::max(w.y0, w.y1) && !(w.level <= best))
            best = w.level;
    return best;
}

} // namespace ServerContainer
