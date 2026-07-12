// Headless stubs for the GL-touching methods the loaders call. We deliberately
// do NOT link renderer.cpp/mesh.cpp/texture.cpp (which need a GL context);
// instead we provide no-op versions of the methods invoked by the loaders
// (MeshData::upload and the various Texture::load* entry points), so the parse
// paths run fully without any GPU.

#include "render/renderer.h"
#include <cstring>

void MeshData::upload() {}
void MeshData::render() {}
void MeshData::destroy() {}
void MeshData::updateGPU() {}

void Texture::load(const uint8_t* /*data*/, size_t /*size*/) {}
bool Texture::loadBM8(const uint8_t* /*data*/, size_t /*size*/) { return false; }
void Texture::loadRaw(const uint8_t* /*pixels*/, int32_t /*w*/, int32_t /*h*/, int32_t /*channels*/) {}
void Texture::bind(int32_t /*unit*/) {}
void Texture::destroy() {}
bool Texture::decodeBM8(const uint8_t* data, size_t size,
                        std::vector<uint8_t>& outPixels,
                        int32_t& outW, int32_t& outH) {
    if (size < 32) return false;
    uint32_t hdr[8];
    memcpy(hdr, data, 32);
    uint32_t w = hdr[1], h = hdr[2], flags = hdr[4];
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return false;
    if (flags != 1 && flags < 3) return false;

    uint32_t paletteSize;
    if (flags == 1) paletteSize = 1024;
    else if (flags >= 3 && flags <= 257) paletteSize = 1024 + (flags - 1) * 4;
    else return false;
    if (size < 32 + paletteSize) return false;
    uint32_t pixelOffset = 32 + paletteSize;
    uint32_t pixelCount = w * h;
    if (pixelOffset + pixelCount > size) return false;

    uint8_t palette[1024];
    memcpy(palette, data + 32, paletteSize > 1024 ? 1024 : paletteSize);

    outPixels.resize(pixelCount * 4);
    for (uint32_t i = 0; i < pixelCount; i++) {
        uint8_t idx = data[pixelOffset + i];
        outPixels[i * 4 + 0] = palette[idx * 4 + 0];
        outPixels[i * 4 + 1] = palette[idx * 4 + 1];
        outPixels[i * 4 + 2] = palette[idx * 4 + 2];
        outPixels[i * 4 + 3] = palette[idx * 4 + 3];
    }
    outW = (int32_t)w;
    outH = (int32_t)h;
    return true;
}
