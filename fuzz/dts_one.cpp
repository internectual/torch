// One-shot DTS loader inspector. Builds + loads a .dts and prints a parse
// summary + a bind-pose skinning sanity check (deformed verts must equal the
// stored initial positions when nodeWorld == bind pose).
//
// Build (mirror fuzz/build.sh, swap in this file, keep engine_stub.cpp):
//   clang++ -std=c++20 -O1 -g -fsanitize=address,undefined -DGLEW_STATIC -Isrc \
//     fuzz/dts_one.cpp fuzz/console_stub.cpp fuzz/render_stubs.cpp \
//     src/render/dif_loader.cpp src/render/dts_loader.cpp src/render/glb_loader.cpp \
//     src/script/dso_reader.cpp src/fs/vol_archive.cpp src/fs/vl2_archive.cpp \
//     src/fs/file_system.cpp src/core/math.cpp -o fuzz/dts_one -lpthread -lz
//
// engine.h must expose Engine::filesys publicly (temporary test-only change).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <cmath>

#include "render/dts_loader.h"
#include "core/engine.h"
#include "fs/file_system.h"

static FileSystem g_fs; // empty filesystem; texture lookup just yields nothing

static float maxDelta(const std::vector<Vertex>& verts,
                      const std::vector<Point3F>& ref) {
    float md = 0.0f;
    size_t n = std::min(verts.size(), ref.size());
    for (size_t i = 0; i < n; i++) {
        float dx = verts[i].pos.x - ref[i].x;
        float dy = verts[i].pos.y - ref[i].y;
        float dz = verts[i].pos.z - ref[i].z;
        float d = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (d > md) md = d;
    }
    return md;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.dts>\n", argv[0]); return 2; }
    // Install an empty filesystem so loadDTS's texture-resolution block is safe.
    Engine::instance().filesys = &g_fs;

    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(sz);
    if (fread(data.data(), 1, sz, f) != (size_t)sz) { fprintf(stderr, "read fail\n"); return 2; }
    fclose(f);

    DTSLoadResult r = loadDTS(data.data(), data.size(), argv[1]);

    printf("=== loadDTS('%s') ===\n", argv[1]);
    printf("loaded            : %s\n", r.loaded ? "YES" : "NO (!corrupted)");
    printf("meshes            : %zu\n", r.meshes.size());
    printf("nodes             : %zu\n", r.nodes.size());
    printf("defaultTransforms : %zu\n", r.defaultTransforms.size());
    printf("skins             : %zu\n", r.skins.size());
    printf("animations        : %zu\n", r.animations.size());
    printf("details           : %zu\n", r.details.size());
    printf("materials         : %zu\n", r.materialNames.size());
    printf("textures          : %zu\n", r.textures.size());

    size_t skinCount = 0;
    for (size_t i = 0; i < r.meshes.size(); i++) {
        bool hs = (i < r.skins.size()) ? r.skins[i].hasSkin : false;
        if (hs) skinCount++;
        printf("  mesh[%zu] verts=%-5zu tris=%-5zu hasSkin=%d nodeIndex=%d\n",
               i, r.meshes[i].vertices.size(),
               r.meshes[i].indices.size()/3, hs, r.meshes[i].nodeIndex);
    }
    printf("skin meshes       : %zu\n", skinCount);

    if (r.defaultTransforms.size() != r.nodes.size())
        printf("\nWARN: defaultTransforms.size (%zu) != nodes.size (%zu)\n",
               r.defaultTransforms.size(), r.nodes.size());

    bool anySkin = false;
    for (size_t i = 0; i < r.meshes.size(); i++) {
        if (i >= r.skins.size() || !r.skins[i].hasSkin) continue;
        if (r.skins[i].initialPositions.empty()) continue;
        anySkin = true;
        updateSkinnedMesh(r.meshes[i], r.skins[i], r.defaultTransforms, r.defaultTransforms);
        float md = maxDelta(r.meshes[i].vertices, r.skins[i].initialPositions);
        size_t infl = r.skins[i].boneIndices.size();
        size_t bones = r.skins[i].initialTransforms.size();
        size_t nodes = r.skins[i].nodeIndices.size();
        printf("  bindpose mesh[%zu]: maxDelta(initialPos)=%.6f infl=%zu bones=%zu nodeIdx=%zu\n",
               i, md, infl, bones, nodes);
    }
    if (!anySkin) printf("(no skin meshes to bind-pose test)\n");

    printf("\n--- nodes (parent -> name) ---\n");
    for (size_t i = 0; i < r.nodes.size(); i++)
        printf("  node[%zu] parent=%-3d name='%s'\n", i, r.nodes[i].parentIndex, r.nodes[i].name.c_str());

    if (!r.defaultTransforms.empty()) {
        printf("\n--- defaultTransforms (first 5 node world matrices) ---\n");
        size_t n = std::min(r.defaultTransforms.size(), (size_t)5);
        for (size_t i = 0; i < n; i++) {
            auto& m = r.defaultTransforms[i];
            printf("  nodeWorld[%zu]: [%+.4f %+.4f %+.4f %+.4f]\n", i, m.m[0][0], m.m[0][1], m.m[0][2], m.m[0][3]);
            printf("                 [%+.4f %+.4f %+.4f %+.4f]\n",   m.m[1][0], m.m[1][1], m.m[1][2], m.m[1][3]);
            printf("                 [%+.4f %+.4f %+.4f %+.4f]\n",   m.m[2][0], m.m[2][1], m.m[2][2], m.m[2][3]);
            printf("                 [%+.4f %+.4f %+.4f %+.4f]\n",   m.m[3][0], m.m[3][1], m.m[3][2], m.m[3][3]);
        }
    }

    printf("\n--- vertex bounds (before node transform) ---\n");
    Point3F gmn{1e9f,1e9f,1e9f}, gmx{-1e9f,-1e9f,-1e9f};
    for (size_t mi = 0; mi < r.meshes.size(); mi++) {
        auto& mesh = r.meshes[mi];
        for (auto& v : mesh.vertices) {
            if (v.pos.x < gmn.x) gmn.x = v.pos.x; if (v.pos.y < gmn.y) gmn.y = v.pos.y; if (v.pos.z < gmn.z) gmn.z = v.pos.z;
            if (v.pos.x > gmx.x) gmx.x = v.pos.x; if (v.pos.y > gmx.y) gmx.y = v.pos.y; if (v.pos.z > gmx.z) gmx.z = v.pos.z;
        }
    }
    printf("  raw local: min=(%.4f, %.4f, %.4f) max=(%.4f, %.4f, %.4f)\n", gmn.x, gmn.y, gmn.z, gmx.x, gmx.y, gmx.z);

    if (!r.defaultTransforms.empty()) {
        Point3F wmn{1e9f,1e9f,1e9f}, wmx{-1e9f,-1e9f,-1e9f};
        for (size_t mi = 0; mi < r.meshes.size(); mi++) {
            auto& mesh = r.meshes[mi];
            int ni = mesh.nodeIndex;
            if (ni < 0 || ni >= (int)r.defaultTransforms.size()) continue;
            auto& nwt = r.defaultTransforms[ni];
            int step = std::max(1, (int)mesh.vertices.size() / 8);
            for (size_t vi = 0; vi < mesh.vertices.size(); vi += step) {
                Point3F wp = nwt.transform(mesh.vertices[vi].pos);
                if (wp.x < wmn.x) wmn.x = wp.x; if (wp.y < wmn.y) wmn.y = wp.y; if (wp.z < wmn.z) wmn.z = wp.z;
                if (wp.x > wmx.x) wmx.x = wp.x; if (wp.y > wmx.y) wmx.y = wp.y; if (wp.z > wmx.z) wmx.z = wp.z;
            }
        }
        printf("  world:     min=(%.4f, %.4f, %.4f) max=(%.4f, %.4f, %.4f)\n", wmn.x, wmn.y, wmn.z, wmx.x, wmx.y, wmx.z);
    }

    if (!r.animations.empty()) {
        printf("\n--- animations (names/durations; keyframes NOT parsed) ---\n");
        for (auto& a : r.animations)
            printf("  %s (%.2fs) looping=%d\n", a.name.c_str(), a.duration, (int)a.looping);
    }
    return r.loaded ? 0 : 1;
}
