#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
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

    fprintf(stderr, "DTS size: %zu bytes\n", data.size());

    DTSLoadResult r = loadDTS(data.data(), data.size(), "bioderm_medium.dts");
    fprintf(stderr, "Loaded: %d meshes=%zu skins=%zu nodes=%zu\n", r.loaded, r.meshes.size(), r.skins.size(), r.nodes.size());

    // Print info about first 30 meshes
    for (size_t mi = 0; mi < std::min(r.meshes.size(), (size_t)30); mi++) {
        auto& mesh = r.meshes[mi];
        auto& skin = r.skins[mi];
        fprintf(stderr, "Mesh[%zu]: verts=%zu idx=%zu nodeIdx=%d hasSkin=%d initPos=%zu initXform=%zu vertIdx=%zu boneIdx=%zu\n",
                mi, mesh.vertices.size(), mesh.indices.size(), mesh.nodeIndex,
                skin.hasSkin, skin.initialPositions.size(), skin.initialTransforms.size(),
                skin.vertexIndices.size(), skin.boneIndices.size());
    }

    // Count skinned meshes
    int skinned = 0;
    for (size_t mi = 0; mi < r.meshes.size() && mi < r.skins.size(); mi++) {
        if (r.skins[mi].hasSkin) skinned++;
    }
    fprintf(stderr, "Total skinned: %d / %zu\n", skinned, r.meshes.size());

    // Check nodes
    for (size_t ni = 0; ni < std::min(r.nodes.size(), (size_t)31); ni++) {
        fprintf(stderr, "Node[%zu]: name='%s' parent=%d\n", ni, r.nodes[ni].name.c_str(), r.nodes[ni].parentIndex);
    }

    return 0;
}
