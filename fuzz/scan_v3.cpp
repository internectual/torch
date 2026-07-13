#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include "render/dts_loader.h"
#include "core/engine.h"
#include "fs/file_system.h"
#include "fs/vl2_archive.h"

static FileSystem g_fs;
static Vl2Archive g_vl2_shapes, g_vl2_base;

int main() {
    Engine::instance().filesys = &g_fs;
    g_vl2_shapes.open("/home/methodown/t2-linux/base/shapes.vl2");
    g_vl2_base.open("/home/methodown/t2-linux/base/base.vl2");
    static Vl2Archive g_vl2_skins;
    g_vl2_skins.open("/home/methodown/t2-linux/base/skins.vl2");
    g_fs.addArchive(&g_vl2_shapes);
    g_fs.addArchive(&g_vl2_base);
    g_fs.addArchive(&g_vl2_skins);

    std::vector<std::string> files;
    g_fs.listFiles(nullptr, files);
    
    std::vector<std::string> dtsFiles;
    for (auto& f : files) {
        if (f.size() > 4 && f.rfind(".dts") == f.size() - 4)
            dtsFiles.push_back(f);
    }
    fprintf(stderr, "Found %zu .dts files\n", dtsFiles.size());

    int loaded = 0, empty = 0, failed = 0, lowver = 0, highver = 0;
    for (auto& path : dtsFiles) {
        auto data = g_fs.read(path.c_str());
        if (data.empty()) { failed++; fprintf(stderr, "READ_FAIL: %s\n", path.c_str()); continue; }
        uint16_t ver = *(const uint16_t*)data.data();
        if (ver < 15) { lowver++; fprintf(stderr, "LOW_VER: %s (v%u)\n", path.c_str(), ver); continue; }
        if (ver > 30) { highver++; fprintf(stderr, "HIGH_VER: %s (v%u, %zu bytes)\n", path.c_str(), ver, data.size()); continue; }
        fprintf(stderr, "  %s (v%u, %zu bytes)... ", path.c_str(), ver, data.size());
        fflush(stderr);
        DTSLoadResult r = loadDTS(data.data(), data.size(), path.c_str());
        if (r.loaded && !r.meshes.empty()) {
            loaded++;
            int skinned = 0;
            for (auto& s : r.skins) if (s.hasSkin) skinned++;
            int textured = 0;
            for (auto& m : r.meshes) if (m.materialIndex >= 0) textured++;
            fprintf(stderr, "OK (%zu meshes, %zu textures, %d/%zu textured",
                    r.meshes.size(), r.textures.size(), textured, r.meshes.size());
            if (!r.animations.empty()) fprintf(stderr, ", %zu anims", r.animations.size());
            if (skinned > 0) fprintf(stderr, ", %d skinned", skinned);
            if (!r.materialNames.empty()) fprintf(stderr, ", %zu materials", r.materialNames.size());
            fprintf(stderr, ")\n");
            if (textured < (int)r.meshes.size() && !r.materialNames.empty()) {
                fprintf(stderr, "    materialNames:");
                for (auto& n : r.materialNames) fprintf(stderr, " '%s'", n.c_str());
                fprintf(stderr, "\n");
            }
            // Dump animation details for first shape with anims
            if (!r.animations.empty()) {
                // Also dump node hierarchy and default transforms for weapon_disc
                std::string pname = path;
                for (auto& c : pname) c = (char)std::tolower((unsigned char)c);
                bool detailed = (pname.find("weapon_disc") != std::string::npos);
                if (detailed) {
                    fprintf(stderr, "    NODES (%zu):\n", r.nodes.size());
                    for (size_t ni = 0; ni < r.nodes.size(); ni++) {
                        auto& n = r.nodes[ni];
                        fprintf(stderr, "      [%zu] parent=%d name='%s'\n", ni, n.parentIndex, n.name.c_str());
                    }
                    fprintf(stderr, "    DEFAULT LOCAL TRANSFORMS (%zu):\n", r.defaultLocalTransforms.size());
                    for (size_t ni = 0; ni < std::min<size_t>(15, r.defaultLocalTransforms.size()); ni++) {
                        auto& m = r.defaultLocalTransforms[ni];
                        fprintf(stderr, "      [%zu] T=(%.3f,%.3f,%.3f) R0=(%.3f,%.3f,%.3f) R1=(%.3f,%.3f,%.3f)\n",
                                ni, m.m[0][3], m.m[1][3], m.m[2][3],
                                m.m[0][0], m.m[0][1], m.m[0][2],
                                m.m[1][0], m.m[1][1], m.m[1][2]);
                    }
                }
                for (size_t ai = 0; ai < r.animations.size(); ai++) {
                    auto& a = r.animations[ai];
                    fprintf(stderr, "    anim[%zu] '%s' dur=%.3f loop=%d kf=%zu\n",
                            ai, a.name.c_str(), a.duration, a.looping, a.keyframes.size());
                    for (size_t ki = 0; ki < std::min<size_t>(3, a.keyframes.size()); ki++) {
                        auto& kf = a.keyframes[ki];
                        fprintf(stderr, "      kf t=%.3f node=%d trans=(%.4f,%.4f,%.4f) rot=(%.4f,%.4f,%.4f,%.4f)\n",
                                kf.time, kf.nodeIndex,
                                kf.translation.x, kf.translation.y, kf.translation.z,
                                kf.rotation.x, kf.rotation.y, kf.rotation.z, kf.rotation.w);
                    }
                }
            }
        } else if (r.loaded && r.meshes.empty()) {
            empty++;
            fprintf(stderr, "EMPTY (%zu nodes)\n", r.nodes.size());
        } else {
            failed++;
            fprintf(stderr, "FAIL\n");
        }
    }
    fprintf(stderr, "\n=== Results: %d loaded, %d empty-mesh, %d failed, %d low-version, %d high-version ===\n", loaded, empty, failed, lowver, highver);
    return 0;
}
