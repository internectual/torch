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

    int loaded = 0, empty = 0, failed = 0, lowver = 0;
    for (auto& path : dtsFiles) {
        auto data = g_fs.read(path.c_str());
        if (data.empty()) { failed++; continue; }
        uint16_t ver = *(const uint16_t*)data.data();
        if (ver < 15) { lowver++; continue; }
        if (ver > 30) { failed++; continue; }
        fprintf(stderr, "  %s (v%u, %zu bytes)... ", path.c_str(), ver, data.size());
        fflush(stderr);
        DTSLoadResult r = loadDTS(data.data(), data.size(), path.c_str());
        if (r.loaded && !r.meshes.empty()) {
            loaded++;
            fprintf(stderr, "OK (%zu meshes)\n", r.meshes.size());
        } else if (r.loaded && r.meshes.empty()) {
            empty++;
            fprintf(stderr, "EMPTY (%zu nodes)\n", r.nodes.size());
        } else {
            failed++;
            fprintf(stderr, "FAIL\n");
        }
    }
    fprintf(stderr, "\n=== Results: %d loaded, %d empty-mesh, %d failed, %d low-version ===\n", loaded, empty, failed, lowver);
    return 0;
}
