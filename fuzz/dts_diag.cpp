// DTS diagnostic: dump vertex bounds, node transforms, and per-mesh summary
// to diagnose jumbled rendering.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

#include "render/dts_loader.h"
#include "core/engine.h"
#include "fs/file_system.h"

static FileSystem g_fs;

static void printBounds(const std::vector<MeshData>& meshes, const char* label) {
    float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
    for (auto& m : meshes)
        for (auto& v : m.vertices) {
            if (v.pos.x < mn[0]) mn[0] = v.pos.x;
            if (v.pos.y < mn[1]) mn[1] = v.pos.y;
            if (v.pos.z < mn[2]) mn[2] = v.pos.z;
            if (v.pos.x > mx[0]) mx[0] = v.pos.x;
            if (v.pos.y > mx[1]) mx[1] = v.pos.y;
            if (v.pos.z > mx[2]) mx[2] = v.pos.z;
        }
    fprintf(stderr, "%s bounds: (%.3f,%.3f,%.3f) to (%.3f,%.3f,%.3f)\n", label,
            mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
    fprintf(stderr, "  size: %.3f x %.3f x %.3f\n",
            mx[0]-mn[0], mx[1]-mn[1], mx[2]-mn[2]);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.dts>\n", argv[0]); return 2; }
    Engine::instance().filesys = &g_fs;

    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(sz);
    fread(data.data(), 1, sz, f);
    fclose(f);

    DTSLoadResult r = loadDTS(data.data(), data.size(), argv[1]);
    fprintf(stderr, "loaded=%d meshes=%zu nodes=%zu transforms=%zu anims=%zu\n",
            r.loaded, r.meshes.size(), r.nodes.size(),
            r.defaultTransforms.size(), r.animations.size());

    printBounds(r.meshes, "local verts");

    // Compose world-space bounds using defaultTransforms
    float wmn[3] = {1e9f, 1e9f, 1e9f}, wmx[3] = {-1e9f, -1e9f, -1e9f};
    for (size_t mi = 0; mi < r.meshes.size(); mi++) {
        int ni = r.meshes[mi].nodeIndex;
        MatrixF M = (ni >= 0 && ni < (int)r.defaultTransforms.size())
                    ? r.defaultTransforms[ni] : MatrixF();
        for (auto& v : r.meshes[mi].vertices) {
            Point3F wp = M.transform(v.pos);
            if (wp.x < wmn[0]) wmn[0] = wp.x;
            if (wp.y < wmn[1]) wmn[1] = wp.y;
            if (wp.z < wmn[2]) wmn[2] = wp.z;
            if (wp.x > wmx[0]) wmx[0] = wp.x;
            if (wp.y > wmx[1]) wmx[1] = wp.y;
            if (wp.z > wmx[2]) wmx[2] = wp.z;
        }
    }
    fprintf(stderr, "world bounds: (%.3f,%.3f,%.3f) to (%.3f,%.3f,%.3f)\n",
            wmn[0], wmn[1], wmn[2], wmx[0], wmx[1], wmx[2]);
    fprintf(stderr, "  world size: %.3f x %.3f x %.3f\n",
            wmx[0]-wmn[0], wmx[1]-wmn[1], wmx[2]-wmn[2]);

    // Dump per-node transform (translation component only for brevity)
    fprintf(stderr, "\n--- node transforms (translation) ---\n");
    for (size_t i = 0; i < r.defaultTransforms.size() && i < r.nodes.size(); i++) {
        auto& M = r.defaultTransforms[i];
        fprintf(stderr, "  node[%zu] parent=%-3d '%s' trans=(%.3f,%.3f,%.3f)\n",
                i, r.nodes[i].parentIndex, r.nodes[i].name.c_str(),
                M.m[3][0], M.m[3][1], M.m[3][2]);
    }

    // Dump first 5 verts of first 3 meshes
    fprintf(stderr, "\n--- first verts per mesh ---\n");
    for (size_t mi = 0; mi < std::min<size_t>(3, r.meshes.size()); mi++) {
        fprintf(stderr, "  mesh[%zu] node=%d prims=%zu:\n",
                mi, r.meshes[mi].nodeIndex, r.meshes[mi].indices.size()/3);
        for (size_t vi = 0; vi < std::min<size_t>(5, r.meshes[mi].vertices.size()); vi++) {
            auto& v = r.meshes[mi].vertices[vi];
            fprintf(stderr, "    v[%zu] pos=(%.4f,%.4f,%.4f) nrm=(%.4f,%.4f,%.4f) uv=(%.4f,%.4f)\n",
                    vi, v.pos.x, v.pos.y, v.pos.z, v.normal.x, v.normal.y, v.normal.z, v.uv.x, v.uv.y);
        }
    }

    // Check for NaN/Inf in positions
    int badCount = 0;
    for (size_t mi = 0; mi < r.meshes.size(); mi++) {
        for (auto& v : r.meshes[mi].vertices) {
            if (!std::isfinite(v.pos.x) || !std::isfinite(v.pos.y) || !std::isfinite(v.pos.z)) {
                badCount++;
                if (badCount <= 5)
                    fprintf(stderr, "BAD: mesh[%zu] NaN/Inf pos=(%f,%f,%f)\n",
                            mi, v.pos.x, v.pos.y, v.pos.z);
            }
        }
    }
    fprintf(stderr, "NaN/Inf positions: %d\n", badCount);

    return 0;
}
