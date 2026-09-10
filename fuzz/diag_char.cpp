#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
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

    // Test multiple DTS files to find one with Skin meshes
    const char* files[] = {
        "shapes/bioderm_medium.dts",
        "shapes/bioderm_light.dts",
        "shapes/bioderm_heavy.dts",
        "shapes/soldier.dts",
        "shapes/soldier_light.dts",
        "shapes/soldier_medium.dts",
        "shapes/soldier_heavy.dts",
        "shapes/brute.dts",
        nullptr
    };

    for (int fi = 0; files[fi]; fi++) {
        auto data = g_fs.read(files[fi]);
        if (data.empty()) continue;
        DTSLoadResult r = loadDTS(data.data(), data.size(), files[fi]);
        if (!r.loaded) continue;
        
        // Count mesh types
        int type0=0, type1=0, type2=0, type3=0, type4=0, skinned=0;
        for (size_t mi = 0; mi < r.skins.size(); mi++) {
            if (r.skins[mi].hasSkin) skinned++;
        }
        fprintf(stderr, "%s: meshes=%zu skins=%zu skinned=%d nodes=%zu defXform=%zu\n",
                files[fi], r.meshes.size(), r.skins.size(), skinned, r.nodes.size(), r.defaultTransforms.size());
        
        // Print first 5 default transforms
        for (size_t ni = 0; ni < std::min(r.defaultTransforms.size(), (size_t)5); ni++) {
            const MatrixF& m = r.defaultTransforms[ni];
            fprintf(stderr, "  node[%zu]: rot=[%7.2f %7.2f %7.2f] trans=[%7.2f %7.2f %7.2f]\n", ni,
                m.m[0][0], m.m[1][0], m.m[2][0],
                m.m[0][3], m.m[1][3], m.m[2][3]);
        }
    }
    return 0;
}
