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
    g_fs.addArchive(&g_vl2_shapes);
    g_fs.addArchive(&g_vl2_base);

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
            fprintf(stderr, "OK (%zu meshes", r.meshes.size());
            if (!r.animations.empty()) fprintf(stderr, ", %zu anims", r.animations.size());
            if (skinned > 0) fprintf(stderr, ", %d skinned", skinned);
            fprintf(stderr, ")\n");
            // Dump animation details for first shape with anims
            if (!r.animations.empty()) {
                for (size_t ai = 0; ai < r.animations.size(); ai++) {
                    auto& a = r.animations[ai];
                    fprintf(stderr, "    anim[%zu] '%s' dur=%.3f loop=%d kf=%zu\n",
                            ai, a.name.c_str(), a.duration, a.looping, a.keyframes.size());
                    for (size_t ki = 0; ki < std::min<size_t>(3, a.keyframes.size()); ki++) {
                        auto& kf = a.keyframes[ki];
                        fprintf(stderr, "      kf t=%.3f node=%d trans=(%.2f,%.2f,%.2f) rot=(%.2f,%.2f,%.2f,%.2f)\n",
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
