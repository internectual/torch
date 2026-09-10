#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include "render/dts_loader.h"
#include "render/renderer.h"
#include "core/engine.h"
#include "core/math.h"
#include "fs/file_system.h"
#include "fs/vl2_archive.h"

static FileSystem g_fs;
static Vl2Archive g_vl2;

int main() {
    Engine::instance().filesys = &g_fs;
    g_vl2.open("/home/methodown/t2-linux/base/shapes.vl2");
    g_fs.addArchive(&g_vl2);

    auto data = g_fs.read("shapes/bioderm_light.dts");
    if (data.empty()) { fprintf(stderr, "CANT READ bioderm\n"); return 1; }

    fprintf(stderr, "=== bioderm_light.dts (%zu bytes) ===\n", data.size());

    DTSLoadResult r = loadDTS(data.data(), data.size(), "bioderm_light");
    if (!r.loaded) { fprintf(stderr, "FAILED TO LOAD\n"); return 1; }

    fprintf(stderr, "\nNodes (%zu):\n", r.nodes.size());
    for (size_t i = 0; i < r.nodes.size(); i++) {
        fprintf(stderr, "  node[%zu] name='%s' parent=%d\n", i, r.nodes[i].name.c_str(),
                r.nodes[i].parentIndex);
    }

    fprintf(stderr, "\ndefaultLocalTransforms (%zu):\n", r.defaultLocalTransforms.size());
    for (size_t i = 0; i < r.defaultLocalTransforms.size(); i++) {
        MatrixF& m = r.defaultLocalTransforms[i];
        fprintf(stderr, "  local[%zu]: R=[(%6.3f %6.3f %6.3f) (%6.3f %6.3f %6.3f) (%6.3f %6.3f %6.3f)] t=(%.3f %.3f %.3f)\n",
            i, m.m[0][0], m.m[0][1], m.m[0][2],
               m.m[1][0], m.m[1][1], m.m[1][2],
               m.m[2][0], m.m[2][1], m.m[2][2],
               m.m[0][3], m.m[1][3], m.m[2][3]);
    }

    fprintf(stderr, "\ndefaultTransforms (%zu):\n", r.defaultTransforms.size());
    for (size_t i = 0; i < r.defaultTransforms.size(); i++) {
        MatrixF& m = r.defaultTransforms[i];
        fprintf(stderr, "  world[%zu]: t=(%.3f %.3f %.3f) R=[(%6.3f %6.3f %6.3f) (%6.3f %6.3f %6.3f) (%6.3f %6.3f %6.3f)]\n",
            i, m.m[0][3], m.m[1][3], m.m[2][3],
            m.m[0][0], m.m[0][1], m.m[0][2],
            m.m[1][0], m.m[1][1], m.m[1][2],
            m.m[2][0], m.m[2][1], m.m[2][2]);
    }

    fprintf(stderr, "\nAnimations (%zu):\n", r.animations.size());
    for (size_t ai = 0; ai < r.animations.size(); ai++) {
        auto& a = r.animations[ai];
        fprintf(stderr, "  anim[%zu] '%s' duration=%.3f looping=%d keyframes=%zu\n",
            ai, a.name.c_str(), a.duration, a.looping, a.keyframes.size());
    }

    // Dump skin data
    int skinCount = 0;
    for (size_t mi = 0; mi < r.meshes.size() && mi < r.skins.size(); mi++) {
        if (r.skins[mi].hasSkin) {
            skinCount++;
            if (skinCount <= 3) {
                fprintf(stderr, "\nSkin mesh[%zu]: %zu verts, %zu bones\n",
                    mi, r.skins[mi].initialPositions.size(), r.skins[mi].initialTransforms.size());
                for (size_t b = 0; b < r.skins[mi].initialTransforms.size() && b < 5; b++) {
                    MatrixF& m = r.skins[mi].initialTransforms[b];
                    fprintf(stderr, "  bone[%zu] transform: t=(%.3f %.3f %.3f)\n",
                        b, m.m[0][3], m.m[1][3], m.m[2][3]);
                    fprintf(stderr, "    R=[(%6.3f %6.3f %6.3f) (%6.3f %6.3f %6.3f) (%6.3f %6.3f %6.3f)]\n",
                        m.m[0][0], m.m[0][1], m.m[0][2],
                        m.m[1][0], m.m[1][1], m.m[1][2],
                        m.m[2][0], m.m[2][1], m.m[2][2]);
                }
            }
        }
    }
    fprintf(stderr, "\nTotal skinned meshes: %d\n", skinCount);

    // Check first few mesh vertices
    for (size_t mi = 0; mi < r.meshes.size() && mi < 5; mi++) {
        fprintf(stderr, "\nMesh[%zu]: %zu verts, nodeIndex=%d, materialIdx=%d\n",
            mi, r.meshes[mi].vertices.size(), r.meshes[mi].nodeIndex, r.meshes[mi].materialIndex);
        for (size_t vi = 0; vi < r.meshes[mi].vertices.size() && vi < 5; vi++) {
            auto& v = r.meshes[mi].vertices[vi];
            fprintf(stderr, "  vert[%zu]: pos=(%.3f %.3f %.3f)\n", vi, v.pos.x, v.pos.y, v.pos.z);
        }
    }

    return 0;
}
