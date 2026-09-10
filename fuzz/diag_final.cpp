#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <algorithm>
#include "render/dts_loader.h"
#include "core/engine.h"
#include "core/math.h"
#include "fs/file_system.h"
#include "fs/vl2_archive.h"

static FileSystem g_fs;
static Vl2Archive g_vl2;

int main() {
    Engine::instance().filesys = &g_fs;
    const char* t2data = getenv("TORCH_T2DATA");
    std::string basePath = t2data ? std::string(t2data) : "base";
    g_vl2.open((basePath + "/shapes.vl2").c_str());
    g_fs.addArchive(&g_vl2);

    auto data = g_fs.read("shapes/bioderm_medium.dts");
    if (data.empty()) { fprintf(stderr, "Failed to read\n"); return 1; }

    DTSLoadResult r = loadDTS(data.data(), data.size(), "bioderm_medium.dts");
    fprintf(stderr, "Loaded: meshes=%zu skins=%zu nodes=%zu\n", r.meshes.size(), r.skins.size(), r.nodes.size());

    // Check defaultTransforms
    fprintf(stderr, "defaultTransforms: %zu\n", r.defaultTransforms.size());
    for (size_t i = 0; i < std::min(r.defaultTransforms.size(), (size_t)5); i++) {
        const MatrixF& m = r.defaultTransforms[i];
        fprintf(stderr, "  node[%zu]: [%8.3f %8.3f %8.3f %8.3f]\n"
                        "            [%8.3f %8.3f %8.3f %8.3f]\n"
                        "            [%8.3f %8.3f %8.3f %8.3f]\n"
                        "            [%8.3f %8.3f %8.3f %8.3f]\n",
            i,
            m.m[0][0], m.m[0][1], m.m[0][2], m.m[0][3],
            m.m[1][0], m.m[1][1], m.m[1][2], m.m[1][3],
            m.m[2][0], m.m[2][1], m.m[2][2], m.m[2][3],
            m.m[3][0], m.m[3][1], m.m[3][2], m.m[3][3]);
    }

    // Check mesh node assignments
    fprintf(stderr, "\nMesh node assignments:\n");
    for (size_t mi = 0; mi < std::min(r.meshes.size(), (size_t)30); mi++) {
        fprintf(stderr, "  Mesh[%zu]: verts=%zu nodeIdx=%d\n", mi,
                r.meshes[mi].vertices.size(), r.meshes[mi].nodeIndex);
    }

    // Check if any meshes have skin data
    int skinned = 0;
    for (size_t mi = 0; mi < r.skins.size(); mi++) {
        if (r.skins[mi].hasSkin) skinned++;
    }
    fprintf(stderr, "\nSkinned meshes: %d / %zu\n", skinned, r.skins.size());

    return 0;
}
