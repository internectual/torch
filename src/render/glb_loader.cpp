#include "render/glb_loader.h"
#include "core/console.h"
#include "fs/file_system.h"
#include "core/engine.h"
#include <GL/glew.h>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <cmath>

// ─── Minimal JSON parser ───────────────────────────────────────────

enum class JType { Null, Bool, Num, Str, Arr, Obj };

struct JVal {
    JType t = JType::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::map<std::string, JVal> o;

    const JVal& operator[](const std::string& k) const {
        static JVal nullVal;
        auto it = o.find(k);
        return it != o.end() ? it->second : nullVal;
    }
    const JVal& operator[](size_t i) const {
        static JVal nullVal;
        return i < a.size() ? a[i] : nullVal;
    }
    int64_t asInt() const { return (int64_t)n; }
    double asNum() const { return n; }
    const std::string& asStr() const { return s; }
    size_t size() const { return a.size(); }
};

static void skipWS(const char*& p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
}

static JVal parseVal(const char*& p);

static std::string parseStr(const char*& p) {
    std::string r;
    if (*p != '"') return r;
    ++p;
    while (*p && *p != '"') {
        if (*p == '\\') {
            ++p;
            switch (*p) {
                case '"': r += '"'; break;
                case '\\': r += '\\'; break;
                case '/': r += '/'; break;
                case 'b': r += '\b'; break;
                case 'f': r += '\f'; break;
                case 'n': r += '\n'; break;
                case 'r': r += '\r'; break;
                case 't': r += '\t'; break;
                case 'u': {
                    // minimal unicode escape handling
                    char hex[5] = {p[1], p[2], p[3], p[4], 0};
                    unsigned long cp = std::strtoul(hex, nullptr, 16);
                    if (cp < 0x80) r += (char)cp;
                    else r += '?';
                    p += 4;
                    break;
                }
                default: r += *p;
            }
        } else {
            r += *p;
        }
        ++p;
    }
    if (*p == '"') ++p;
    return r;
}

static JVal parseArr(const char*& p) {
    JVal v; v.t = JType::Arr;
    if (*p != '[') return v; ++p;
    skipWS(p);
    if (*p == ']') { ++p; return v; }
    while (true) {
        v.a.push_back(parseVal(p));
        skipWS(p);
        if (*p == ']') { ++p; return v; }
        if (*p == ',') ++p;
        skipWS(p);
    }
}

static JVal parseObj(const char*& p) {
    JVal v; v.t = JType::Obj;
    if (*p != '{') return v; ++p;
    skipWS(p);
    if (*p == '}') { ++p; return v; }
    while (true) {
        skipWS(p);
        std::string key = parseStr(p);
        skipWS(p);
        if (*p == ':') ++p;
        skipWS(p);
        v.o[key] = parseVal(p);
        skipWS(p);
        if (*p == '}') { ++p; return v; }
        if (*p == ',') ++p;
    }
}

static JVal parseVal(const char*& p) {
    skipWS(p);
    if (!*p) return JVal();
    if (*p == '{') return parseObj(p);
    if (*p == '[') return parseArr(p);
    if (*p == '"') { JVal v; v.t = JType::Str; v.s = parseStr(p); return v; }
    if (*p == 't' || *p == 'f') {
        JVal v; v.t = JType::Bool;
        if (strncmp(p, "true", 4) == 0) { v.b = true; p += 4; }
        else { v.b = false; p += 5; }
        return v;
    }
    if (*p == 'n') {
        p += 4; // null
        return JVal();
    }
    // number
    JVal v; v.t = JType::Num;
    char* end = nullptr;
    v.n = std::strtod(p, &end);
    if (end > p) p = end;
    return v;
}

static JVal parseJSON(const uint8_t* data, size_t size) {
    std::string s((const char*)data, size);
    const char* p = s.c_str();
    return parseVal(p);
}

// ─── GLB loading ──────────────────────────────────────────────────

struct GLBAccessor {
    int bufferView = -1;
    int byteOffset = 0;
    int componentType = 0;
    int count = 0;
    std::string type; // "VEC3", "VEC2", "SCALAR", "MAT4"
};

struct GLBBufferView {
    int buffer = 0;
    int byteOffset = 0;
    int byteLength = 0;
    int byteStride = 0;
};

static int componentSize(int ct) {
    switch (ct) {
        case 5120: return 1; // BYTE
        case 5121: return 1; // UNSIGNED_BYTE
        case 5122: return 2; // SHORT
        case 5123: return 2; // UNSIGNED_SHORT
        case 5125: return 4; // UNSIGNED_INT
        case 5126: return 4; // FLOAT
        default: return 0;
    }
}

