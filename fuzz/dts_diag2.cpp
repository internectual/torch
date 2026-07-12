// Diagnostic: load a DTS from VL2 archive and dump node transform + mesh info
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <cmath>

#include "render/dts_loader.h"
#include "core/engine.h"
#include "fs/file_system.h"
#include "fs/vl2_archive.h"

static FileSystem g_fs;
static Vl2Archive g_vl2;

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <shapes/bioderm_light.dts>\n", argv[0]); return 2; }
    Engine::instance().filesys = &g_fs;

    // Mount the shapes VL2
    g_vl2.open("/home/methodown/t2-linux/base/shapes.vl2");
    g_fs.addArchive(&g_vl2);

    auto data = g_fs.read(argv[1]);
    if (data.empty()) { fprintf(stderr, "Cannot read %s\n", argv[1]); return 2; }

    DTSLoadResult r = loadDTS(data.data(), data.size(), argv[1]);

    printf("=== loadDTS('%s') ===\n", argv[1]);
    printf("loaded            : %s\n", r.loaded ? "YES" : "NO");
    printf("meshes            : %zu\n", r.meshes.size());
    printf("nodes             : %zu\n", r.nodes.size());
    printf("defaultTransforms : %zu\n", r.defaultTransforms.size());
    printf("skins             : %zu\n", r.skins.size());
    printf("animations        : %zu\n", r.animations.size());

    int hasSkinCount = 0, noNodeCount = 0;
    for (size_t i = 0; i < r.meshes.size(); i++) {
        bool hs = (i < r.skins.size()) ? r.skins[i].hasSkin : false;
        if (hs) hasSkinCount++;
        if (r.meshes[i].nodeIndex < 0) noNodeCount++;
    }
    printf("skin meshes       : %d\n", hasSkinCount);
    printf("meshes with no node: %d\n", noNodeCount);

    printf("\n--- nodes (first 10) ---\n");
    for (size_t i = 0; i < std::min(r.nodes.size(), (size_t)10); i++)
        printf("  node[%zu] parent=%-3d name='%s'\n", i, r.nodes[i].parentIndex, r.nodes[i].name.c_str());

    if (!r.defaultTransforms.empty()) {
        printf("\n--- defaultTransforms (first 5) ---\n");
        size_t n = std::min(r.defaultTransforms.size(), (size_t)5);
        for (size_t i = 0; i < n; i++) {
            auto& m = r.defaultTransforms[i];
            printf("  nodeWorld[%zu]: [%+.6f %+.6f %+.6f %+.6f]\n", i, m.m[0][0], m.m[0][1], m.m[0][2], m.m[0][3]);
            printf("                 [%+.6f %+.6f %+.6f %+.6f]\n",   m.m[1][0], m.m[1][1], m.m[1][2], m.m[1][3]);
            printf("                 [%+.6f %+.6f %+.6f %+.6f]\n",   m.m[2][0], m.m[2][1], m.m[2][2], m.m[2][3]);
            printf("                 [%+.6f %+.6f %+.6f %+.6f]\n",   m.m[3][0], m.m[3][1], m.m[3][2], m.m[3][3]);
        }
    }

    printf("\n--- vertex bounds ---\n");
    Point3F gmn{1e9f,1e9f,1e9f}, gmx{-1e9f,-1e9f,-1e9f};
    for (size_t mi = 0; mi < r.meshes.size(); mi++) {
        auto& mesh = r.meshes[mi];
        for (auto& v : mesh.vertices) {
            if (v.pos.x < gmn.x) gmn.x = v.pos.x; if (v.pos.y < gmn.y) gmn.y = v.pos.y; if (v.pos.z < gmn.z) gmn.z = v.pos.z;
            if (v.pos.x > gmx.x) gmx.x = v.pos.x; if (v.pos.y > gmx.y) gmx.y = v.pos.y; if (v.pos.z > gmx.z) gmx.z = v.pos.z;
        }
    }
    printf("  local: min=(%.4f, %.4f, %.4f) max=(%.4f, %.4f, %.4f)\n", gmn.x, gmn.y, gmn.z, gmx.x, gmx.y, gmx.z);

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
        printf("  world: min=(%.4f, %.4f, %.4f) max=(%.4f, %.4f, %.4f)\n", wmn.x, wmn.y, wmn.z, wmx.x, wmx.y, wmx.z);
        float dx = wmx.x-wmn.x, dy = wmx.y-wmn.y, dz = wmx.z-wmn.z;
        float radius = 0.5f * std::sqrt(dx*dx+dy*dy+dz*dz);
        printf("  center=(%.4f, %.4f, %.4f) radius=%.4f\n", (wmn.x+wmx.x)*0.5f, (wmn.y+wmx.y)*0.5f, (wmn.z+wmx.z)*0.5f, radius);
    }

    // Check mesh geometry
    printf("\n--- mesh geometry (first 30) ---\n");
    int emptyCount = 0, totalTris = 0;
    for (size_t i = 0; i < std::min(r.meshes.size(), (size_t)30); i++) {
        int tris = (int)r.meshes[i].indices.size() / 3;
        totalTris += tris;
        printf("  mesh[%zu] nodeIdx=%-3d verts=%-4zu tris=%-4d\n",
               i, r.meshes[i].nodeIndex, r.meshes[i].vertices.size(), tris);
        if (tris == 0) emptyCount++;
    }
    int allEmpty = 0, allTris = 0;
    for (size_t i = 0; i < r.meshes.size(); i++) {
        allTris += (int)r.meshes[i].indices.size() / 3;
        if (r.meshes[i].indices.empty()) allEmpty++;
    }
    printf("  ... total meshes=%zu empty=%d with_tris=%d total_tris=%d\n",
           r.meshes.size(), allEmpty, (int)r.meshes.size() - allEmpty, allTris);

    return r.loaded ? 0 : 1;
}
