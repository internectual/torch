// Diagnostic: check whether defaultTransforms and initialTransforms
// are in the same coordinate space, by testing if
// defaultTransforms[nodeIdx] * initialTransforms[boneIdx] ~= identity
// at bind pose.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <cmath>
#include "render/dts_loader.h"
#include "core/engine.h"
#include "core/math.h"
#include "fs/file_system.h"
#include "fs/vl2_archive.h"

static FileSystem g_fs;
static Vl2Archive g_vl2_shapes, g_vl2_base;

int main() {
    Engine::instance().filesys = &g_fs;
    const char* t2data = getenv("TORCH_T2DATA");
    std::string basePath = t2data ? std::string(t2data) : "base";
    g_vl2_shapes.open((basePath + "/shapes.vl2").c_str());
    g_vl2_base.open((basePath + "/base.vl2").c_str());
    g_fs.addArchive(&g_vl2_shapes);
    g_fs.addArchive(&g_vl2_base);

    const char* target = getenv("SV_START");
    std::string targetFile = target ? std::string(target) : "bioderm";

    std::vector<std::string> files;
    g_fs.listFiles(nullptr, files);

    std::vector<std::string> dtsFiles;
    for (auto& f : files) {
        if (f.size() > 4 && f.rfind(".dts") == f.size() - 4)
            dtsFiles.push_back(f);
    }

    // Find the first matching dts
    std::string dtsPath;
    for (auto& f : dtsFiles) {
        if (f.find(targetFile) != std::string::npos) {
            dtsPath = f;
            break;
        }
    }
    if (dtsPath.empty()) {
        fprintf(stderr, "No DTS file found containing '%s'\n", targetFile.c_str());
        return 1;
    }

    auto data = g_fs.read(dtsPath.c_str());
    if (data.empty()) {
        fprintf(stderr, "Failed to read %s\n", dtsPath.c_str());
        return 1;
    }

    DTSLoadResult r = loadDTS(data.data(), data.size(), dtsPath.c_str());
    if (!r.loaded) {
        fprintf(stderr, "Failed to load %s\n", dtsPath.c_str());
        return 1;
    }

    fprintf(stderr, "Loaded %s: %zu meshes, %zu skins, %zu nodes, %zu animations\n",
            dtsPath.c_str(), r.meshes.size(), r.skins.size(),
            r.nodes.size(), r.animations.size());

    // Check skin data
    int skinCount = 0;
    for (size_t mi = 0; mi < r.meshes.size() && mi < r.skins.size(); mi++) {
        auto& skin = r.skins[mi];
        if (!skin.hasSkin) continue;
        skinCount++;
        fprintf(stderr, "\nMesh[%zu] has skin: %zu vertex entries, %zu bone transforms\n",
                mi, skin.vertexIndices.size(), skin.initialTransforms.size());

        // Print first few bone/node index pairs
        int showN = std::min((size_t)5, skin.vertexIndices.size());
        for (int i = 0; i < showN; i++) {
            int32_t vertIdx = skin.vertexIndices[i];
            int32_t boneIdx = (i < (int)skin.boneIndices.size()) ? skin.boneIndices[i] : 0;
            int32_t nodeIdx = (i < (int)skin.nodeIndices.size()) ? skin.nodeIndices[i] : 0;
            float weight = (i < (int)skin.boneWeights.size()) ? skin.boneWeights[i] : 1.0f;
            Point3F bindPos = skin.initialPositions[i];
            fprintf(stderr, "  vert[%d]: vertIdx=%d boneIdx=%d nodeIdx=%d weight=%.3f bindPos=(%.2f %.2f %.2f)\n",
                    i, vertIdx, boneIdx, nodeIdx, weight, bindPos.x, bindPos.y, bindPos.z);

            // Check: defaultTransforms[nodeIdx] * initialTransforms[boneIdx]
            if (nodeIdx >= 0 && nodeIdx < (int)r.defaultTransforms.size() &&
                boneIdx >= 0 && boneIdx < (int)skin.initialTransforms.size()) {
                MatrixF bind = r.defaultTransforms[nodeIdx];
                MatrixF inv = skin.initialTransforms[boneIdx];
                MatrixF product = bind * inv;
                // Check if product is approximately identity
                float dev = 0;
                for (int i = 0; i < 4; i++)
                    for (int j = 0; j < 4; j++) {
                        float expected = (i == j) ? 1.0f : 0.0f;
                        dev += fabsf(product.m[i][j] - expected);
                    }
                fprintf(stderr, "    bind*inv deviation from identity: %.4f\n", dev);
                if (dev > 0.1f) {
                    fprintf(stderr, "    bind transform:\n");
                    for (int i = 0; i < 4; i++)
                        fprintf(stderr, "      [%.3f %.3f %.3f %.3f]\n",
                                bind.m[i][0], bind.m[i][1], bind.m[i][2], bind.m[i][3]);
                    fprintf(stderr, "    inv transform:\n");
                    for (int i = 0; i < 4; i++)
                        fprintf(stderr, "      [%.3f %.3f %.3f %.3f]\n",
                                inv.m[i][0], inv.m[i][1], inv.m[i][2], inv.m[i][3]);
                    fprintf(stderr, "    product:\n");
                    for (int i = 0; i < 4; i++)
                        fprintf(stderr, "      [%.3f %.3f %.3f %.3f]\n",
                                product.m[i][0], product.m[i][1], product.m[i][2], product.m[i][3]);
                }
            }
        }
    }

    if (skinCount == 0) {
        fprintf(stderr, "No skinned meshes found\n");
    }

    return 0;
}