static int componentCount(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT2") return 4;
    if (type == "MAT3") return 9;
    if (type == "MAT4") return 16;
    return 0;
}

GLBMesh loadGLB(const uint8_t* data, size_t size) {
    GLBMesh result;

    if (!data || size < 12) {
        Console::instance().printf(LogLevel::Error, "GLB: file too small (%zu bytes)", size);
        return result;
    }

    uint32_t magic = data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    uint32_t version = data[4] | ((uint32_t)data[5] << 8) | ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
    uint32_t fileLen = data[8] | ((uint32_t)data[9] << 8) | ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);

    if (magic != 0x46546C67) {
        Console::instance().printf(LogLevel::Error, "GLB: invalid magic 0x%08X", magic);
        return result;
    }
    if (version != 2) {
        Console::instance().printf(LogLevel::Error, "GLB: unsupported version %u", version);
        return result;
    }

    if (fileLen > size) fileLen = (uint32_t)size;

    size_t offset = 12;
    const uint8_t* binData = nullptr;
    size_t binLen = 0;
    std::string jsonStr;

    while (offset + 8 <= fileLen) {
        uint32_t chunkLen = data[offset] | ((uint32_t)data[offset+1] << 8) | ((uint32_t)data[offset+2] << 16) | ((uint32_t)data[offset+3] << 24);
        uint32_t chunkType = data[offset+4] | ((uint32_t)data[offset+5] << 8) | ((uint32_t)data[offset+6] << 16) | ((uint32_t)data[offset+7] << 24);
        offset += 8;

        if (offset + chunkLen > fileLen) break;

        if (chunkType == 0x4E4F534A) { // JSON
            jsonStr.assign((const char*)data + offset, chunkLen);
        } else if (chunkType == 0x004E4942) { // BIN
            binData = data + offset;
            binLen = chunkLen;
        }
        offset += chunkLen;
    }

    if (jsonStr.empty()) {
        Console::instance().printf(LogLevel::Error, "GLB: no JSON chunk");
        return result;
    }

    JVal root = parseJSON((const uint8_t*)jsonStr.c_str(), jsonStr.size());

    // Parse buffer views
    std::vector<GLBBufferView> bufViews;
    const JVal& bvArr = root["bufferViews"];
    for (size_t i = 0; i < bvArr.size(); i++) {
        GLBBufferView bv;
        bv.buffer = (int)bvArr[i]["buffer"].asInt();
        bv.byteOffset = (int)bvArr[i]["byteOffset"].asInt();
        bv.byteLength = (int)bvArr[i]["byteLength"].asInt();
        bv.byteStride = (int)bvArr[i]["byteStride"].asInt();
        bufViews.push_back(bv);
    }

    // Parse accessors
    std::vector<GLBAccessor> accessors;
    const JVal& accArr = root["accessors"];
    for (size_t i = 0; i < accArr.size(); i++) {
        GLBAccessor a;
        a.bufferView = (int)accArr[i]["bufferView"].asInt();
        a.byteOffset = (int)accArr[i]["byteOffset"].asInt();
        a.componentType = (int)accArr[i]["componentType"].asInt();
        a.count = (int)accArr[i]["count"].asInt();
        a.type = accArr[i]["type"].asStr();
        accessors.push_back(a);
    }

    if (accessors.empty()) {
        Console::instance().printf(LogLevel::Debug, "GLB: no accessors found");
        return result;
    }

    // ─── Parse images (embedded textures) ────────────────────────────
    struct GLBImage {
        int bufferView = -1;
        int byteOffset = 0;
        std::string mimeType;
    };
    std::vector<GLBImage> glbImages;
    const JVal& imgArr = root["images"];
    for (size_t i = 0; i < imgArr.size(); i++) {
        GLBImage img;
        img.bufferView = (int)imgArr[i]["bufferView"].asInt();
        img.mimeType = imgArr[i]["mimeType"].asStr();
        glbImages.push_back(img);
    }

    // Load textures from embedded images
    // Build a mapping: glTF texture index → loaded Texture
    // glTF texture references an image via its "source" field
    const JVal& texArr = root["textures"];
    std::vector<Texture> loadedTextures; // per texture index
    for (size_t i = 0; i < texArr.size(); i++) {
        int source = (int)texArr[i]["source"].asInt();
        Texture tex;
        if (source >= 0 && source < (int)glbImages.size()) {
            int bv = glbImages[source].bufferView;
            if (bv >= 0 && bv < (int)bufViews.size() && binData) {
                GLBBufferView& imgBV = bufViews[bv];
                size_t imgOffset = (size_t)imgBV.byteOffset;
                size_t imgSize = (size_t)imgBV.byteLength;
                if (imgOffset + imgSize <= binLen) {
                    tex.load(binData + imgOffset, imgSize);
                }
            }
        }
        loadedTextures.push_back(std::move(tex));
    }

    // Build per-material texture index mapping
    // Material → texture index (or -1 if no baseColorTexture)
    const JVal& matArr = root["materials"];
    struct MatInfo { int texIndex = -1; };
    std::vector<MatInfo> matInfos;
    result.materials.resize(matArr.size());
    for (size_t i = 0; i < matArr.size(); i++) {
        MatInfo mi;
        const JVal& pbr = matArr[i]["pbrMetallicRoughness"];
        const JVal& bct = pbr["baseColorTexture"];
        if (bct.t != JType::Null) {
            mi.texIndex = (int)bct["index"].asInt();
        }
        matInfos.push_back(mi);

        // Extract resource_path from extras
        MaterialInfo& matInfo = result.materials[i];
        if (matArr[i]["extras"].t != JType::Null) {
            const JVal& ext = matArr[i]["extras"];
            if (ext["resource_path"].t != JType::Null) {
                matInfo.resourcePath = ext["resource_path"].asStr();
                // Normalize backslashes to forward slashes
                for (auto& c : matInfo.resourcePath) if (c == '\\') c = '/';
            }
        }
    }

    // Collect unique textures referenced by materials
    result.textures.clear();
    std::vector<int> matToTexIndex(matInfos.size(), -1); // material index → index in result.textures
    for (size_t i = 0; i < matInfos.size(); i++) {
        int ti = matInfos[i].texIndex;
        if (ti >= 0 && ti < (int)loadedTextures.size() && loadedTextures[ti].loaded) {
            // Check if this texture is already in result.textures
            bool found = false;
            for (size_t j = 0; j < result.textures.size(); j++) {
                if (result.textures[j].id == loadedTextures[ti].id) {
                    matToTexIndex[i] = (int)j;
                    found = true;
                    break;
                }
            }
            if (!found) {
                matToTexIndex[i] = (int)result.textures.size();
                result.textures.push_back(std::move(loadedTextures[ti]));
            }
        }
    }

    // ─── Parse meshes ──────────────────────────────────────────────
    const JVal& meshArr = root["meshes"];
    Console::instance().printf(LogLevel::Debug, "GLB: %zu meshes, %zu accessors, %zu bufViews, %zu materials, %zu textures, %zu images, bin=%zu bytes",
        meshArr.size(), accessors.size(), bufViews.size(), matInfos.size(), result.textures.size(), glbImages.size(), binLen);

    for (size_t m = 0; m < meshArr.size(); m++) {
        const JVal& primArr = meshArr[m]["primitives"];
        if (m == 0 && meshArr[m].o.count("name"))
            result.name = meshArr[m]["name"].asStr();

        for (size_t p = 0; p < primArr.size(); p++) {
            const JVal& prim = primArr[p];
            int mode = (int)prim["mode"].asInt();
            if (mode == 0) mode = 4; // default triangles

            if (mode != 4) continue; // Skip non-triangle primitives

            // Get attribute accessor indices
            const JVal& attrs = prim["attributes"];
            int posAcc = (int)attrs["POSITION"].asInt();
            int nrmAcc = (int)attrs["NORMAL"].asInt();
            int uv0Acc = (int)attrs["TEXCOORD_0"].asInt();
            int idxAcc = (int)prim["indices"].asInt();
            int rawMatIdx = (int)prim["material"].asInt();

            if (posAcc < 0 || posAcc >= (int)accessors.size()) continue;
            if (idxAcc < 0 || idxAcc >= (int)accessors.size()) continue;

            GLBAccessor& posA = accessors[posAcc];
            GLBAccessor& idxA = accessors[idxAcc];

            if (posA.bufferView < 0 || posA.bufferView >= (int)bufViews.size()) continue;
            if (idxA.bufferView < 0 || idxA.bufferView >= (int)bufViews.size()) continue;

            GLBBufferView& posBV = bufViews[posA.bufferView];
            GLBBufferView& idxBV = bufViews[idxA.bufferView];

            int vertexCount = posA.count;
            int indexCount = idxA.count;
            int vertStride = posBV.byteStride > 0 ? posBV.byteStride : (componentSize(posA.componentType) * componentCount(posA.type));
            int idxStride = componentSize(idxA.componentType);
            int idxCompType = idxA.componentType;

            // Get vertex data pointers
            size_t vertBase = (size_t)posBV.byteOffset + (size_t)posA.byteOffset;
            size_t nrmOffset = 0;
            size_t uv0Offset = 0;

            // Find normal and UV offsets within the interleaved vertex
            if (nrmAcc >= 0 && nrmAcc < (int)accessors.size()) {
                GLBAccessor& nrmA = accessors[nrmAcc];
                if (nrmA.bufferView == posA.bufferView) {
                    nrmOffset = (size_t)nrmA.byteOffset;
                }
            }
            if (uv0Acc >= 0 && uv0Acc < (int)accessors.size()) {
                GLBAccessor& uv0A = accessors[uv0Acc];
                if (uv0A.bufferView == posA.bufferView) {
                    uv0Offset = (size_t)uv0A.byteOffset;
                }
            }

            // Build vertices
            std::vector<Vertex> verts;
            verts.reserve(vertexCount);

            for (int vi = 0; vi < vertexCount; vi++) {
                Vertex v;
                size_t base = vertBase + (size_t)vi * (size_t)vertStride;

                float px, py, pz, nx, ny, nz, u, vtx;

                // POSITION (always at offset 0 in GLB)
                if (base + 12 <= binLen) {
                    px = *(const float*)(binData + base);
                    py = *(const float*)(binData + base + 4);
                    pz = *(const float*)(binData + base + 8);
                } else { px = py = pz = 0; }

                // NORMAL
                if (nrmOffset > 0 && base + nrmOffset + 12 <= binLen) {
                    nx = *(const float*)(binData + base + nrmOffset);
                    ny = *(const float*)(binData + base + nrmOffset + 4);
                    nz = *(const float*)(binData + base + nrmOffset + 8);
                } else { nx = 0; ny = 1; nz = 0; }

                // TEXCOORD_0
                if (uv0Offset > 0 && base + uv0Offset + 8 <= binLen) {
                    u  = *(const float*)(binData + base + uv0Offset);
                    vtx = *(const float*)(binData + base + uv0Offset + 4);
                } else { u = vtx = 0; }

                // Both GLB and engine use Y-up, use directly
                v.pos = { px, py, pz };
                v.normal = { nx, ny, nz };
                v.uv = { u, vtx };
                v.color = { 1.0f, 1.0f, 1.0f, 1.0f };

                verts.push_back(v);
            }

            // Build indices
            std::vector<uint32_t> idxs;
            idxs.reserve(indexCount);

            size_t idxBase = (size_t)idxBV.byteOffset + (size_t)idxA.byteOffset;

            for (int ii = 0; ii < indexCount; ii++) {
                uint32_t idx = 0;
                size_t ioff = idxBase + (size_t)ii * (size_t)idxStride;
                if (ioff + idxStride <= binLen) {
                    if (idxCompType == 5123) { // UNSIGNED_SHORT
                        idx = *(const uint16_t*)(binData + ioff);
                    } else if (idxCompType == 5125) { // UNSIGNED_INT
                        idx = *(const uint32_t*)(binData + ioff);
                    } else if (idxCompType == 5121) { // UNSIGNED_BYTE
                        idx = binData[ioff];
                    }
                }
                idxs.push_back(idx);
            }

            if (verts.empty() || idxs.empty()) continue;

            MeshData mesh;
            mesh.vertices = std::move(verts);
            mesh.indices = std::move(idxs);
            // Assign material index (maps to result.textures)
            mesh.materialIdx = rawMatIdx;
            mesh.materialIndex = (rawMatIdx >= 0 && rawMatIdx < (int)matToTexIndex.size()) ? matToTexIndex[rawMatIdx] : -1;
            mesh.upload();
            result.meshes.push_back(std::move(mesh));
        }
    }

    Console::instance().printf(LogLevel::Debug, "GLB: loaded %zu meshes, %zu textures", result.meshes.size(), result.textures.size());
    return result;
}

GLBMesh loadGLBFromFile(const std::string& path) {
    auto& fs = Engine::instance().fs();
    Console::instance().printf(LogLevel::Debug, "GLB: loading %s", path.c_str());

    auto data = fs.read(path.c_str());
    if (data.empty()) {
        Console::instance().printf(LogLevel::Error, "GLB: file not found: %s", path.c_str());
        return GLBMesh();
    }

    return loadGLB(data.data(), data.size());
}
