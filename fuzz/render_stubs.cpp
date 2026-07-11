// Headless stubs for the GL-touching methods the loaders call. We deliberately
// do NOT link renderer.cpp/mesh.cpp/texture.cpp (which need a GL context);
// instead we provide no-op versions of the methods invoked by the loaders
// (MeshData::upload and the various Texture::load* entry points), so the parse
// paths run fully without any GPU.

#include "render/renderer.h"

void MeshData::upload() {}
void MeshData::render() {}
void MeshData::destroy() {}
void MeshData::updateGPU() {}

void Texture::load(const uint8_t* /*data*/, size_t /*size*/) {}
bool Texture::loadBM8(const uint8_t* /*data*/, size_t /*size*/) { return false; }
void Texture::loadRaw(const uint8_t* /*pixels*/, int32_t /*w*/, int32_t /*h*/, int32_t /*channels*/) {}
void Texture::bind(int32_t /*unit*/) {}
void Texture::destroy() {}
bool Texture::decodeBM8(const uint8_t* /*data*/, size_t /*size*/,
                        std::vector<uint8_t>& /*outPixels*/,
                        int32_t& /*outW*/, int32_t& /*outH*/) { return false; }
