#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
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

    auto data = g_fs.read("shapes/octahedron.dts");
    if (data.empty()) { fprintf(stderr, "not found\n"); return 1; }
    
    fprintf(stderr, "file size: %zu bytes\n", data.size());
    uint16_t ver = *(uint16_t*)data.data();
    uint16_t pad = *(uint16_t*)(data.data()+2);
    fprintf(stderr, "version=%u pad=%u\n", ver, pad);
    
    // Print first 200 bytes as hex dump
    for (size_t i = 0; i < std::min(data.size(), (size_t)200); i++) {
        if (i % 16 == 0) fprintf(stderr, "\n%4zu: ", i);
        fprintf(stderr, "%02x ", data[i]);
    }
    fprintf(stderr, "\n");
    
    return 0;
}
