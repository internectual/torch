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

    int loadOk = 0, loadFail = 0;
    for (auto& path : dtsFiles) {
        auto data = g_fs.read(path.c_str());
        if (data.empty()) { loadFail++; continue; }
        DTSLoadResult r = loadDTS(data.data(), data.size(), path.c_str());
        if (r.loaded) {
            loadOk++;
            // Simulate render: check materialIndex against materialFlags
            for (size_t i = 0; i < r.meshes.size(); i++) {
                auto& mesh = r.meshes[i];
                // This is what render() does:
                if (mesh.materialIndex >= 0 && mesh.materialIndex < (int)r.textures.size()) {
                    if (mesh.materialIndex < (int)r.materialFlags.size()) {
                        volatile uint32_t f = r.materialFlags[mesh.materialIndex];
                        (void)f;
                    }
                }
            }
        } else {
            loadFail++;
        }
    }
    fprintf(stderr, "Loaded: %d  Failed: %d\n", loadOk, loadFail);
    return 0;
}
