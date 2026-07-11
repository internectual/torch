#include "render/gui_renderer.h"
#include "render/shader.h"
#include "core/console.h"
#include "core/engine.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include <GL/glew.h>
#include <algorithm>
#include <unordered_map>
#include <cstdio>
#include <cmath>

GuiControl* GuiControl::findChild(const std::string& name) {
    for (auto* c : children) if (c->name == name) return c;
    for (auto* c : children) { auto* r = c->findChild(name); if (r) return r; }
    return nullptr;
}

void GuiControl::addChild(GuiControl* child) {
    child->parent = this;
    children.push_back(child);
}

// Reusable quad batch (avoids per-frame VAO/VBO/EBO creation)
static uint32_t s_quadVAO{}, s_quadVBO{}, s_quadEBO{};
static bool s_quadBatchInit = false;

static void initQuadBatch() {
    struct SV { float x,y,z; float u,v; float r,g,b,a; };
    glGenVertexArrays(1, &s_quadVAO);
    glGenBuffers(1, &s_quadVBO);
    glGenBuffers(1, &s_quadEBO);
    glBindVertexArray(s_quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, s_quadVBO);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(SV),0); glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)12); glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)20); glEnableVertexAttribArray(2);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_quadEBO);
    glBindVertexArray(0);
    s_quadBatchInit = true;
}

static void drawQuadBatch(const void* verts, size_t vertSize, const void* indices, size_t indexSize) {
    if (!s_quadBatchInit) initQuadBatch();
    glBindVertexArray(s_quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, s_quadVBO);
    glBufferData(GL_ARRAY_BUFFER, vertSize, verts, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_quadEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indexSize, indices, GL_STREAM_DRAW);
    glDisable(GL_CULL_FACE);
    glDrawElements(GL_TRIANGLES, (GLsizei)(indexSize / sizeof(uint32_t)), GL_UNSIGNED_INT, 0);
}

static void destroyQuadBatch() {
    if (s_quadVAO) { glDeleteVertexArrays(1, &s_quadVAO); s_quadVAO = 0; }
    if (s_quadVBO) { glDeleteBuffers(1, &s_quadVBO); s_quadVBO = 0; }
    if (s_quadEBO) { glDeleteBuffers(1, &s_quadEBO); s_quadEBO = 0; }
    s_quadBatchInit = false;
}

GuiRenderer::GuiRenderer() {}
GuiRenderer::~GuiRenderer() {
    auto del = [&](auto& self, GuiControl* ctl) -> void {
        for (auto* child : ctl->children) self(self, child);
        delete ctl;
    };
    if (canvas) del(del, canvas);
    delete checkerTex;
}

void GuiRenderer::init() {
    canvas = nullptr;
    auto& objs = ScriptEngine::instance().objects;

    // First pass: create GuiControl objects for all GUI-related ScriptObjects
    std::unordered_map<std::string, GuiControl*> controlMap;
    for (auto& [name, obj] : objs) {
        if (obj->className.find("Gui") == 0 || obj->className.find("Shell") == 0 || obj->className == "GameTSCtrl") {
            GuiControl* ctl = new GuiControl;
            ctl->name = obj->name;
            ctl->className = obj->className;

            auto rf = [&](const std::string& key, float def) {
                auto it = obj->fields.find(key);
                if (it != obj->fields.end()) return (float)it->second.toDouble();
                auto it2 = obj->internals.find(key);
                if (it2 != obj->internals.end()) return (float)it2->second.toDouble();
                return def;
            };
            // Parse "x y" format strings
            auto parsePair = [&](const std::string& key, float& a, float& b) {
                auto it = obj->fields.find(key);
                if (it != obj->fields.end()) {
                    std::string s = it->second.toString();
                    sscanf(s.c_str(), "%f %f", &a, &b);
                }
            };
            parsePair("position", ctl->posX, ctl->posY);
            parsePair("extent", ctl->extentX, ctl->extentY);

            auto it = obj->fields.find("text");
            if (it != obj->fields.end()) ctl->text = it->second.toString();
            it = obj->fields.find("bitmap");
            if (it != obj->fields.end()) ctl->bitmap = it->second.toString();
            it = obj->fields.find("command");
            if (it != obj->fields.end()) ctl->command = it->second.toString();
            it = obj->fields.find("altCommand");
            if (it != obj->fields.end()) ctl->altCommand = it->second.toString();
            it = obj->fields.find("profile");
            if (it != obj->fields.end()) ctl->profileName = it->second.toString();
            it = obj->fields.find("visible");
            if (it != obj->fields.end()) ctl->visible = it->second.toBool();

            controlMap[name] = ctl;

            if (ctl->className == "GuiCanvas") {
                canvas = ctl;
            }

            // Wire command execution for clickable controls
            bool isClickable =
                ctl->className.find("Button") != std::string::npos ||
                ctl->className == "GuiCheckBoxCtrl" ||
                ctl->className == "GuiRadioCtrl" ||
                ctl->className == "ShellBitmapButton" ||
                ctl->className == "ShellToggleButton" ||
                ctl->className == "ShellTabButton" ||
                ctl->className == "GuiTextEditCtrl" ||
                ctl->className == "ShellTextEditCtrl";
            if (!ctl->command.empty() && isClickable) {
                std::string cmd = ctl->command;
                ctl->onClick = [cmd]() {
                    Console::instance().execute(cmd.c_str());
                };
            }
        }
    }

    // Ensure canvas exists
    if (!canvas) {
        canvas = new GuiControl;
        canvas->name = "Canvas";
        canvas->className = "GuiCanvas";
        canvas->extentX = 1024;
        canvas->extentY = 768;
    }

    // Second pass: link parent-child relationships
    for (auto& [name, obj] : objs) {
        if (obj->className.find("Gui") == 0 || obj->className.find("Shell") == 0 || obj->className == "GameTSCtrl") {
            auto ctl = controlMap.find(name);
            if (ctl == controlMap.end()) continue;
            auto pit = obj->internals.find("parent");
            if (pit != obj->internals.end()) {
                std::string pname = pit->second.toString();
                auto pc = controlMap.find(pname);
                if (pc != controlMap.end()) {
                    pc->second->addChild(ctl->second);
                }
            } else if (ctl->second != canvas && canvas) {
                canvas->addChild(ctl->second);
            }
        }
    }

    // Third pass: propagate extent from canvas down, inheriting from
    // the nearest ancestor with a non-default (>100x30) extent.
    std::function<void(GuiControl*, float, float)> propagateExtent;
    propagateExtent = [&](GuiControl* ctl, float parentW, float parentH) {
        float w = ctl->extentX, h = ctl->extentY;
        if (w <= 100 && h <= 30) { w = parentW; h = parentH; }
        ctl->extentX = w; ctl->extentY = h;
        for (auto* child : ctl->children)
            propagateExtent(child, w, h);
    };
    auto& platRef = Engine::instance().platform();
    if (canvas) propagateExtent(canvas, (float)platRef.width(), (float)platRef.height());

    // Checkerboard texture
    std::vector<uint8_t> cb(16*16*4);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            bool bright = ((x/4)+(y/4))%2==0;
            cb[(y*16+x)*4+0]=bright?255:0; cb[(y*16+x)*4+1]=0;
            cb[(y*16+x)*4+2]=bright?255:0; cb[(y*16+x)*4+3]=255;
        }
    checkerTex = new Texture;
    checkerTex->loadRaw(cb.data(), 16, 16, 4);
}

void GuiRenderer::refresh() {
    auto& objs = ScriptEngine::instance().objects;
    for (auto& [name, obj] : objs) {
        if (obj->className.find("Gui") == 0 || obj->className.find("Shell") == 0 || obj->className == "GameTSCtrl") {
            if (findControl(name)) continue;
            GuiControl* ctl = new GuiControl;
            ctl->name = obj->name; ctl->className = obj->className;
            auto parsePair = [&](const std::string& key, float& a, float& b) {
                auto it = obj->fields.find(key);
                if (it != obj->fields.end()) { std::string s = it->second.toString(); sscanf(s.c_str(), "%f %f", &a, &b); }
            };
            parsePair("position", ctl->posX, ctl->posY);
            parsePair("extent", ctl->extentX, ctl->extentY);
            auto it = obj->fields.find("text"); if (it != obj->fields.end()) ctl->text = it->second.toString();
            it = obj->fields.find("bitmap"); if (it != obj->fields.end()) ctl->bitmap = it->second.toString();
            it = obj->fields.find("command"); if (it != obj->fields.end()) ctl->command = it->second.toString();
            it = obj->fields.find("altCommand"); if (it != obj->fields.end()) ctl->altCommand = it->second.toString();
            it = obj->fields.find("profile"); if (it != obj->fields.end()) ctl->profileName = it->second.toString();
            it = obj->fields.find("visible"); if (it != obj->fields.end()) ctl->visible = it->second.toBool();
            if (ctl->className == "GuiCanvas") canvas = ctl;
            bool isClickable = ctl->className.find("Button") != std::string::npos || ctl->className == "GuiCheckBoxCtrl" || ctl->className == "GuiRadioCtrl" || ctl->className == "ShellBitmapButton" || ctl->className == "ShellToggleButton" || ctl->className == "ShellTabButton" || ctl->className == "GuiTextEditCtrl" || ctl->className == "ShellTextEditCtrl";
            if (!ctl->command.empty() && isClickable) { std::string cmd = ctl->command; ctl->onClick = [cmd]() { Console::instance().execute(cmd.c_str()); }; }
            auto pit = obj->internals.find("parent");
            if (pit != obj->internals.end()) {
                std::string pname = pit->second.toString();
                GuiControl* parent = findControl(pname);
                if (parent) parent->addChild(ctl);
            } else if (ctl != canvas && canvas) { canvas->addChild(ctl); }
            if (canvas) {
                bool smallX = ctl->extentX <= 100;
                bool smallY = ctl->extentY <= 30;
                ctl->extentX = smallX && smallY ? canvas->extentX : ctl->extentX;
                ctl->extentY = smallX && smallY ? canvas->extentY : ctl->extentY;
            }
        }
    }
}



void GuiRenderer::render() {
    if (!canvas) return;
    auto& r = Engine::instance().renderer();



    // Save projection and view, set orthographic for GUI
    MatrixF savedProj = r.projectionMatrix();
    MatrixF savedView = r.view;
    auto& plat = Engine::instance().platform();
    int w = plat.width(), h = plat.height();
    MatrixF ortho;
    ortho.identity();
    ortho.m[0][0] = 2.0f / w;
    ortho.m[1][1] = -2.0f / h;
    ortho.m[0][3] = -1.0f;
    ortho.m[1][3] = 1.0f;
    r.setProjection(ortho);
    r.setView(MatrixF{});

    // Render all dialogs: first is content, subsequent are overlays
    for (auto* dlg : dialogStack) {
        renderControl(dlg);
    }
    // Always render debug overlay so user knows the engine is alive
    r.renderText("TORCH", 10, 10, {0,1,0,1}, 2.0f);
    r.renderText("~ Console | Pause Debug", 10, 35, {0.8f,0.8f,0.8f,1}, 1.0f);
    // When no dialog is active, show loading text
    if (dialogStack.empty()) {
        r.drawBox({{0,0,0}, {1024,768,0}}, {0.15f,0.15f,0.2f,1});
        r.renderText("Torch - Loading...", 380, 370, {0.6f,0.6f,0.7f,1}, 1.5f);
    }



    // Restore projection and view
    r.setProjection(savedProj);
    r.setView(savedView);
}

struct ClipRect { float x, y, w, h; };
struct BmpCell { int x, y, w, h; };

// Profile lookup from loaded scripts
static std::unordered_map<std::string, ScriptObject*> s_profileCache;
static ScriptObject* getProfile(const std::string& name) {
    auto cit = s_profileCache.find(name);
    if (cit != s_profileCache.end()) return cit->second;
    auto& objs = ScriptEngine::instance().objects;
    auto it = objs.find(name);
    ScriptObject* result = nullptr;
    if (it != objs.end() && it->second->className == "GuiControlProfile")
        result = it->second;
    s_profileCache[name] = result;
    return result;
}

// Get font from profile's fontType/fontSize
static Font* getProfileFont(ScriptObject* prof) {
    if (!prof) return Engine::instance().renderer().getFont();
    auto ft = prof->fields.find("fontType");
    auto fs = prof->fields.find("fontSize");
    std::string name = (ft != prof->fields.end()) ? ft->second.toString() : "";
    int size = (fs != prof->fields.end()) ? fs->second.toInt() : 0;
    if (name.empty() || size <= 0) return Engine::instance().renderer().getFont();
    return Engine::instance().renderer().getFont(name.c_str(), size);
}

// Parse textOffset from profile (two ints: "x y")
static bool getTextOffset(ScriptObject* prof, float& ox, float& oy) {
    if (!prof) return false;
    auto to = prof->fields.find("textOffset");
    if (to == prof->fields.end()) return false;
    int ix = 0, iy = 0;
    if (sscanf(to->second.toString().c_str(), "%d %d", &ix, &iy) >= 2) {
        ox = (float)ix; oy = (float)iy;
        return true;
    }
    return false;
}

// Parse "R G B" or "R G B A" color string (0-255 range)
static bool parseColor(const std::string& s, ColorF& out) {
    int r = 0, g = 0, b = 0, a = 255;
    if (sscanf(s.c_str(), "%d %d %d %d", &r, &g, &b, &a) >= 3) {
        out = {(float)r/255.0f, (float)g/255.0f, (float)b/255.0f, (float)a/255.0f};
        return true;
    }
    return false;
}


// Bitmap array cell detection: scan texture for separator color (pixel 0,0 or magenta as fallback)
static std::vector<BmpCell> detectBitmapCells(const uint8_t* rgba, int w, int h) {
    std::vector<BmpCell> cells;
    uint8_t sepR = rgba[0], sepG = rgba[1], sepB = rgba[2];
    // Try magenta (255,0,255) as separator if pixel(0,0) is common (black/transparent)
    if ((sepR == 0 && sepG == 0 && sepB == 0) || rgba[3] == 0) {
        // Check if magenta exists as a separator color
        bool hasMagenta = false;
        for (int i = 0; i < w * 4; i += 4) if (rgba[i]==255 && rgba[i+1]==0 && rgba[i+2]==255) { hasMagenta = true; break; }
        if (hasMagenta) { sepR = 255; sepG = 0; sepB = 255; }
    }
    auto isSep = [&](int x, int y) {
        int i = (y * w + x) * 4;
        return rgba[i] == sepR && rgba[i+1] == sepG && rgba[i+2] == sepB;
    };
    // Scan rows top-to-bottom
    int y = 0;
    while (y < h) {
        if (isSep(0, y)) { y++; continue; }
        // Find bottom of this cell row
        int rowBot = y;
        while (rowBot < h && !isSep(0, rowBot)) rowBot++;
        // Scan columns left-to-right within this row
        int x = 0;
        while (x < w) {
            if (isSep(x, y)) { x++; continue; }
            int cellL = x, cellR = x;
            while (cellR < w && !isSep(cellR, y)) cellR++;
            cells.push_back({cellL, y, cellR - cellL, rowBot - y});
            x = cellR;
        }
        y = rowBot;
    }
    return cells;
}

// 9-slice bitmap array rendering for shell textures using detected cells
static void drawBmpArrayButton(Renderer& r, const Point3F& dstA, const Point3F& dstB,
                                uint32_t texId, const std::vector<BmpCell>& cells,
                                int texW, int texH,
                                int state /* 0=normal,1=pressed,2=hover,3=disabled */) {
    int baseIdx = 9 * state;
    if (baseIdx + 8 >= (int)cells.size()) { r.drawTexturedRect(dstA, dstB, texId); return; }
    auto* ss = ShaderManager::getSpriteShader();
    if (!ss) return;
    ss->bind();
    ss->setUniform("uProjection", r.projection);
    ss->setUniform("uView", r.view);
    ss->setUniform("uUseTexture", int32_t(1));
    ss->setUniform("uTexture", int32_t(0));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texId);

    struct SV { float x,y,z; float u,v; float r,g,b,a; };
    auto drawCell = [&](int cellIdx, float rx, float ry, float rw, float rh, bool stretchX, bool stretchY) {
        if (rw <= 0 || rh <= 0) return;
        const auto& src = cells[baseIdx + cellIdx];
        float u0 = (float)(src.x) / (float)texW, v0 = (float)(src.y) / (float)texH;
        float u1 = (float)(src.x + src.w) / (float)texW, v1 = (float)(src.y + src.h) / (float)texH;
        if (stretchX) { u0 += 1.0f/texW; u1 -= 1.0f/texW; }
        if (stretchY) { v0 += 1.0f/texH; v1 -= 1.0f/texH; }
        SV verts[4] = {
            {rx,ry,0, u0,v0, 1,1,1,1}, {rx+rw,ry,0, u1,v0, 1,1,1,1},
            {rx,ry+rh,0, u0,v1, 1,1,1,1}, {rx+rw,ry+rh,0, u1,v1, 1,1,1,1}
        };
        uint32_t ids[] = {0,1,2,1,3,2};
        drawQuadBatch(verts, sizeof(verts), ids, sizeof(ids));
    };

    float dx = dstA.x, dy = dstA.y, dw = dstB.x - dstA.x, dh = dstB.y - dstA.y;
    const auto& c0 = cells[baseIdx+0], c2 = cells[baseIdx+2];
    const auto& c6 = cells[baseIdx+6], c8 = cells[baseIdx+8];
    float lw = (float)c0.w, rw = (float)c2.w, th = (float)c0.h, bh = (float)c6.h;
    float mw = dw - lw - rw; if (mw < 0) mw = 0;
    float mh = dh - th - bh; if (mh < 0) mh = 0;

    // Corners (no stretch)
    drawCell(0, dx, dy, lw, th, false, false); // TL
    drawCell(2, dx+dw-rw, dy, rw, th, false, false); // TR
    drawCell(6, dx, dy+dh-bh, lw, bh, false, false); // BL
    drawCell(8, dx+dw-rw, dy+dh-bh, rw, bh, false, false); // BR
    // Edges (stretch one axis)
    drawCell(1, dx+lw, dy, mw, th, true, false); // T
    drawCell(7, dx+lw, dy+dh-bh, mw, bh, true, false); // B
    drawCell(3, dx, dy+th, lw, mh, false, true); // L
    drawCell(5, dx+dw-rw, dy+th, rw, mh, false, true); // R
    // Fill (tile both axes using GL_REPEAT)
    if (mw > 0 && mh > 0) {
        const auto& src = cells[baseIdx + 4];
        float tileW = (float)src.w, tileH = (float)src.h;
        float repeatX = mw / tileW, repeatY = mh / tileH;
        float u0 = (float)src.x / texW, v0 = (float)src.y / texH;
        float u1 = (float)(src.x + src.w) / texW, v1 = (float)(src.y + src.h) / texH;
        // Use GL_REPEAT for tiling
        GLint oldWrap;
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &oldWrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        struct SV2 { float x,y,z; float u,v; float r,g,b,a; };
        SV2 verts[4] = {
            {dx+lw, dy+th, 0, 0, 0, 1,1,1,1},
            {dx+lw+mw, dy+th, 0, repeatX, 0, 1,1,1,1},
            {dx+lw, dy+th+mh, 0, 0, repeatY, 1,1,1,1},
            {dx+lw+mw, dy+th+mh, 0, repeatX, repeatY, 1,1,1,1}
        };
        uint32_t ids[] = {0,1,2,1,3,2};
        drawQuadBatch(verts, sizeof(verts), ids, sizeof(ids));
        // Restore wrap
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, oldWrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, oldWrap);
    }
}

// Generated shell texture cache (for textures not found in archives)
static std::unordered_map<std::string, Texture*> g_genShellTex;

static void putPixel(std::vector<uint8_t>& p, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    if (x < 0 || x >= w || y < 0 || y >= h) return;
    int i = (y * w + x) * 4; p[i] = r; p[i+1] = g; p[i+2] = b; p[i+3] = a;
}
static void fillRect(std::vector<uint8_t>& p, int x, int y, int w, int h, int tw, int th, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    for (int j = y; j < y + h && j < th; j++) for (int i = x; i < x + w && i < tw; i++) putPixel(p, i, j, tw, th, r, g, b, a);
}
static void fillGradV(std::vector<uint8_t>& p, int x, int y, int w, int h, int tw, int th, uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2, uint8_t a = 255) {
    for (int j = y; j < y + h && j < th; j++) {
        float t = (h > 1) ? (float)(j - y) / (h - 1) : 0;
        uint8_t rr = (uint8_t)(r1 + (r2 - r1) * t), gg = (uint8_t)(g1 + (g2 - g1) * t), bb = (uint8_t)(b1 + (b2 - b1) * t);
        for (int i = x; i < x + w && i < tw; i++) putPixel(p, i, j, tw, th, rr, gg, bb, a);
    }
}
static void drawSepLine(std::vector<uint8_t>& p, int x, int y, int w, int h, int tw, int th) {
    // magenta separator
    for (int j = y; j < y + h && j < th; j++) for (int i = x; i < x + w && i < tw; i++) putPixel(p, i, j, tw, th, 255, 0, 255);
}

// Generate a 9-cell (3×3) texture for shell buttons/entries.
// Layout: 3x3 cells of (cellW×cellH) each, magenta separator lines ONLY between cells.
// NO outer margins, matching the original T2 bitmap array layout.
// pixel(0,0) is the first cell's corner (not magenta) so detectBitmapCells
// uses the cell color as base and magenta inner lines as separators.
static Texture* gen9SliceTexture(Renderer& r, const char* path,
    uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2,    // normal
    uint8_t r3, uint8_t g3, uint8_t b3, uint8_t r4, uint8_t g4, uint8_t b4,    // hover
    uint8_t r5, uint8_t g5, uint8_t b5, uint8_t r6, uint8_t g6, uint8_t b6,    // pressed
    int cellW = 8, int cellH = 24)
{
    const int sep = 2;
    int mw = cellW * 3 + sep * 2; // C1 | C2 | C3
    int mh = cellH * 3 + sep * 2; // row1 --- row2 --- row3
    std::vector<uint8_t> pix(mw * mh * 4, 0);
    // Fill separator areas (magenta) — only between cells, no outer margins
    for (int j = 0; j < mh; j++) for (int i = 0; i < mw; i++) {
        int modW = cellW + sep, modH = cellH + sep;
        bool onSep = (i % modW >= cellW) || (j % modH >= cellH);
        if (onSep) putPixel(pix, i, j, mw, mh, 255, 0, 255);
    }
    // Fill each cell with gradient + border
    struct State { uint8_t r1,g1,b1, r2,g2,b2; } states[3] = {
        {r1,g1,b1, r2,g2,b2},
        {r3,g3,b3, r4,g4,b4},
        {r5,g5,b5, r6,g6,b6}
    };
    for (int s = 0; s < 3; s++) {
        int sy = s * (cellH + sep);
        for (int c = 0; c < 3; c++) {
            int cx = c * (cellW + sep);
            fillGradV(pix, cx, sy, cellW, cellH, mw, mh, states[s].r1, states[s].g1, states[s].b1, states[s].r2, states[s].g2, states[s].b2);
            // 1px highlight/shadow border
            for (int i = cx; i < cx + cellW; i++) { putPixel(pix, i, sy, mw, mh, 255,255,255,35); putPixel(pix, i, sy+cellH-1, mw, mh, 0,0,0,50); }
            for (int j = sy; j < sy + cellH; j++) { putPixel(pix, cx, j, mw, mh, 255,255,255,25); putPixel(pix, cx+cellW-1, j, mw, mh, 0,0,0,40); }
        }
    }
    // Pixel(0,0) must be black so detectBitmapCells triggers its magenta fallback
    putPixel(pix, 0, 0, mw, mh, 0, 0, 0);
    auto* tex = new Texture;
    tex->loadRaw(pix.data(), mw, mh, 4);
    r.addTexture(tex);
    g_genShellTex[path] = tex;
    return tex;
}

// Generate a simple single-tile texture (for dialog fills, scroll tracks)
static Texture* genTileTexture(Renderer& r, const char* path, int w, int h,
    uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2)
{
    std::vector<uint8_t> pix(w * h * 4, 0);
    fillGradV(pix, 0, 0, w, h, w, h, r1, g1, b1, r2, g2, b2);
    // Add subtle noise
    srand(42);
    for (int i = 0; i < w * h * 4; i += 4) {
        int n = (rand() % 32) - 16;
        pix[i] = (uint8_t)std::max(0, std::min(255, (int)pix[i] + n));
        pix[i+1] = (uint8_t)std::max(0, std::min(255, (int)pix[i+1] + n));
        pix[i+2] = (uint8_t)std::max(0, std::min(255, (int)pix[i+2] + n));
    }
    auto* tex = new Texture;
    tex->loadRaw(pix.data(), w, h, 4);
    r.addTexture(tex);
    g_genShellTex[path] = tex;
    return tex;
}

// Generate scrollbar textures (thumb + field as simple bars)
static Texture* genScrollTexture(Renderer& r, const char* path, bool vert, uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2) {
    int w = vert ? 12 : 64, h = vert ? 64 : 12;
    std::vector<uint8_t> pix(w * h * 4, 0);
    fillGradV(pix, 0, 0, w, h, w, h, r1, g1, b1, r2, g2, b2);
    // Border
    for (int i = 0; i < w; i++) { putPixel(pix, i, 0, w, h, 255,255,255,30); putPixel(pix, i, h-1, w, h, 0,0,0,40); }
    for (int j = 0; j < h; j++) { putPixel(pix, 0, j, w, h, 255,255,255,20); putPixel(pix, w-1, j, w, h, 0,0,0,30); }
    auto* tex = new Texture;
    tex->loadRaw(pix.data(), w, h, 4);
    r.addTexture(tex);
    g_genShellTex[path] = tex;
    return tex;
}

static Texture* generateShellTexture(Renderer& r, const char* name) {
    // Check cache
    auto it = g_genShellTex.find(name);
    if (it != g_genShellTex.end()) return it->second;

    std::string n = name;
    if (n == "shll_button.png") {
        return gen9SliceTexture(r, name,
            80,80,100, 60,60,80,   // normal
            100,120,160, 80,100,140, // hover
            60,60,80, 45,45,60,     // pressed
            6, 24);
    } else if (n == "shll_entryfield.png") {
        return gen9SliceTexture(r, name,
            55,55,65, 40,40,50,   // normal (darker, inset look)
            60,60,70, 45,45,55,   // hover
            50,50,60, 35,35,45,   // pressed
            6, 22);
    } else if (n == "shll_radio.png") {
        // Simple radio button: 16×16 circle
        std::vector<uint8_t> pix(16*16*4, 0);
        for (int j = 0; j < 16; j++) for (int i = 0; i < 16; i++) {
            float dx = i - 7.5f, dy = j - 7.5f, d = sqrtf(dx*dx + dy*dy);
            if (d < 7.5f) {
                float bright = 80 + 40 * (1 - d/7.5f);
                putPixel(pix, i, j, 16, 16, (uint8_t)bright, (uint8_t)(bright*0.8f), (uint8_t)(bright*0.9f));
            }
        }
        auto* tex = new Texture; tex->loadRaw(pix.data(), 16, 16, 4); r.addTexture(tex); g_genShellTex[name] = tex; return tex;
    } else if (n == "shll_scroll_vertbar.png") {
        return genScrollTexture(r, name, true, 100,100,120, 70,70,90);
    } else if (n == "shll_scroll_vertfield.png") {
        return genScrollTexture(r, name, true, 45,45,55, 35,35,45);
    } else if (n == "shll_scroll_horzbar.png") {
        return genScrollTexture(r, name, false, 100,100,120, 70,70,90);
    } else if (n == "shll_scroll_horzfield.png") {
        return genScrollTexture(r, name, false, 45,45,55, 35,35,45);
    } else if (n == "dlg_fieldfill.png") {
        return genTileTexture(r, name, 64, 64, 55,55,65, 35,35,45);
    } else if (n == "dlg_frame_edge.png") {
        return genTileTexture(r, name, 8, 8, 70,70,85, 50,50,65);
    } else if (n == "dlg_titletab.png") {
        return gen9SliceTexture(r, name,
            100,120,160, 80,100,140,
            100,120,160, 80,100,140,
            100,120,160, 80,100,140,
            6, 28);
    }
    return nullptr;
}

// Shell texture cache - tries .bm8 first, generates procedurally if not found
static Texture* getShellTex(Renderer& r, const char* name) {
    // Check generated cache first
    {
        auto git = g_genShellTex.find(name);
        if (git != g_genShellTex.end()) return git->second;
    }
    std::string base = std::string("textures/gui/") + name;
    auto tryExt = [&](const std::string& ext) -> Texture* {
        std::string stem = base.substr(0, base.rfind('.'));
        std::string p = stem + ext;
        Texture* t = r.loadTexture(p.c_str());
        if (!t) {
            std::string alt = p;
            auto sl = alt.rfind('/');
            if (sl != std::string::npos && sl + 1 < alt.size()) {
                alt[sl + 1] = (char)toupper(alt[sl + 1]);
                t = r.loadTexture(alt.c_str());
                if (!t) { alt[sl + 1] = (char)tolower(alt[sl + 1]); t = r.loadTexture(alt.c_str()); }
            }
        }
        return t;
    };
    Texture* t = tryExt(".bm8");
    if (!t) t = tryExt(".png");
    // If not found on disk, generate procedurally
    if (!t) t = generateShellTexture(r, name);
    return t;
}

// Bitmap array cell cache: detect cells from texture by scanning separator color
static std::unordered_map<uint32_t, std::vector<BmpCell>> g_cellCache;

static const std::vector<BmpCell>& getBitmapCells(const uint8_t* rgba, int w, int h, uint32_t texId) {
    auto it = g_cellCache.find(texId);
    if (it != g_cellCache.end()) return it->second;
    auto cells = detectBitmapCells(rgba, w, h);
    // If few cells and texture divides evenly by 3, use implicit 3×3 grid per state block
    if (cells.size() <= 3 && w % 3 == 0 && h % 3 == 0) {
        int cw = w / 3, ch = h / 3;
        cells.clear();
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 3; col++)
                cells.push_back({col * cw, row * ch, cw, ch});
    }
    Console::instance().printf(LogLevel::Debug, "Bitmap cells for tex %u: %zu cells (%dx%d)", texId, cells.size(), w, h);
    for (size_t i = 0; i < cells.size() && i < 36; i++)
        Console::instance().printf(LogLevel::Debug, "  cell[%zu]: %d,%d %dx%d", i, cells[i].x, cells[i].y, cells[i].w, cells[i].h);
    auto& stored = g_cellCache[texId] = std::move(cells);
    return stored;
}

// Load a shell texture and detect its bitmap array cells (for 9-slice rendering)
static Texture* getShellTexWithCells(Renderer& r, const char* name, const std::vector<BmpCell>*& outCells) {
    outCells = nullptr;
    Texture* tex = getShellTex(r, name);
    if (!tex || !tex->loaded) return nullptr;
    // Check cell cache first to avoid expensive glGetTexImage readback
    {
        auto cit = g_cellCache.find(tex->id);
        if (cit != g_cellCache.end()) { outCells = &cit->second; return tex; }
    }
    int tw = tex->width, th = tex->height;
    std::vector<uint8_t> pixels(tw * th * 4);
    glBindTexture(GL_TEXTURE_2D, tex->id);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    outCells = &getBitmapCells(pixels.data(), tw, th, tex->id);
    return tex;
}

// Forward declaration with clip support
static void renderControlRec(GuiRenderer* gr, GuiControl* ctl, GuiControl* canvas,
    float scrollOfsX, float scrollOfsY, const ClipRect* clip);

static void computeContentExtent(GuiControl* ctl) {
    float maxX = 0, maxY = 0;
    std::function<void(GuiControl*)> recurse = [&](GuiControl* c) {
        if (c->className == "GuiConsole") {
            float linesH = (float)Console::instance().getLog().size() * 12.0f;
            float cy = c->posY + std::max(c->extentY, linesH);
            if (cy > maxY) maxY = cy;
            // Content width from longest log line (approx 8px per char at scale 1.5)
            size_t maxLen = 0;
            for (auto& ln : Console::instance().getLog())
                if (ln.size() > maxLen) maxLen = ln.size();
            float cw = c->posX + std::max(c->extentX, (float)maxLen * 10.0f);
            if (cw > maxX) maxX = cw;
        } else {
            float cx = c->posX + c->extentX;
            float cy = c->posY + c->extentY;
            if (cx > maxX) maxX = cx;
            if (cy > maxY) maxY = cy;
        }
        for (auto* ch : c->children) recurse(ch);
    };
    for (auto* ch : ctl->children) recurse(ch);
    ctl->contentH = maxY;
    ctl->contentW = maxX;
}

void GuiRenderer::renderControl(GuiControl* ctl) {
    renderControlRec(this, ctl, canvas, 0, 0, nullptr);
}

static void renderControlRec(GuiRenderer* gr, GuiControl* ctl, GuiControl* canvas,
    float scrollOfsX, float scrollOfsY, const ClipRect* clip)
{
    if (!ctl || !ctl->visible) return;

    auto& r = Engine::instance().renderer();
    auto& fs = Engine::instance().fs();
    auto* font = r.getFont();

    float x = ctl->posX + scrollOfsX;
    float y = ctl->posY + scrollOfsY;

    // Add parent offset (walk up to canvas, excluding scroll ancestor's offset which is in scrollOfs)
    GuiControl* p = ctl->parent;
    while (p && p != canvas) {
        x += p->posX;
        y += p->posY;
        p = p->parent;
    }

    const std::string& cn = ctl->className;

    // For GuiCanvas, just use full screen
    if (cn == "GuiCanvas") {
        // Check if PlayGui (GameTSCtrl) is a child — if so, skip background so 3D scene shows through
        bool has3D = false;
        for (auto* ch : ctl->children)
            if (ch->className == "GameTSCtrl" || ch->className == "GuiTSCtrl") { has3D = true; break; }
        if (!has3D)
            r.drawRectFill({0,0,0}, {1024,768,0}, {0.15f,0.15f,0.2f,1});
        for (auto* child : ctl->children) renderControlRec(gr, child, canvas, 0, 0, nullptr);
        return;
    }

    // GameTSCtrl / PlayGui: transparent 3D viewport, render children only
    if (cn == "GameTSCtrl" || cn == "GuiTSCtrl") {
        for (auto* child : ctl->children)
            renderControlRec(gr, child, canvas, scrollOfsX, scrollOfsY, clip);
        return;
    }

    // Button types
    if (cn == "GuiButtonCtrl" || cn == "GuiTextButtonCtrl" ||
        cn == "ShellBitmapButton" || cn == "GuiBitmapButtonCtrl" ||
        cn == "ShellLaunchMenu") {
        ColorF btnFill{0.25f, 0.25f, 0.35f, 1}, btnBorder{0.5f, 0.5f, 0.6f, 1};
        ColorF btnText{1,1,1,1};
        auto* prof = getProfile(ctl->profileName);
        if (prof) {
            auto fi = prof->fields.find("fillColor");
            if (fi != prof->fields.end()) parseColor(fi->second.toString(), btnFill);
            auto bi = prof->fields.find("borderColor");
            if (bi != prof->fields.end()) parseColor(bi->second.toString(), btnBorder);
            auto fci = prof->fields.find("fontColor");
            if (fci != prof->fields.end()) parseColor(fci->second.toString(), btnText);
        }
        // Check profile bitmap
        auto loadProfileBmp = [&](const std::string& base, const std::string& ext) -> Texture* {
            std::string p1 = base + ext;
            Texture* tt = r.loadTexture(p1.c_str());
            if (!tt) {
                std::string p2 = "textures/" + base + ext;
                tt = r.loadTexture(p2.c_str());
            }
            if (!tt && base.find("textures/") != 0) {
                std::string p3 = "textures/gui/" + base + ext;
                tt = r.loadTexture(p3.c_str());
            }
            return tt;
        };
        Texture* btnTex = nullptr;
        if (prof) {
            auto bmi = prof->fields.find("bitmap");
            if (bmi != prof->fields.end()) {
                std::string base = bmi->second.toString();
                for (auto& ext : {".png", ".bm8", ".jpg"}) {
                    btnTex = loadProfileBmp(base, ext);
                    if (btnTex) break;
                }
            }
        }
        // 3-slice renderer: cells are row-major [row0:Ln,Lh,Lp row1:Mn,Mh,Mp row2:Rn,Rh,Rp]
        // For state S: Left=cells[S], Mid=cells[3+S], Right=cells[6+S] (states in columns, pieces in rows)
        const std::vector<BmpCell>* bmpCells = nullptr;
        Texture* shTex = getShellTexWithCells(r, "shll_button.png", bmpCells);
        if (shTex && bmpCells && bmpCells->size() >= 9) {
            int state = 0; // 0=normal, 1=highlight, 2=pressed
            auto& cL = (*bmpCells)[state * 3 + 0]; // left cap for this state
            auto& cF = (*bmpCells)[state * 3 + 1]; // fill/middle for this state
            auto& cR = (*bmpCells)[state * 3 + 2]; // right cap for this state
            auto* ss = ShaderManager::getSpriteShader();
            if (ss) {
                ss->bind(); ss->setUniform("uProjection",r.projection); ss->setUniform("uView",r.view);
                ss->setUniform("uUseTexture",int32_t(1)); ss->setUniform("uTexture",int32_t(0));
                glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, shTex->id);
                float cellH = (float)cL.h;
                float lw = (float)cL.w, rw = (float)cR.w;
                float midW = ctl->extentX - lw - rw; if (midW < 0) midW = 0;
                float by = y + (ctl->extentY - cellH) * 0.5f; // vertically center
                // Left cap (native size)
                r.drawTexturedRectUV({x, by, 0}, {x + lw, by + cellH, 0}, shTex->id,
                    (float)cL.x/shTex->width, (float)cL.y/shTex->height,
                    (float)(cL.x+cL.w)/shTex->width, (float)(cL.y+cL.h)/shTex->height);
                // Right cap (native size)
                r.drawTexturedRectUV({x + lw + midW, by, 0}, {x + lw + midW + rw, by + cellH, 0}, shTex->id,
                    (float)cR.x/shTex->width, (float)cR.y/shTex->height,
                    (float)(cR.x+cR.w)/shTex->width, (float)(cR.y+cR.h)/shTex->height);
                // Fill center (stretched, inset 1px to avoid separator border)
                r.drawTexturedRectUV({x + lw, by, 0}, {x + lw + midW, by + cellH, 0}, shTex->id,
                    (float)(cF.x+1)/shTex->width, (float)(cF.y+1)/shTex->height,
                    (float)(cF.x+cF.w-1)/shTex->width, (float)(cF.y+cF.h-1)/shTex->height);
            }
        } else {
            // Gradient-like button using stacked rects (since shell textures unavailable)
            ColorF top = btnFill;
            ColorF bot = {btnFill.r * 0.6f, btnFill.g * 0.6f, btnFill.b * 0.6f, btnFill.a};
            // Constrain visual height to parent bounds so button doesn't overflow toolbar
            float visH = ctl->extentY;
            if (ctl->parent && ctl->parent->extentY > 0 && ctl->parent->extentY < visH)
                visH = ctl->parent->extentY;
            float visY = y + (ctl->extentY - visH) * 0.5f;
            float half = visH * 0.5f;
            r.drawRectFill({x-1, visY-1, 0}, {x + ctl->extentX + 1, visY + visH + 1, 0}, btnBorder);
            r.drawRectFill({x, visY, 0}, {x + ctl->extentX, visY + half, 0}, top);
            r.drawRectFill({x, visY + half, 0}, {x + ctl->extentX, visY + visH, 0}, bot);
        }
        if (!ctl->bitmap.empty()) {
            Texture* tex = r.loadTexture(ctl->bitmap.c_str());
            if (!tex) {
                std::string tryPath = "textures/gui/" + ctl->bitmap;
                tex = r.loadTexture(tryPath.c_str());
                if (!tex) {
                    auto slash = ctl->bitmap.rfind('/');
                    std::string fname = (slash != std::string::npos) ? ctl->bitmap.substr(slash + 1) : ctl->bitmap;
                    if (!fname.empty()) fname[0] = (char)toupper(fname[0]);
                    tex = r.loadTexture(("textures/gui/" + fname).c_str());
                }
            }
            if (tex && tex->loaded)
                r.drawTexturedRect({x+2,y+2,0}, {x+ctl->extentX-2,y+ctl->extentY-2,0}, tex->id);
        }
        if (!ctl->text.empty()) {
            ColorF tc{1,1,1,1};
            Font* bf = font;
            float textOfsX = 4, textOfsY = 0;
            std::string justify = "left";
            auto* prof = getProfile(ctl->profileName);
            if (prof) {
                auto fci = prof->fields.find("fontColor");
                if (fci != prof->fields.end()) {
                    parseColor(fci->second.toString(), tc);
                    if (tc.r + tc.g + tc.b < 0.3f) tc = {1,1,1,1};
                }
                bf = getProfileFont(prof);
                getTextOffset(prof, textOfsX, textOfsY);
                auto ji = prof->fields.find("justify");
                if (ji != prof->fields.end()) justify = ji->second.toString();
            }
            if (!bf) bf = font;
            float tx = x + textOfsX;
            float ty = y + textOfsY;
            float tw = bf->measure(ctl->text.c_str()).x;
            if (justify == "center") tx = x + (ctl->extentX - tw) * 0.5f;
            else if (justify == "right") tx = x + ctl->extentX - tw - textOfsX;
            bf->render(ctl->text.c_str(), tx, ty, tc, 1.0f);
        }
        // Popup menu for ShellLaunchMenu
        if (ctl->menuOpen && !ctl->menuItems.empty()) {
            float popX = x;
            float popY = y - (float)ctl->menuItems.size() * 20.0f - 4; // render above button
            float popW = 180, lineH = 20;
            float popH = lineH * (float)ctl->menuItems.size();
            r.drawRectFill({popX, popY, 0}, {popX + popW, popY + popH, 0}, {0.15f,0.15f,0.2f,0.95f});
            float iy = popY;
            for (auto& item : ctl->menuItems) {
                if (item.isSeparator) {
                    r.drawRectFill({popX + 4, iy + lineH*0.5f, 0}, {popX + popW - 4, iy + lineH*0.5f + 1, 0}, {0.4f,0.4f,0.5f,0.8f});
                } else if (font) {
                    font->render(item.text.c_str(), popX + 6, iy + 2, {0.8f,0.9f,1,1}, 1.0f);
                }
                iy += lineH;
            }
        }
    } else if (cn == "GuiTextCtrl") {
        ColorF tc{1,1,1,1};
        auto* prof = getProfile(ctl->profileName);
        if (prof) {
            auto fci = prof->fields.find("fontColor");
            if (fci != prof->fields.end()) {
                parseColor(fci->second.toString(), tc);
                // Skip profile fontColor if too dark for our dark backgrounds
                if (tc.r + tc.g + tc.b < 0.3f) tc = {1,1,1,1};
            }
            font = getProfileFont(prof);
        }
        if (font && !ctl->text.empty()) {
            float ch = (float)font->charHeight;
            float lineY = y + 2;
            size_t pos = 0;
            while (pos < ctl->text.size()) {
                size_t nl = ctl->text.find('\n', pos);
                std::string line = (nl != std::string::npos) ? ctl->text.substr(pos, nl - pos) : ctl->text.substr(pos);
                font->render(line.c_str(), x + 2, lineY, tc, 1.0f);
                lineY += ch;
                pos = (nl != std::string::npos) ? nl + 1 : ctl->text.size();
            }
        }
    } else if (cn == "GuiMLTextCtrl") {
        // Rich text renderer — parses GuiMLTextCtrl markup tags
        auto* prof = getProfile(ctl->profileName);
        if (prof) font = getProfileFont(prof);
        ColorF curColor{1,1,1,1};
        auto getHexColor = [](const std::string& hex) -> ColorF {
            if (hex.size() < 6) return {1,1,1,1};
            auto h2i = [](char c) -> int { if (c>='0'&&c<='9') return c-'0'; if (c>='a'&&c<='f') return c-'a'+10; if (c>='A'&&c<='F') return c-'A'+10; return 0; };
            int r = h2i(hex[0])*16 + h2i(hex[1]);
            int g = h2i(hex[2])*16 + h2i(hex[3]);
            int b = h2i(hex[4])*16 + h2i(hex[5]);
            return {(float)r/255.0f, (float)g/255.0f, (float)b/255.0f, 1};
        };
        enum class Justify { Left, Center, Right };
        Justify curJust = Justify::Left;
        struct RichSpan { std::string text; std::string fontName; int fontSize{}; ColorF color{1,1,1,1}; Justify justify{Justify::Left}; bool isLink{}; std::string linkTarget; std::string bitmap; };
        // Flush current span into list
        auto flushSpan = [&](std::vector<RichSpan>& s, RichSpan& c) {
            if (c.text.empty() && c.bitmap.empty()) return;
            if (!c.bitmap.empty()) { s.push_back(c); c.bitmap.clear(); return; }
            s.push_back(c);
            c.text.clear();
            c.isLink = false;
            c.linkTarget.clear();
        };
        // Parse text into spans
        auto parseRich = [&](const std::string& src) -> std::vector<RichSpan> {
            std::vector<RichSpan> spans;
            RichSpan cur;
            cur.fontName = font ? font->fontName : "";
            cur.fontSize = font ? font->fontSize : 12;
            cur.color = curColor;
            cur.justify = curJust;
            size_t i = 0;
            while (i < src.size()) {
                if (src[i] == '<') {
                    size_t ce = src.find('>', i);
                    if (ce == std::string::npos) { cur.text += src.substr(i); break; }
                    std::string tag = src.substr(i+1, ce-i-1);
                    i = ce + 1;
                    if (tag == "just:center") { flushSpan(spans, cur); cur.justify = Justify::Center; }
                    else if (tag == "just:left") { flushSpan(spans, cur); cur.justify = Justify::Left; }
                    else if (tag == "just:right") { flushSpan(spans, cur); cur.justify = Justify::Right; }
                    else if (tag.substr(0, 5) == "font:") {
                        flushSpan(spans, cur);
                        auto rest = tag.substr(5);
                        auto cl = rest.rfind(':');
                        if (cl != std::string::npos) { cur.fontName = rest.substr(0, cl); cur.fontSize = atoi(rest.substr(cl+1).c_str()); }
                    }
                    else if (tag.substr(0, 6) == "color:") {
                        flushSpan(spans, cur);
                        cur.color = getHexColor(tag.substr(6));
                    }
                    else if (tag.substr(0, 2) == "a:") {
                        auto tabPos = tag.find('\t');
                        if (tabPos == std::string::npos) tabPos = tag.find("\\tab");
                        if (tabPos != std::string::npos) {
                            flushSpan(spans, cur);
                            cur.isLink = true;
                            cur.linkTarget = tag.substr(tabPos + (tag[tabPos]=='\\'?5:1));
                        }
                    }
                    else if (tag == "/a") { flushSpan(spans, cur); cur.isLink = false; cur.linkTarget.clear(); }
                    else if (tag.substr(0, 8) == "BITMAP:") {
                        flushSpan(spans, cur);
                        RichSpan imgSpan;
                        imgSpan.bitmap = tag.substr(8);
                        spans.push_back(imgSpan);
                    }
                } else if (src[i] == '\n') {
                    flushSpan(spans, cur);
                    // Add an explicit line break by pushing a zero-width marker
                    spans.push_back({"\n", "", 0, {0,0,0,0}, Justify::Left, false, "", ""});
                    i++;
                } else {
                    if (src[i] == '\t') cur.text += ' ';
                    else cur.text += src[i];
                    i++;
                }
            }
            flushSpan(spans, cur);
            return spans;
        };
        auto spans = parseRich(ctl->text);
        // Render spans
        float lineY = y + 2;
        float maxW = ctl->extentX - 4;
        float penX = x + 2;
        int lineStartSpan = 0;
        for (int si = 0; si < (int)spans.size(); ) {
            // Collect spans for this line
            float lineW = 0;
            int sj = si;
            while (sj < (int)spans.size()) {
                auto& sp = spans[sj];
                // Force line break on \n marker
                if (sp.text == "\n") { sj++; break; }
                if (!sp.bitmap.empty()) {
                    Texture* tex = Engine::instance().renderer().loadTexture(sp.bitmap.c_str());
                    float bw = tex ? (float)tex->width : 0;
                    float bh = tex ? (float)tex->height : 0;
                    if (lineW + bw > maxW) break;
                    lineW += bw; sj++;
                    continue;
                }
                Font* spf = Engine::instance().renderer().getFont(sp.fontName.c_str(), sp.fontSize);
                float adv = spf ? spf->measure(sp.text.c_str(), 1.0f).x : (float)sp.text.size() * 8;
                if (lineW + adv > maxW && lineW > 0) break;
                // Check for word break
                auto ws = sp.text.rfind(' ');
                if (ws != std::string::npos && lineW > 0) {
                    std::string before = sp.text.substr(0, ws);
                    float bfW = spf ? spf->measure(before.c_str(), 1.0f).x : (float)before.size() * 8;
                    if (lineW + bfW > maxW) break;
                }
                lineW += adv; sj++;
            }
            if (sj == si) sj = si + 1; // at least one span
            // Determine line width for justification
            float actualW = 0;
            for (int k = si; k < sj; k++) {
                auto& sp = spans[k];
                if (!sp.bitmap.empty()) {
                    Texture* tex = Engine::instance().renderer().loadTexture(sp.bitmap.c_str());
                    actualW += tex ? (float)tex->width : 0;
                } else {
                    Font* spf = Engine::instance().renderer().getFont(sp.fontName.c_str(), sp.fontSize);
                    actualW += spf ? spf->measure(sp.text.c_str(), 1.0f).x : (float)sp.text.size() * 8;
                }
            }
            float lineStartX = x + 2;
            if (spans[si].justify == Justify::Center) lineStartX = x + (maxW - actualW) * 0.5f;
            else if (spans[si].justify == Justify::Right) lineStartX = x + maxW - actualW;
            // Render spans on this line
            penX = lineStartX;
            float lineH = (float)font->charHeight;
            for (int k = si; k < sj; k++) {
                auto& sp = spans[k];
                if (!sp.bitmap.empty()) {
                    Texture* tex = Engine::instance().renderer().loadTexture(sp.bitmap.c_str());
                    if (tex && tex->loaded) {
                        float bw = (float)tex->width, bh = (float)tex->height;
                        Engine::instance().renderer().drawTexturedRectUV({penX, lineY, 0}, {penX+bw, lineY+bh, 0}, tex->id, 0, 0, 1, 1);
                        penX += bw;
                    }
                    continue;
                }
                Font* spf = Engine::instance().renderer().getFont(sp.fontName.c_str(), sp.fontSize);
                if (!spf) spf = font;
                if (spf) {
                    float lyOff = 0;
                    if (spf->charHeight < lineH) lyOff = (lineH - spf->charHeight) * 0.5f;
                    ColorF col = sp.color;
                    if (sp.isLink) { col = {0.3f, 0.7f, 1, 1}; }
                    spf->render(sp.text.c_str(), penX, lineY + lyOff, col, 1.0f);
                    penX += spf->measure(sp.text.c_str(), 1.0f).x;
                }
            }
            lineY += lineH;
            si = sj;
        }
    } else if (cn == "GuiBitmapCtrl" || cn == "GuiChunkedBitmapCtrl" || cn == "GuiFadeinBitmapCtrl") {
        std::string bmpPath;
        auto* prof = getProfile(ctl->profileName);
        // Support useVariable: bitmap path from a console variable
        auto* sobj = ScriptEngine::instance().findObject(ctl->name.c_str());
        if (sobj) {
            auto uv = sobj->fields.find("useVariable");
            if (uv != sobj->fields.end() && uv->second.toBool()) {
                auto vi = sobj->fields.find("variable");
                if (vi != sobj->fields.end())
                    bmpPath = Console::instance().getStringVariable(vi->second.toString().c_str(), "");
            }
        }
        if (bmpPath.empty() && prof) { auto bi = prof->fields.find("bitmap"); if (bi != prof->fields.end()) bmpPath = bi->second.toString(); }
        if (bmpPath.empty()) bmpPath = ctl->bitmap;
        ColorF bgColor{0.15f, 0.15f, 0.2f, 1};
        if (prof) { auto bi = prof->fields.find("fillColor"); if (bi != prof->fields.end()) parseColor(bi->second.toString(), bgColor); }
        if (!bmpPath.empty()) {
            auto tryLoad = [&](const std::string& p) -> Texture* {
                Texture* t = r.loadTexture(p.c_str());
                if (!t) { std::string c = p; if (!c.empty()) { c[0] = (char)toupper(c[0]); t = r.loadTexture(c.c_str()); } }
                return t;
            };
            Texture* tex = tryLoad(bmpPath + ".png");
            if (!tex) tex = tryLoad("textures/" + bmpPath + ".png");
            if (!tex) tex = tryLoad("textures/gui/" + bmpPath + ".png");
            if (tex && tex->loaded)
                r.drawTexturedRect({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, tex->id);
            else
                r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, bgColor);
        } else if (strcmp(ctl->name.c_str(), "LaunchGui") == 0) {
            // LaunchGui background: try menu background, then gradient
            Texture* bgTex = r.loadTexture("menu/background.png");
            if (bgTex && bgTex->loaded)
                r.drawTexturedRect({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, bgTex->id);
            else
                r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.08f,0.08f,0.12f,1});
        } else {
            r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, bgColor);
        }
        // Fade overlay for GuiFadeinBitmapCtrl
        if (cn == "GuiFadeinBitmapCtrl") {
            FadeState* fs = gr->getFadeState(ctl, false);
            if (fs && fs->currentAlpha < 1.0f)
                r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0,0,0, 1.0f - fs->currentAlpha});
        }
    } else if (cn == "GuiTextEditCtrl" || cn == "ShellTextEditCtrl" ||
               cn == "GuiLoginPasswordCtrl") {
        ColorF fc{0.3f,0.3f,0.35f,1}, tc{0.8f,0.8f,1,1};
        std::string bmp;
        auto* prof = getProfile(ctl->profileName);
        if (prof) {
            auto fi = prof->fields.find("fillColor"); if (fi != prof->fields.end()) parseColor(fi->second.toString(), fc);
            auto fci = prof->fields.find("fontColor"); if (fci != prof->fields.end()) parseColor(fci->second.toString(), tc);
            auto bi = prof->fields.find("bitmap"); if (bi != prof->fields.end()) bmp = bi->second.toString();
        }
        Texture* fieldTex = nullptr;
        if (!bmp.empty()) { fieldTex = r.loadTexture((bmp + ".png").c_str()); if (!fieldTex) fieldTex = r.loadTexture(("textures/" + bmp + ".png").c_str()); }
        // Shell entry field: same row-major 3-slice layout
        const std::vector<BmpCell>* fieldCells = nullptr;
        Texture* fieldTex2 = getShellTexWithCells(r, "shll_entryfield.png", fieldCells);
        if (fieldTex2 && fieldCells && fieldCells->size() >= 3) {
            int state = 0;
            auto& cL = (*fieldCells)[state];
            auto& cF = (*fieldCells)[3 + state];
            auto& cR = (*fieldCells)[6 + state];
            float cellH = (float)cL.h, lw = (float)cL.w, rw = (float)cR.w, midW = ctl->extentX - lw - rw;
            if (midW < 0) midW = 0;
            float fy = y + (ctl->extentY - cellH) * 0.5f;
            r.drawTexturedRectUV({x,fy,0},{x+lw,fy+cellH,0}, fieldTex2->id,
                (float)cL.x/fieldTex2->width,(float)cL.y/fieldTex2->height,
                (float)(cL.x+cL.w)/fieldTex2->width,(float)(cL.y+cL.h)/fieldTex2->height);
            r.drawTexturedRectUV({x+lw+midW,fy,0},{x+lw+midW+rw,fy+cellH,0}, fieldTex2->id,
                (float)cR.x/fieldTex2->width,(float)cR.y/fieldTex2->height,
                (float)(cR.x+cR.w)/fieldTex2->width,(float)(cR.y+cR.h)/fieldTex2->height);
            // Fill center (stretched, inset 1px)
            r.drawTexturedRectUV({x+lw,fy,0},{x+lw+midW,fy+cellH,0}, fieldTex2->id,
                (float)(cF.x+1)/fieldTex2->width,(float)(cF.y+1)/fieldTex2->height,
                (float)(cF.x+cF.w-1)/fieldTex2->width,(float)(cF.y+cF.h-1)/fieldTex2->height);
        } else {
            r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, fc);
        }
        if (font) {
            std::string display = ctl->text.empty() ? "..." : ctl->text;
            font->render(display.c_str(), x + 3, y + 2, tc, 1.0f);
            // Cursor when focused
            if (ctl == gr->getFocused()) {
                float preW = font->measure(ctl->text.substr(0, ctl->cursorPos).c_str()).x;
                float cy = y + 2;
                float ch = (float)font->charHeight;
                r.drawRectFill({x + 3 + preW, cy, 0}, {x + 4 + preW, cy + ch, 0}, {1,1,1,1});
            }
        }
    } else if (cn == "GuiListBoxCtrl" || cn == "ShellTextList" || cn == "GuiTextListCtrl") {
        ColorF fc{0.15f,0.15f,0.2f,1}, tc{0.9f,0.9f,1,1}, selFc{0.3f,0.3f,0.5f,1}, selTc{1,1,1,1};
        auto* prof = getProfile(ctl->profileName);
        if (prof) {
            auto fi = prof->fields.find("fillColor"); if (fi != prof->fields.end()) parseColor(fi->second.toString(), fc);
            auto fci = prof->fields.find("fontColor"); if (fci != prof->fields.end()) parseColor(fci->second.toString(), tc);
            auto sfi = prof->fields.find("selectionColor"); if (sfi != prof->fields.end()) parseColor(sfi->second.toString(), selFc);
        }
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, fc);
        // Find scroll offset from parent ShellScrollCtrl
        float listScrollY = 0, listScrollX = 0;
        GuiControl* sp = ctl->parent;
        while (sp) {
            if (sp->className == "GuiScrollCtrl" || sp->className == "ShellScrollCtrl") {
                listScrollY = sp->scrollY;
                listScrollX = sp->scrollX;
                break;
            }
            sp = sp->parent;
        }
        // Get font height
        float lineH = font ? font->charHeight + 2 : 14;
        // Get enumerate setting from ScriptObject
        bool enumerate = false;
        ScriptObject* sobj = ScriptEngine::instance().findObject(ctl->name.c_str());
        if (sobj) {
            auto ei = sobj->internals.find("enumerate");
            if (ei == sobj->internals.end()) ei = sobj->fields.find("enumerate");
            if (ei != sobj->internals.end()) enumerate = ei->second.toInt() != 0;
        }
        // Draw visible rows
        int visibleRows = (int)(ctl->extentY / lineH) + 1;
        int totalRows = (int)ctl->listRows.size();
        int scrollRow = (int)(listScrollY / lineH);
        float rowY = y - fmodf(listScrollY, lineH);
        for (int i = scrollRow; i < totalRows && (i - scrollRow) < visibleRows; i++, rowY += lineH) {
            bool isSel = (i == ctl->selectedRow);
            if (isSel) {
                r.drawRectFill({x, rowY, 0}, {x + ctl->extentX, rowY + lineH, 0}, selFc);
                font->render(ctl->listRows[i].c_str(), x + 3, rowY + 1, selTc, 1.0f);
            } else {
                font->render(ctl->listRows[i].c_str(), x + 3, rowY + 1, tc, 1.0f);
            }
        }
    } else if (cn == "GuiCheckBoxCtrl" || cn == "GuiRadioCtrl" ||
               cn == "ShellToggleButton" || cn == "ShellRadioButton") {
        ColorF fc{0.5f,0.5f,0.6f,1}, tc{1,1,1,1};
        std::string bmp;
        auto* prof = getProfile(ctl->profileName);
        if (prof) {
            auto fci = prof->fields.find("fontColor"); if (fci != prof->fields.end()) parseColor(fci->second.toString(), tc);
            auto bi = prof->fields.find("bitmap"); if (bi != prof->fields.end()) bmp = bi->second.toString();
        }
        float sz = 16;
        Texture* cbTex = nullptr;
        if (!bmp.empty()) { cbTex = r.loadTexture((bmp + ".png").c_str()); if (!cbTex) cbTex = r.loadTexture(("textures/" + bmp + ".png").c_str()); }
        if (!cbTex) cbTex = getShellTex(r, "shll_radio.png");
        if (cbTex && cbTex->loaded) {
            r.drawTexturedRect({x, y, 0}, {x + sz, y + sz, 0}, cbTex->id);
            if (ctl->checked)
                r.drawRectFill({x + 4, y + 4, 0}, {x + sz - 4, y + sz - 4, 0}, {0.3f,0.5f,0.8f,0.8f});
        } else {
            ColorF bg = ctl->checked ? ColorF{0.3f,0.5f,0.8f,1} : fc;
            r.drawRectFill({x, y, 0}, {x + sz, y + sz, 0}, bg);
        }
        if (font && !ctl->text.empty())
            font->render(ctl->text.c_str(), x + sz + 4, y + 2, tc, 1.0f);
    } else if (cn == "GuiSliderCtrl" || cn == "ShellSliderCtrl") {
        ColorF fc{0.4f,0.4f,0.5f,1}, kc{0.7f,0.7f,0.8f,1};
        auto* prof = getProfile(ctl->profileName);
        if (prof) { auto fi = prof->fields.find("fillColor"); if (fi != prof->fields.end()) parseColor(fi->second.toString(), fc); }
        float barY = y + ctl->extentY * 0.4f;
        float barH = ctl->extentY * 0.2f;
        float knobX = x + ctl->extentX * 0.5f;
        r.drawRectFill({x, barY, 0}, {x + ctl->extentX, barY + barH, 0}, fc);
        r.drawRectFill({knobX-4, y, 0}, {knobX+4, y+ctl->extentY, 0}, kc);
    } else if (cn == "GuiProgressCtrl") {
        ColorF bg{0.2f,0.2f,0.25f,1}, fg{0.0f,0.4f,0.8f,1};
        auto* prof = getProfile(ctl->profileName);
        if (prof) { auto fi = prof->fields.find("fillColor"); if (fi != prof->fields.end()) parseColor(fi->second.toString(), bg); }
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, bg);
        r.drawRectFill({x, y, 0}, {x + ctl->extentX * 0.5f, y + ctl->extentY, 0}, fg);
    } else if (cn == "GuiConsole") {
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0, 0, 0, 0.75f});
        if (font) {
            float scrollOfsY = 0, scrollOfsX = 0;
            GuiControl* sp = ctl->parent;
            while (sp) {
                if (sp->className == "GuiScrollCtrl" || sp->className == "ShellScrollCtrl") {
                    scrollOfsY = sp->scrollY;
                    scrollOfsX = sp->scrollX;
                    break;
                }
                sp = sp->parent;
            }
            auto& lines = Console::instance().getLog();
            float lh = 12;
            int visLines = (int)(ctl->extentY / lh);
            int totalLines = (int)lines.size();
            int topLine = totalLines - visLines - (int)(scrollOfsY / lh);
            if (topLine < 0) topLine = 0;
            float ly = y;
            float hScroll = scrollOfsX;
            for (int i = topLine; i < totalLines && (i - topLine) < visLines; i++, ly += lh) {
                ColorF col{0.7f, 0.7f, 0.7f, 0.9f};
                const std::string& line = lines[i];
                if (line.find("[ERROR]") == 0)       col = {1, 0.2f, 0.2f, 0.9f};
                else if (line.find("[WARN]") == 0)   col = {1, 0.8f, 0.2f, 0.9f};
                else if (line.find("[INFO]") == 0)   col = {0.7f, 0.7f, 1, 0.9f};
                else if (line.find("[DEBUG]") == 0)  col = {0.5f, 0.5f, 0.5f, 0.7f};
                font->render(line.c_str(), x + 4 - hScroll, ly, col, 1.5f);
            }
        }
    } else if (cn == "GuiConsoleEditCtrl") {
        ColorF bg{0,0,0,0.85f}, tc{0,1,0,1};
        auto* prof = getProfile(ctl->profileName);
        if (prof) {
            auto fi = prof->fields.find("fillColor"); if (fi != prof->fields.end()) parseColor(fi->second.toString(), bg);
            auto fci = prof->fields.find("fontColor"); if (fci != prof->fields.end()) parseColor(fci->second.toString(), tc);
        }
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, bg);
        if (font) {
            std::string prompt = "> " + ctl->text;
            static float cursorTimer = 0;
            cursorTimer += 0.02f;
            bool cursorOn = ((int)(cursorTimer / 0.4f) % 2) == 0;
            if (cursorOn) prompt += '_';
            font->render(prompt.c_str(), x + 4, y + 3, tc, 1.5f);
        }
    } else if (cn == "ShellPaneCtrl" || cn == "GuiPaneControl" ||
               cn.find("ShellPane") == 0 || cn == "ShellDlgFrame") {
        ColorF fc{0.12f,0.12f,0.15f,1.0f}, txc{1,1,1,1};
        auto* prof = getProfile(ctl->profileName);
        if (prof) {
            auto fi = prof->fields.find("fillColor"); if (fi != prof->fields.end()) parseColor(fi->second.toString(), fc);
            // Don't use profile fontColor for panes — it's designed for the original game's lighter title bars
        }
        if (fc.a < 1.0f) fc.a = 1.0f; // force opaque
        // Opaque dark background behind everything
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.12f, 0.12f, 0.15f, 1.0f});
        // Fill background with fieldfill texture (small tile on top)
        const std::vector<BmpCell>* fillCells = nullptr;
        Texture* fillTex = getShellTexWithCells(r, "dlg_fieldfill.png", fillCells);
        if (fillTex && fillTex->loaded) {
            auto& src = fillCells && fillCells->size() >= 1 ? (*fillCells)[0] : BmpCell{0,0,fillTex->width,fillTex->height};
            float u0 = (float)src.x/fillTex->width, v0 = (float)src.y/fillTex->height;
            float u1 = (float)(src.x+src.w)/fillTex->width, v1 = (float)(src.y+src.h)/fillTex->height;
            GLint oldS, oldT; glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &oldS); glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &oldT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            auto* ss = ShaderManager::getSpriteShader(); if (ss) {
                ss->bind(); ss->setUniform("uProjection",r.projection); ss->setUniform("uView",r.view);
                ss->setUniform("uUseTexture",int32_t(1)); ss->setUniform("uTexture",int32_t(0));
                glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, fillTex->id);
                struct SV{float x,y,z;float u,v;float r,g,b,a;};
                SV v[4]={{x,y,0,0,0,1,1,1,1},{x+ctl->extentX,y,0,ctl->extentX/src.w,0,1,1,1,1},{x,y+ctl->extentY,0,0,ctl->extentY/src.h,1,1,1,1},{x+ctl->extentX,y+ctl->extentY,0,ctl->extentX/src.w,ctl->extentY/src.h,1,1,1,1}};
                uint32_t ids[]={0,1,2,1,3,2};
                drawQuadBatch(v, sizeof(v), ids, sizeof(ids));
            }
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,oldS); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,oldT);
        } else r.drawRectFill({x,y,0},{x+ctl->extentX,y+ctl->extentY,0},fc);
        // Load textures for frame edges and title tab
        const std::vector<BmpCell>* edgeCells = nullptr;
        Texture* edgeTex = getShellTexWithCells(r, "dlg_frame_edge.png", edgeCells);
        const std::vector<BmpCell>* tabCells = nullptr;
        Texture* tabTex = getShellTexWithCells(r, "dlg_titletab.png", tabCells);
        bool hasTitle = !ctl->text.empty() && tabTex && tabTex->loaded;
        // Frame edges using dlg_frame_edge (top edge skipped if title tab present)
        if (edgeTex && edgeTex->loaded) {
            auto doEdge = [&](float ex,float ey,float ew,float eh, const BmpCell& s, bool tileX, bool tileY) {
                float u0=(float)s.x/edgeTex->width,v0=(float)s.y/edgeTex->height;
                float u1=(float)(s.x+s.w)/edgeTex->width,v1=(float)(s.y+s.h)/edgeTex->height;
                GLint ow; glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,&ow);
                if(tileX)glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
                if(tileY)glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
                auto*ss2=ShaderManager::getSpriteShader(); if(ss2){
                    ss2->bind();ss2->setUniform("uProjection",r.projection);ss2->setUniform("uView",r.view);
                    ss2->setUniform("uUseTexture",int32_t(1));ss2->setUniform("uTexture",int32_t(0));
                    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,edgeTex->id);
                    struct SV{float x,y,z;float u,v;float r,g,b,a;};
                    float ru=tileX?ew/s.w:u1-u0, rv=tileY?eh/s.h:v1-v0;
                    SV v[4]={{ex,ey,0,0,0,1,1,1,1},{ex+ew,ey,0,ru,0,1,1,1,1},{ex,ey+eh,0,0,rv,1,1,1,1},{ex+ew,ey+eh,0,ru,rv,1,1,1,1}};
                    uint32_t ids[]={0,1,2,1,3,2};
                    drawQuadBatch(v, sizeof(v), ids, sizeof(ids));
                }
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,ow);
            };
            BmpCell ec{0,0,edgeTex->width,edgeTex->height};
            if(edgeCells&&edgeCells->size()>=1) ec=(*edgeCells)[0];
            if (!hasTitle) doEdge(x,y,ctl->extentX,ec.h,ec,true,false); // top edge only if no title
            doEdge(x,y+ctl->extentY-ec.h,ctl->extentX,ec.h,ec,true,false); // bottom
            doEdge(x,y,ec.w,ctl->extentY,ec,false,true); // left
            doEdge(x+ctl->extentX-ec.w,y,ec.w,ctl->extentY,ec,false,true); // right
        }
        // Title tab (3-strip or full stretch)
        if (!ctl->text.empty() && tabTex && tabTex->loaded && tabCells && tabCells->size() >= 3) {
            int st = 0;
            auto& cL=(*tabCells)[st],&cM=(*tabCells)[1+st],&cR=(*tabCells)[2+st];
            float th=(float)tabTex->height, txtW=(float)ctl->text.size()*8.0f*1.0f+16;
            float lw=(float)cL.w, rw=(float)cR.w, midW=std::max(txtW-lw-rw,0.0f);
            auto* ss3=ShaderManager::getSpriteShader(); if(ss3){
                ss3->bind();ss3->setUniform("uProjection",r.projection);ss3->setUniform("uView",r.view);
                ss3->setUniform("uUseTexture",int32_t(1));ss3->setUniform("uTexture",int32_t(0));
                glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,tabTex->id);
                struct SV{float x,y,z;float u,v;float r,g,b,a;};
                float ty=y-th*0.5f, bh=th;
                uint32_t ids[]={0,1,2,1,3,2};
                auto dq=[&](float qx,float qy,float qw,float qh,const BmpCell& s,bool tx){
                    if(qw<=0||qh<=0)return;
                    float u0=(float)s.x/tabTex->width,v0=(float)s.y/tabTex->height;
                    float u1=(float)(s.x+s.w)/tabTex->width,v1=(float)(s.y+s.h)/tabTex->height;
                    if(tx){
                        GLint ow;glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,&ow);
                        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
                        float rp=qw/s.w;
                        SV v[4]={{qx,qy,0,0,v0,1,1,1,1},{qx+qw,qy,0,rp,v0,1,1,1,1},{qx,qy+qh,0,0,v1,1,1,1,1},{qx+qw,qy+qh,0,rp,v1,1,1,1,1}};
                        drawQuadBatch(v, sizeof(v), ids, sizeof(ids));
                        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,ow);
                    } else {
                        SV v[4]={{qx,qy,0,u0,v0,1,1,1,1},{qx+qw,qy,0,u1,v0,1,1,1,1},{qx,qy+qh,0,u0,v1,1,1,1,1},{qx+qw,qy+qh,0,u1,v1,1,1,1,1}};
                        drawQuadBatch(v, sizeof(v), ids, sizeof(ids));
                    }
                };
                dq(x+4,ty,lw,bh,cL,false); dq(x+4+lw,ty,midW,bh,cM,true); dq(x+4+lw+midW,ty,rw,bh,cR,false);
                if (font) {
                    float tx = x+4+lw+4;
                    font = getProfileFont(prof);
                    float toX, toY;
                    if (getTextOffset(prof, toX, toY)) { tx += toX; }
                    font->render(ctl->text.c_str(), tx, ty+bh*0.2f, txc, 1.0f);
                }
            }
        } else if (!ctl->text.empty()) {
            // Title tab: 2 cells = top row (left cap), bottom row (fill). Right cap = left cap flipped.
            if (tabTex && tabTex->loaded && tabCells && tabCells->size() >= 2) {
                auto& cL = (*tabCells)[0]; // left cap cell
                auto& cF = (*tabCells)[1]; // fill cell
                float th = (float)cL.h; // use cell height (28), not full texture height (58)
                float lw = (float)cL.w, midW = ctl->extentX - lw * 2;
                if (midW < 0) midW = 0;
                // Left cap at native cell size
                r.drawTexturedRectUV({x, y, 0}, {x + lw, y + th, 0}, tabTex->id,
                    (float)cL.x/tabTex->width, (float)cL.y/tabTex->height,
                    (float)(cL.x+cL.w)/tabTex->width, (float)(cL.y+cL.h)/tabTex->height);
                // Right cap (left cap flipped horizontally)
                r.drawTexturedRectUV({x + ctl->extentX - lw, y, 0}, {x + ctl->extentX, y + th, 0}, tabTex->id,
                    (float)(cL.x+cL.w)/tabTex->width, (float)cL.y/tabTex->height,
                    (float)cL.x/tabTex->width, (float)(cL.y+cL.h)/tabTex->height);
                // Middle fill stretched to cell height
                r.drawTexturedRectUV({x + lw, y, 0}, {x + lw + midW, y + th, 0}, tabTex->id,
                    (float)cF.x/tabTex->width, (float)cF.y/tabTex->height,
                    (float)(cF.x+cF.w)/tabTex->width, (float)(cF.y+cF.h)/tabTex->height);
                if (font) {
                    font = getProfileFont(prof);
                    float tx = x + lw + 4, ty = y + 2;
                    float toX, toY;
                    if (getTextOffset(prof, toX, toY)) { tx += toX; ty += toY; }
                    font->render(ctl->text.c_str(), tx, ty, txc, 1.0f);
                }
            } else if (tabTex && tabTex->loaded) {
                float th = (float)tabTex->height;
                r.drawTexturedRect({x, y, 0}, {x + ctl->extentX, y + th, 0}, tabTex->id);
                if (font) {
                    font = getProfileFont(prof);
                    float tx = x + 8, ty = y + 2;
                    float toX, toY;
                    if (getTextOffset(prof, toX, toY)) { tx += toX; ty += toY; }
                    font->render(ctl->text.c_str(), tx, ty, txc, 1.0f);
                }
            } else if (font) {
                font->render(ctl->text.c_str(), x + 6, y + 4, txc, 1.5f);
            }
        }
    } else if (cn == "ShellTabButton" || cn == "GuiTabPageCtrl") {
        // For GuiTabPageCtrl (content pane): only render if it's the selected tab page
        if (cn == "GuiTabPageCtrl") {
            // Find parent tab group and check if this page is the active one
            GuiControl* parent = ctl->parent;
            bool isSelected = false;
            if (parent && parent->selectedTab >= 0) {
                int pageIdx = 0;
                for (auto* sib : parent->children) {
                    if (sib->className == "GuiTabPageCtrl") {
                        if (sib == ctl) {
                            isSelected = (pageIdx == parent->selectedTab);
                            break;
                        }
                        pageIdx++;
                    }
                }
            }
            if (!isSelected) {
                return; // skip children too — page content hidden
            }
        }
        ColorF fc{0.25f,0.25f,0.32f,1}, bc{0.35f,0.35f,0.45f,1}, txc{1,1,1,1};
        std::string bmp, bmpBase;
        float textOfsX = 4, textOfsY = 0;
        auto* prof = getProfile(ctl->profileName);
        if (prof) {
            auto fi = prof->fields.find("fillColor"); if (fi != prof->fields.end()) parseColor(fi->second.toString(), fc);
            auto fci = prof->fields.find("fontColor"); if (fci != prof->fields.end()) parseColor(fci->second.toString(), txc);
            auto bi = prof->fields.find("bitmap"); if (bi != prof->fields.end()) bmp = bi->second.toString();
            auto bbi = prof->fields.find("bitmapBase"); if (bbi != prof->fields.end()) bmpBase = bbi->second.toString();
            auto toi = prof->fields.find("textOffset"); if (toi != prof->fields.end()) sscanf(toi->second.toString().c_str(), "%f %f", &textOfsX, &textOfsY);
        }
        Texture* tabTex = nullptr;
        auto loadTex = [&](const std::string& p) {
            if (p.empty()) return;
            tabTex = r.loadTexture((p + ".png").c_str());
            if (!tabTex) tabTex = r.loadTexture(("textures/" + p + ".png").c_str());
            if (!tabTex) tabTex = r.loadTexture(("textures/gui/" + p + ".png").c_str());
        };
        if (!bmpBase.empty()) loadTex(bmpBase);
        if (!bmp.empty() && !tabTex) loadTex(bmp);
        if (cn == "ShellTabButton") {
            // Tab button: determine selected state from parent tab group
            bool isSelected = ctl->selected;
            if (!isSelected && ctl->parent && ctl->parent->selectedTab >= 0) {
                int btnIdx = 0;
                for (auto* sib : ctl->parent->children) {
                    if (sib->className == "ShellTabButton") {
                        if (sib == ctl) { isSelected = (btnIdx == ctl->parent->selectedTab); break; }
                        btnIdx++;
                    }
                }
            }
            // Tab button with 3D-ish look
            ColorF fill = isSelected ? ColorF{0.35f,0.45f,0.55f,1} : fc;
            r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, fill);
            // Bottom highlight line for selected tab (extends below to overlap pane border)
            if (isSelected)
                r.drawRectFill({x, y + ctl->extentY - 1, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.6f,0.7f,0.8f,1});
            else
                r.drawRectFill({x, y + ctl->extentY - 1, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.15f,0.15f,0.2f,1});
            if (font && !ctl->text.empty()) {
                float tx2 = x + textOfsX;
                float ty2 = y + (ctl->extentY - (float)font->charHeight) * 0.5f + textOfsY;
                font->render(ctl->text.c_str(), tx2, ty2, txc, 1.0f);
            }
        } else {
            // GuiTabPageCtrl (selected content pane): draw dark background and content
            r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.12f,0.12f,0.15f,1});
            // Thin border at top to separate from tab bar
            r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + 1, 0}, {0.3f,0.3f,0.4f,0.5f});
            if (font && !ctl->text.empty()) {
                float tx2 = x + textOfsX;
                float ty2 = y + textOfsY;
                font->render(ctl->text.c_str(), tx2, ty2, txc, 1.0f);
            }
        }
    } else if (cn == "GuiPopUpMenuCtrl" || cn == "ShellPopupMenu") {
        ColorF fc{0.25f,0.25f,0.3f,1}, txc{0.6f,0.9f,1,1};
        auto* prof = getProfile(ctl->profileName);
        if (prof) {
            auto fi = prof->fields.find("fillColor"); if (fi != prof->fields.end()) parseColor(fi->second.toString(), fc);
            // Ignore profile fontColor (usually dark/black in original game profiles)
        }
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, fc);
        r.drawRectFill({x + ctl->extentX - 16, y + 4, 0}, {x + ctl->extentX - 4, y + ctl->extentY - 4, 0}, {0.5f,0.5f,0.6f,1});
        if (font && !ctl->text.empty())
            font->render(ctl->text.c_str(), x + 4, y + 2, txc, 1.0f);
        // Dropdown list when open
        if (ctl->menuOpen && !ctl->menuItems.empty()) {
            float popX = x;
            float popY = y + ctl->extentY;
            float popW = ctl->extentX, lineH = 20;
            float popH = lineH * (float)ctl->menuItems.size();
            r.drawRectFill({popX, popY, 0}, {popX + popW, popY + popH, 0}, {0.15f,0.15f,0.2f,0.95f});
            float iy = popY;
            for (auto& item : ctl->menuItems) {
                if (item.isSeparator) {
                    r.drawRectFill({popX + 4, iy + lineH*0.5f, 0}, {popX + popW - 4, iy + lineH*0.5f + 1, 0}, {0.4f,0.4f,0.5f,0.8f});
                } else if (font) {
                    bool sel = (item.text == ctl->text);
                    ColorF ic = sel ? ColorF{0.3f,0.5f,0.8f,0.9f} : ColorF{0.8f,0.9f,1,1};
                    font->render(item.text.c_str(), popX + 6, iy + 2, ic, 1.0f);
                }
                iy += lineH;
            }
        }
    } else if (cn == "GuiScrollCtrl" || cn == "ShellScrollCtrl") {
        ColorF sc{0.18f,0.18f,0.22f,1};
        auto* prof = getProfile(ctl->profileName);
        if (prof) { auto fi = prof->fields.find("fillColor"); if (fi != prof->fields.end()) parseColor(fi->second.toString(), sc); }
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, sc);

        // Save old content height to detect if user was at bottom
        float oldContentH = ctl->contentH;
        computeContentExtent(ctl);

        // Auto-scroll: if at bottom (scrollY=0), stay at bottom as new content arrives
        if (oldContentH > 0 && ctl->scrollY < 1.0f)
            ctl->scrollY = 0;

        // Clamp scroll: scrollY=0 = bottom (newest), scrollY=maxScroll = top (oldest)
        float maxScroll = ctl->contentH - ctl->extentY;
        if (maxScroll < 0) maxScroll = 0;
        if (ctl->scrollY > maxScroll) ctl->scrollY = maxScroll;
        if (ctl->scrollY < 0) ctl->scrollY = 0;

        // Set up clip rect for children
        ClipRect childClip = {x, y, ctl->extentX, ctl->extentY};
        bool useScissor = false;
        GLboolean scissorWas = glIsEnabled(GL_SCISSOR_TEST);
        GLint oldScissor[4];
        if (clip) {
            // Intersect with existing clip
            float cx = (x > clip->x) ? x : clip->x;
            float cy = (y > clip->y) ? y : clip->y;
            float cxe = (x + ctl->extentX < clip->x + clip->w) ? x + ctl->extentX : clip->x + clip->w;
            float cye = (y + ctl->extentY < clip->y + clip->h) ? y + ctl->extentY : clip->y + clip->h;
            if (cxe > cx && cye > cy) {
                childClip = {cx, cy, cxe - cx, cye - cy};
                useScissor = true;
            }
        } else {
            useScissor = true;
        }
        if (useScissor) {
            auto& plat = Engine::instance().platform();
            int winH = plat.height();
            glGetIntegerv(GL_SCISSOR_BOX, oldScissor);
            glEnable(GL_SCISSOR_TEST);
            glScissor((GLint)childClip.x, (GLint)(winH - childClip.y - childClip.h),
                      (GLint)childClip.w, (GLint)childClip.h);
        }

        // Render children at normal positions (each control uses scrollY directly)
        for (auto* child : ctl->children)
            renderControlRec(gr, child, canvas, 0, 0, &childClip);

        // Restore scissor
        if (useScissor) {
            if (scissorWas)
                glScissor(oldScissor[0], oldScissor[1], oldScissor[2], oldScissor[3]);
            else
                glDisable(GL_SCISSOR_TEST);
        }

        // Scrollbar thumb (vertical) with shell texture
        auto* vBarTex = getShellTex(r, "shll_scroll_vertbar.png");
        auto* vFieldTex = getShellTex(r, "shll_scroll_vertfield.png");
        if (maxScroll > 0) {
            float thumbH = ctl->extentY * (ctl->extentY / ctl->contentH);
            if (thumbH < 16) thumbH = 16;
            if (thumbH > ctl->extentY) thumbH = ctl->extentY;
            float thumbY = (1.0f - ctl->scrollY / maxScroll) * (ctl->extentY - thumbH);
            float sbW = 12;
            // Scroll field background
            if (vFieldTex && vFieldTex->loaded)
                r.drawTexturedRect({x + ctl->extentX - sbW, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, vFieldTex->id);
            // Scrollbar thumb
            if (vBarTex && vBarTex->loaded)
                r.drawTexturedRect({x + ctl->extentX - sbW, y + thumbY, 0}, {x + ctl->extentX, y + thumbY + thumbH, 0}, vBarTex->id);
            else
                r.drawRectFill({x + ctl->extentX - sbW, y + thumbY, 0}, {x + ctl->extentX, y + thumbY + thumbH, 0}, {0.5f, 0.5f, 0.6f, 0.7f});
        }
        // Horizontal scrollbar with shell texture
        auto* hBarTex = getShellTex(r, "shll_scroll_horzbar.png");
        auto* hFieldTex = getShellTex(r, "shll_scroll_horzfield.png");
        float maxScrollX = ctl->contentW - ctl->extentX;
        if (maxScrollX > 0) {
            ctl->scrollX = (ctl->scrollX < 0) ? 0 : ctl->scrollX;
            if (ctl->scrollX > maxScrollX) ctl->scrollX = maxScrollX;
            float thumbW = ctl->extentX * (ctl->extentX / ctl->contentW);
            if (thumbW < 16) thumbW = 16;
            if (thumbW > ctl->extentX) thumbW = ctl->extentX;
            float thumbX = (1.0f - ctl->scrollX / maxScrollX) * (ctl->extentX - thumbW);
            float sbH = 12;
            float sbY2 = y + ctl->extentY - sbH;
            if (hFieldTex && hFieldTex->loaded)
                r.drawTexturedRect({x, sbY2, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, hFieldTex->id);
            if (hBarTex && hBarTex->loaded)
                r.drawTexturedRect({x + thumbX, sbY2, 0}, {x + thumbX + thumbW, sbY2 + sbH, 0}, hBarTex->id);
            else
                r.drawRectFill({x + thumbX, sbY2, 0}, {x + thumbX + thumbW, sbY2 + sbH, 0}, {0.5f, 0.5f, 0.6f, 0.7f});
        }
        return; // Don't do default child rendering below
    } else if (cn == "GuiServerBrowser") {
        // Server browser: table with column headers and rows
        ColorF bg{0.15f,0.15f,0.2f,1}, headerBg{0.25f,0.25f,0.35f,1}, altRow{0.18f,0.18f,0.24f,1};
        ColorF selBg{0.3f,0.4f,0.6f,1}, tc{0.9f,0.9f,1,1}, headerTc{1,1,1,1};
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, bg);
        float rowH = 18;
        float headerH = 20;
        float availW = ctl->extentX;
        // Sync server list from NetworkManager (avoid O(N^2) — just copy once)
        if (Engine::instance().timer().now() - ctl->sbLastQueryTime > 0.5) {
            ctl->sbLastQueryTime = Engine::instance().timer().now();
            ctl->sbServers = Engine::instance().network().getServerList();
        }
        // If no columns defined, set up defaults
        if (ctl->sbColumns.empty()) {
            ctl->sbColumns = {
                {"Name", availW * 0.35f, true},
                {"Map",  availW * 0.25f, true},
                {"Type",  availW * 0.15f, true},
                {"Ping",  availW * 0.1f, true},
                {"Players",  availW * 0.15f, true}
            };
        }
        // Header row
        float hx = x;
        for (size_t ci = 0; ci < ctl->sbColumns.size(); ci++) {
            float cw = ctl->sbColumns[ci].width;
            if (ci == ctl->sbColumns.size() - 1) cw = availW - (hx - x); // last fills remainder
            r.drawRectFill({hx, y, 0}, {hx + cw, y + headerH, 0}, headerBg);
            if (font) {
                std::string label = ctl->sbColumns[ci].name;
                if ((int)ci == ctl->sbSortCol) label += ctl->sbSortInc ? " ▲" : " ▼";
                font->render(label.c_str(), hx + 3, y + 2, headerTc, 1.0f);
            }
            hx += cw;
        }
        // Data rows
        float ry = y + headerH;
        int visRows = (int)((ctl->extentY - headerH) / rowH);
        auto& servers = ctl->sbServers;
        // Sort
        if (ctl->sbSortCol >= 0) {
            std::sort(servers.begin(), servers.end(), [&](const auto& a, const auto& b) {
                auto cmp = [](const std::string& sa, const std::string& sb) -> bool {
                    // Try numeric comparison first
                    char* ea, *eb;
                    double da = strtod(sa.c_str(), &ea);
                    double db = strtod(sb.c_str(), &eb);
                    if (*ea == 0 && *eb == 0) return da < db;
                    return sa < sb;
                };
                bool less = false;
                switch (ctl->sbSortCol) {
                    case 0: less = cmp(a.name, b.name); break;
                    case 1: less = cmp(a.map, b.map); break;
                    case 2: less = cmp(a.gameType, b.gameType); break;
                    case 3: less = a.ping < b.ping; break;
                    case 4: less = (a.numPlayers + a.maxPlayers * 1000) < (b.numPlayers + b.maxPlayers * 1000); break;
                    default: less = a.name < b.name; break;
                }
                return ctl->sbSortInc ? less : !less;
            });
        }
        int scrollOff = (int)(ctl->scrollY / rowH);
        for (int i = scrollOff; i < (int)servers.size() && (i - scrollOff) < visRows; i++, ry += rowH) {
            bool sel = (i == ctl->sbSelected);
            r.drawRectFill({x, ry, 0}, {x + availW, ry + rowH, 0}, sel ? selBg : ((i & 1) ? altRow : bg));
            float rx = x;
            char buf[256];
            for (size_t ci = 0; ci < ctl->sbColumns.size(); ci++) {
                float cw = ctl->sbColumns[ci].width;
                if (ci == ctl->sbColumns.size() - 1) cw = availW - (rx - x);
                std::string val;
                switch (ci) {
                    case 0: val = servers[i].name; break;
                    case 1: val = servers[i].map; break;
                    case 2: val = servers[i].gameType; break;
                    case 3: snprintf(buf, sizeof(buf), "%d", servers[i].ping); val = buf; break;
                    case 4: snprintf(buf, sizeof(buf), "%d/%d", servers[i].numPlayers, servers[i].maxPlayers); val = buf; break;
                    default: val = ""; break;
                }
                if (!val.empty() && font)
                    font->render(val.c_str(), rx + 3, ry + 1, tc, 1.0f);
                rx += cw;
            }
        }
        // Content height for scroll container
        ctl->contentH = headerH + servers.size() * rowH;
    } else if (cn == "GuiScrollContentCtrl") {
        ColorF scc{0.15f,0.15f,0.2f,0.3f};
        auto* prof = getProfile(ctl->profileName);
        if (prof) { auto fi = prof->fields.find("fillColor"); if (fi != prof->fields.end()) parseColor(fi->second.toString(), scc); }
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, scc);
    } else if (cn == "ShellTabFrame") {
        // Tab content frame — draw a bordered area for tab pages
        ColorF frameBg{0.12f,0.12f,0.15f,1};
        ColorF frameBorder{0.25f,0.25f,0.32f,1};
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, frameBg);
        // Thin border
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + 1, 0}, frameBorder);
        r.drawRectFill({x, y + ctl->extentY - 1, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, frameBorder);
        r.drawRectFill({x, y, 0}, {x + 1, y + ctl->extentY, 0}, frameBorder);
        r.drawRectFill({x + ctl->extentX - 1, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, frameBorder);
    } else if (cn == "ShellTabGroupCtrl" || cn == "GuiTabBookCtrl") {
        // Tab group: draw tab buttons along the top, then children below
        const float tabH = 29;
        // Background for the area below tabs
        r.drawRectFill({x, y + tabH, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.12f, 0.12f, 0.15f, 1});
        // Draw each tab button
        float tabX = x + 2;
        Font* tabFont = font;
        auto* prof = getProfile(ctl->profileName);
        if (prof) tabFont = getProfileFont(prof);
        for (int ti = 0; ti < (int)ctl->tabs.size(); ti++) {
            float textW = tabFont ? tabFont->measure(ctl->tabs[ti].text.c_str()).x : (float)ctl->tabs[ti].text.size() * 9.0f;
            float tw = std::max(60.0f, textW + 16);
            bool sel = (ti == ctl->selectedTab);
            ColorF fill = sel ? ColorF{0.35f,0.45f,0.55f,1} : ColorF{0.2f,0.22f,0.28f,1};
            r.drawRectFill({tabX, y, 0}, {tabX + tw, y + tabH, 0}, fill);
            if (sel)
                r.drawRectFill({tabX, y + tabH - 1, 0}, {tabX + tw, y + tabH, 0}, {0.6f,0.7f,0.8f,1});
            if (tabFont && !ctl->tabs[ti].text.empty()) {
                float ty = y + (tabH - (float)tabFont->charHeight) * 0.5f;
                tabFont->render(ctl->tabs[ti].text.c_str(), tabX + (tw - textW) * 0.5f, ty, {1,1,1,1}, 1.0f);
            }
            tabX += tw + 1;
        }
        // Render children (tab content below tab bar)
        for (auto* child : ctl->children)
            renderControlRec(gr, child, canvas, scrollOfsX, scrollOfsY, clip);
        return; // prevent generic children loop below
    } else if (cn == "ShellBitmapButton" && ctl->name == "LaunchToolbarCloseButton") {
        // Close button in LaunchToolbarDlg: draw an X
        ColorF bg{0.25f,0.25f,0.3f,1};
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, bg);
        // Draw X mark
        float cx = x + ctl->extentX * 0.5f, cy = y + ctl->extentY * 0.5f;
        auto drawLine = [&](float x1, float y1, float x2, float y2) {
            r.drawRectFill({x1, y1, 0}, {x2, y2, 0}, {0.8f,0.8f,0.9f,1});
        };
        drawLine(cx-3, cy-3, cx+3, cy+3);
        drawLine(cx-3, cy+3, cx+3, cy-3);
        // Border
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + 1, 0}, {0.4f,0.4f,0.5f,0.5f});
        r.drawRectFill({x, y + ctl->extentY - 1, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.1f,0.1f,0.15f,0.5f});
    } else if (cn == "GuiControl" && ctl->name == "LaunchToolbarDlg") {
        // Transparent overlay — no background, just render children
    } else if (cn == "GuiControl" && ctl->name == "LaunchToolbarPane") {
        // Toolbar pane at the bottom of LaunchGui: gradient fill + top border
        float halfH = ctl->extentY * 0.5f;
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + halfH, 0}, {0.14f,0.14f,0.17f,1});
        r.drawRectFill({x, y + halfH, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.1f,0.1f,0.12f,1});
        // Top highlight border
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + 1, 0}, {0.3f,0.3f,0.4f,0.6f});
    } else if (cn.find("Hud") == 0 || cn.find("ShellFieldCtrl") == 0 || cn.find("ShellField") == 0) {
        // HUD controls: transparent background with specific rendering per type
        auto* prof = getProfile(ctl->profileName);
        ColorF tc{0.8f,0.8f,0.9f,0.9f};
        float fontSize = 12.0f;
        Font* hf = font;
        // HudEnergy/HudDamage/HudHeat/HudBarBaseCtrl: bar display
        if (cn == "HudEnergy" || cn == "HudDamage" || cn == "HudHeat" || cn == "HudBarBaseCtrl") {
            ColorF barBg{0.1f,0.1f,0.15f,0.5f};
            ColorF barFg = cn == "HudEnergy" ? ColorF{0,0.8f,1,0.8f} : cn == "HudDamage" ? ColorF{1,0.2f,0.2f,0.8f} : cn == "HudHeat" ? ColorF{1,0.5f,0,0.8f} : ColorF{0.3f,0.6f,0.3f,0.8f};
            float barH = ctl->extentY * 0.7f;
            float barY = y + (ctl->extentY - barH) * 0.5f;
            r.drawRectFill({x, barY, 0}, {x + ctl->extentX, barY + barH, 0}, barBg);
            r.drawRectFill({x, barY, 0}, {x + ctl->extentX * 0.5f, barY + barH, 0}, barFg);
        } else if (cn == "HudCompass") {
            // Compass: circular in top-center of HUD
            float cx = x + ctl->extentX * 0.5f, cy = y + ctl->extentY * 0.5f;
            float radius = std::min(ctl->extentX, ctl->extentY) * 0.4f;
            r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0,0,0,0.3f});
            // Draw N/S/E/W text
            if (hf) {
                hf->render("N", cx - 4, y + 2, {1,0.5f,0.5f,0.9f}, 1.0f);
                hf->render("S", cx - 4, y + ctl->extentY - 12, {0.5f,0.5f,0.5f,0.9f}, 1.0f);
            }
        } else if (cn == "HudClock") {
            // Clock: top-center of HUD
            if (hf) {
                std::string timeStr = "00:00";
                auto* sobj = ScriptEngine::instance().findObject(ctl->name.c_str());
                if (sobj) {
                    auto ti = sobj->fields.find("text");
                    if (ti != sobj->fields.end()) timeStr = ti->second.toString();
                }
                float tw = hf->measure(timeStr.c_str()).x;
                hf->render(timeStr.c_str(), x + (ctl->extentX - tw) * 0.5f, y + 2, {0.5f,1,0.5f,0.9f}, 1.5f);
            }
        } else if (cn == "HudPulsingBitmap" || cn == "HudBitmapCtrl") {
            // Bitmap HUD: semi-transparent background, no fill to let 3D show through
            if (ctl->text.empty() && ctl->bitmap.empty()) {
                // Draw a subtle outline so the control area is visible
                r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + 1, 0}, {0.5f,0.5f,0.6f,0.2f});
                r.drawRectFill({x, y + ctl->extentY - 1, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.5f,0.5f,0.6f,0.2f});
            }
        } else {
            // Generic HUD: transparent background, render text
            ColorF gc{0.2f,0.2f,0.25f,1};
            bool opaque = false;
            if (prof) {
                auto fi = prof->fields.find("fontColor"); if (fi != prof->fields.end()) parseColor(fi->second.toString(), tc);
                auto fsi = prof->fields.find("fontSize"); if (fsi != prof->fields.end()) fontSize = (float)fsi->second.toDouble();
                auto fti = prof->fields.find("fontType"); if (fti != prof->fields.end()) hf = r.getFont(fti->second.toString().c_str(), (int)fontSize);
                auto oi = prof->fields.find("opaque");
                if (oi != prof->fields.end()) { std::string ov = oi->second.toString(); if (ov == "true" || ov == "1") opaque = true; }
            }
            if (opaque)
                r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.1f,0.1f,0.15f,0.6f});
            if (hf && !ctl->text.empty()) {
                float tx = x + 4, ty = y + 2;
                hf->render(ctl->text.c_str(), tx, ty, tc, 1.0f);
            }
        }
    } else {
        // Generic GuiControl with profile-aware fill and text
        ColorF gc{0.2f, 0.2f, 0.25f, 1.0f};
        ColorF tc{1,1,1,1};
        float fontSize = 14.0f;
        Font* gf = r.getFont();
        float textOfsX = 4, textOfsY = 0;
        std::string justify = "left";
        auto* prof = getProfile(ctl->profileName);
        if (prof) {
            auto fi = prof->fields.find("fillColor"); if (fi != prof->fields.end()) parseColor(fi->second.toString(), gc);
            auto fci = prof->fields.find("fontColor"); if (fci != prof->fields.end()) parseColor(fci->second.toString(), tc);
            auto fsi = prof->fields.find("fontSize"); if (fsi != prof->fields.end()) fontSize = (float)fsi->second.toDouble();
            auto fti = prof->fields.find("fontType"); if (fti != prof->fields.end()) gf = r.getFont(fti->second.toString().c_str(), (int)fontSize);
            auto toi = prof->fields.find("textOffset"); if (toi != prof->fields.end()) sscanf(toi->second.toString().c_str(), "%f %f", &textOfsX, &textOfsY);
            auto ji = prof->fields.find("justify"); if (ji != prof->fields.end()) justify = ji->second.toString();
            auto oi = prof->fields.find("opaque");
            if (oi != prof->fields.end()) { std::string ov = oi->second.toString(); if (ov == "true" || ov == "1") gc.a = 1.0f; }
        }
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, gc);
        if (!ctl->text.empty() && gf) {
            float tx = x + textOfsX;
            float ty = y + (ctl->extentY - fontSize) * 0.5f + textOfsY;
            float tw = gf->measure(ctl->text.c_str()).x;
            if (justify == "center") tx = x + (ctl->extentX - tw) * 0.5f;
            else if (justify == "right") tx = x + ctl->extentX - tw - textOfsX;
            gf->render(ctl->text.c_str(), tx, ty, tc, 1.0f);
        }
    }

    // Render children (propagate scroll offset)
    for (auto* child : ctl->children)
        renderControlRec(gr, child, canvas, scrollOfsX, scrollOfsY, clip);
}

void GuiRenderer::update(float dt) {
    updateFades(dt);
    double now = Engine::instance().timer().now();
    // Collect expired events first, then execute after erasing
    // (execution may add new events, invalidating iterators)
    std::vector<std::string> toRun;
    size_t writeIdx = 0;
    for (size_t i = 0; i < events.size(); i++) {
        if (now >= events[i].triggerTime) {
            toRun.push_back(events[i].command);
        } else {
            if (writeIdx != i) events[writeIdx] = std::move(events[i]);
            writeIdx++;
        }
    }
    events.resize(writeIdx);
    for (auto& cmd : toRun)
        Console::instance().execute(cmd.c_str());
}

void GuiRenderer::addSchedule(double delay, const std::string& command) {
    ScheduledEvent ev;
    ev.triggerTime = Engine::instance().timer().now() + delay;
    ev.command = command;
    events.push_back(ev);
}

FadeState* GuiRenderer::getFadeState(const GuiControl* ctl, bool createIfMissing) {
    auto it = fadeStates.find(ctl->name);
    if (it != fadeStates.end())
        return &it->second;
    if (!createIfMissing) return nullptr;
    FadeState fs;
    fs.elapsed = 0.0;
    fs.fadeTime = 2.0;
    fs.fadeOut = true;
    fs.done = false;
    fs.currentAlpha = 0.0;
    auto r = fadeStates.insert({ctl->name, fs});
    return &r.first->second;
}

void GuiRenderer::updateFades(float dt) {
    // Update all active GuiFadeinBitmapCtrl fade animations
    for (size_t i = 0; i < dialogStack.size(); i++) {
        auto* dlg = dialogStack[i];
        if (!dlg || dlg->className != "GuiFadeinBitmapCtrl") continue;
        FadeState* fs = getFadeState(dlg, true);
        if (fs->done) continue;
        fs->elapsed += dt;

        // Parse fadeTime from control (default 2000 ms)
        float fadeTime = 2.0f;
        auto* obj = ScriptEngine::instance().findObject(dlg->name.c_str());
        if (obj) {
            auto fi = obj->fields.find("fadeTime");
            if (fi != obj->fields.end())
                fadeTime = fi->second.toFloat() / 1000.0f;
        }

        float halfTime = fadeTime * 0.5f;
        if (fs->elapsed < halfTime) {
            // Fade in: 0 -> 1
            fs->currentAlpha = fs->elapsed / halfTime;
        } else if (fs->fadeOut && fs->elapsed < fadeTime) {
            // Fade out: 1 -> 0
            fs->currentAlpha = 1.0f - (fs->elapsed - halfTime) / halfTime;
        } else if (fs->elapsed >= fadeTime) {
            fs->currentAlpha = fs->fadeOut ? 0.0f : 1.0f;
            fs->done = true;
            // Set done=true on the script object so TS can detect it
            if (obj) {
                obj->fields["done"] = VMValue("1");
                obj->fields["skip"] = VMValue("1"); // skip AVI intro, go straight to StartLoginProcess
            }
        }
    }

    // Clean up fade states for controls no longer in the stack
    for (auto it = fadeStates.begin(); it != fadeStates.end(); ) {
        bool found = false;
        for (auto* dlg : dialogStack) {
            if (dlg && dlg->name == it->first) { found = true; break; }
        }
        if (found) ++it;
        else it = fadeStates.erase(it);
    }
}

GuiControl* GuiRenderer::hitTest(GuiControl* ctl, int mx, int my) {
    if (!ctl || !ctl->visible || !ctl->active) return nullptr;
    // Tab pages never intercept clicks — pass through to parent tab group
    if (ctl->className == "GuiTabPageCtrl") return nullptr;
    float x = ctl->posX;
    float y = ctl->posY;
    GuiControl* p = ctl->parent;
    while (p && p != canvas) {
        x += p->posX;
        y += p->posY;
        p = p->parent;
    }
    if (mx >= x && mx < x + ctl->extentX && my >= y && my < y + ctl->extentY) {
        for (auto* child : ctl->children) { auto* h = hitTest(child, mx, my); if (h) return h; }
        return ctl;
    }
    return nullptr;
}

bool GuiRenderer::handleScroll(int x, int y, int wheelDelta) {
    // Check all dialogs from top to bottom
    GuiControl* hit = nullptr;
    for (auto it = dialogStack.rbegin(); it != dialogStack.rend(); ++it) {
        hit = hitTest(*it, x, y);
        if (!hit) continue;
        if (hit == *it && hit->onClick == nullptr) {
            bool isScrollable = hit->className == "GuiScrollCtrl" || hit->className == "ShellScrollCtrl";
            if (!isScrollable) { hit = nullptr; continue; }
        }
        break;
    }
    if (!hit) return false;
    GuiControl* scrollCtrl = hit;
    while (scrollCtrl) {
        if (scrollCtrl->className == "GuiScrollCtrl" || scrollCtrl->className == "ShellScrollCtrl") {
            scrollCtrl->scrollY += (wheelDelta > 0 ? 30 : -30);
            return true;
        }
        scrollCtrl = scrollCtrl->parent;
    }
    return false;
}

bool GuiRenderer::handleInput(int x, int y, bool pressed) {
    if (!pressed) return false;
    GuiControl* hit = nullptr;
    // Check all dialogs from top to bottom so clicks pass through transparent overlays
    for (auto it = dialogStack.rbegin(); it != dialogStack.rend(); ++it) {
        hit = hitTest(*it, x, y);
        if (!hit) continue;
        // If hit is a dialog root (no clickable class) with no onClick, skip to next
        if (hit == *it && hit->onClick == nullptr) {
            bool isClickable = hit->className == "ShellLaunchMenu" || hit->className == "ShellPopupMenu" ||
                hit->className == "GuiPopUpMenuCtrl" || hit->className == "ShellTabGroupCtrl" ||
                hit->className == "GuiTabBookCtrl" || hit->className == "ShellTextList" ||
                hit->className == "GuiListBoxCtrl" || hit->className == "GuiTextListCtrl" ||
                hit->className == "GuiServerBrowser" || hit->className.find("Button") != std::string::npos;
            if (!isClickable) { hit = nullptr; continue; }
        }
        break;
    }
    if (!hit) return false;
    // GuiServerBrowser: row selection + column header sort
    if (hit->className == "GuiServerBrowser") {
        // Compute absolute position of the control
        float ax = hit->posX, ay = hit->posY;
        for (auto* p = hit->parent; p && p != canvas; p = p->parent) { ax += p->posX; ay += p->posY; }
        float rowH = 18, headerH = 20;
        int row = (int)((y - ay) / rowH);
        float hx = ax;
        int col = -1;
        for (size_t ci = 0; ci < hit->sbColumns.size(); ci++) {
            hx += hit->sbColumns[ci].width;
            if (x < hx) { col = ci; break; }
        }
        if (row == 0 && col >= 0) {
            if (col == hit->sbSortCol) hit->sbSortInc = !hit->sbSortInc;
            else { hit->sbSortCol = col; hit->sbSortInc = true; }
        } else if (row > 0) {
            int serverIdx = row - 1;
            if (serverIdx < (int)hit->sbServers.size()) {
                hit->sbSelected = serverIdx;
                ScriptEngine::instance().executeString(
                    ("GMJ_Browser.onSelect(\"" + hit->sbServers[serverIdx].addr.toString() + "\");").c_str());
            }
        }
        return true;
    }
    // ShellTextList / GuiListBoxCtrl: row selection
    if (hit->className == "ShellTextList" || hit->className == "GuiListBoxCtrl" || hit->className == "GuiTextListCtrl") {
        float ax = hit->posX, ay = hit->posY;
        for (auto* p = hit->parent; p && p != canvas; p = p->parent) { ax += p->posX; ay += p->posY; }
        // Get scroll offset
        float scrollY = 0;
        GuiControl* sp = hit->parent;
        while (sp) {
            if (sp->className == "GuiScrollCtrl" || sp->className == "ShellScrollCtrl") {
                scrollY = sp->scrollY;
                break;
            }
            sp = sp->parent;
        }
        auto* font = Engine::instance().renderer().getFont();
        float lineH = font ? font->charHeight + 2 : 14;
        int row = (int)((y - ay + scrollY) / lineH);
        if (row >= 0 && row < (int)hit->listRows.size()) {
            hit->selectedRow = row;
            // Call onSelect if defined
            auto* ts = Engine::instance().script().ts();
            std::string selName = hit->name + "::onSelect";
            if (ts && ts->hasFunction(selName))
                ts->callFunction(selName, {VMValue(hit->name), VMValue(row), VMValue(hit->listRows[row])});
        }
        return true;
    }
    // ShellLaunchMenu: toggle popup menu or select item
    if (hit->className == "ShellLaunchMenu") {
        if (hit->menuOpen) {
            // Check if click is on a popup item
            float ax = hit->posX, ay = hit->posY;
            for (auto* p = hit->parent; p && p != canvas; p = p->parent) { ax += p->posX; ay += p->posY; }
            float popX = ax;
            float popY = ay - (float)hit->menuItems.size() * 20.0f - 4;
            float popW = 180, lineH = 20;
            if (x >= popX && x < popX + popW && y >= popY && y < popY + lineH * (float)hit->menuItems.size()) {
                int idx = (int)((y - popY) / lineH);
                if (idx >= 0 && idx < (int)hit->menuItems.size() && !hit->menuItems[idx].isSeparator) {
                    auto* ts = Engine::instance().script().ts();
                    if (ts && ts->hasFunction(hit->name + "::onSelect"))
                        ts->callFunction(hit->name + "::onSelect",
                            {VMValue(hit->name), VMValue(hit->menuItems[idx].id), VMValue(hit->menuItems[idx].text)});
                }
            }
            hit->menuOpen = false;
        } else {
            hit->menuOpen = true;
        }
        return true;
    }
    // ShellPopupMenu / GuiPopUpMenuCtrl: toggle dropdown or select item
    if (hit->className == "ShellPopupMenu" || hit->className == "GuiPopUpMenuCtrl") {
        if (hit->menuOpen) {
            float ax = hit->posX, ay = hit->posY;
            for (auto* p = hit->parent; p && p != canvas; p = p->parent) { ax += p->posX; ay += p->posY; }
            float popX = ax;
            float popY = ay + hit->extentY;
            float popW = hit->extentX, lineH = 20;
            if (x >= popX && x < popX + popW && y >= popY && y < popY + lineH * (float)hit->menuItems.size()) {
                int idx = (int)((y - popY) / lineH);
                if (idx >= 0 && idx < (int)hit->menuItems.size() && !hit->menuItems[idx].isSeparator) {
                    hit->text = hit->menuItems[idx].text;
                    auto* ts = Engine::instance().script().ts();
                    if (ts && ts->hasFunction(hit->name + "::onSelect"))
                        ts->callFunction(hit->name + "::onSelect",
                            {VMValue(hit->name), VMValue(hit->menuItems[idx].id), VMValue(hit->menuItems[idx].text)});
                }
            }
            hit->menuOpen = false;
        } else {
            hit->menuOpen = true;
        }
        return true;
    }
    // Tab group click: determine which tab button was clicked
    if (hit->className == "ShellTabGroupCtrl" || hit->className == "GuiTabBookCtrl") {
        // Compute absolute position
        float ax = hit->posX, ay = hit->posY;
        for (auto* p = hit->parent; p && p != canvas; p = p->parent) { ax += p->posX; ay += p->posY; }
        const float tabH = 29;
        if (y >= ay && y < ay + tabH && x >= ax) {
            float tabX = ax + 2;
            auto* font = Engine::instance().renderer().getFont();
            for (int ti = 0; ti < (int)hit->tabs.size(); ti++) {
                float textW = font ? font->measure(hit->tabs[ti].text.c_str()).x : (float)hit->tabs[ti].text.size() * 9.0f;
                float tw = std::max(60.0f, textW + 16);
                if (x >= tabX && x < tabX + tw) {
                    hit->selectedTab = ti;
                    // Show/hide sibling GuiTabPageCtrl children (T3D-style page visibility)
                    int pageIdx = 0;
                    for (auto* sib : hit->children) {
                        if (sib->className == "GuiTabPageCtrl") {
                            sib->visible = (pageIdx == ti);
                            pageIdx++;
                        }
                    }
                    // Call onSelect script (handles setContent via %this.gui[%tab])
                    // The script's gui[] is stored by numeric index (gui[0], gui[1], etc.)
                    // but the script's viewTab uses text as index (gui["TRAINING"]) which fails.
                    // So we also set content directly from C++ using the stored numeric field.
                    auto* ts = Engine::instance().script().ts();
                    if (ts && ts->hasFunction(hit->name + "::onSelect"))
                        ts->callFunction(hit->name + "::onSelect",
                            {VMValue(hit->name), VMValue(ti), VMValue(hit->tabs[ti].text)});
                    // C++ fallback: only for main LaunchTabView (not nested tab groups like GM_TabView)
                    // The script's viewTab uses text-based lookup which fails. Use numeric index instead.
                    if (hit->name == "LaunchTabView") {
                        auto* sobj = ScriptEngine::instance().findObject(hit->name.c_str());
                        if (sobj) {
                            std::string gk = "gui[" + std::to_string(ti) + "]";
                            auto gi = sobj->fields.find(gk);
                            if (gi != sobj->fields.end()) {
                                std::string guiName = gi->second.toString();
                                if (!guiName.empty()) {
                                    Engine::instance().guiRenderer().setContent(guiName);
                                    if (!Engine::instance().guiRenderer().isDialogActive("LaunchToolbarDlg"))
                                        Engine::instance().guiRenderer().pushDialog("LaunchToolbarDlg");
                                }
                            }
                        }
                    }
                    return true;
                }
                tabX += tw + 1;
            }
        }
    }
    // Text edit: set focusedCtrl for keyboard input
    if (hit->className == "GuiTextEditCtrl" || hit->className == "ShellTextEditCtrl") {
        focusedCtrl = hit;
        return true;
    }

    // Click on non-text control clears focus
    if (focusedCtrl) focusedCtrl = nullptr;

    // Radio button / checkbox: handle group mutual exclusion before onClick
    if (hit->className == "GuiRadioCtrl" || hit->className == "ShellRadioButton") {
        if (hit->groupNum != 0) {
            // Uncheck siblings with same groupNum, then check this one
            auto* p = hit->parent;
            if (p) {
                for (auto* sib : p->children) {
                    if (sib != hit && sib->groupNum == hit->groupNum)
                        sib->checked = false;
                }
            }
        }
        hit->checked = true;
    }
    if (hit->onClick) {
        hit->onClick();
        return true;
    }
    return false;
}

void GuiRenderer::handleKeyboard() {
    if (!focusedCtrl) return;
    // Check if focused control is still valid (still in the tree)
    bool found = false;
    for (auto* d : dialogStack) {
        if (d == focusedCtrl || d->findChild(focusedCtrl->name)) { found = true; break; }
        // Also check children recursively
        std::vector<GuiControl*> stack = d->children;
        while (!stack.empty()) {
            auto* c = stack.back(); stack.pop_back();
            if (c == focusedCtrl) { found = true; break; }
            for (auto* ch : c->children) stack.push_back(ch);
        }
        if (found) break;
    }
    if (!found) { focusedCtrl = nullptr; return; }

    auto& input = Engine::instance().platform().input();
    const std::string& ti = input.textInput;
    if (!ti.empty()) {
        for (char c : ti) {
            if (c >= 0x20 && c <= 0x7e && focusedCtrl->text.size() < 200) {
                focusedCtrl->text.insert(focusedCtrl->cursorPos, 1, c);
                focusedCtrl->cursorPos++;
            }
        }
    }

    static bool prevBS = false, prevEnter = false, prevEsc = false;
    if (input.keysDown[SCANCODE_BACKSPACE] && !prevBS) {
        if (focusedCtrl->cursorPos > 0 && !focusedCtrl->text.empty()) {
            focusedCtrl->text.erase(focusedCtrl->cursorPos - 1, 1);
            focusedCtrl->cursorPos--;
        }
    }
    prevBS = input.keysDown[SCANCODE_BACKSPACE];

    if (input.keysDown[SCANCODE_RETURN] && !prevEnter) {
        // Fire command (Enter handler), then fall back to altCommand, then onClick
        if (!focusedCtrl->command.empty()) {
            Console::instance().execute(focusedCtrl->command.c_str());
        } else if (!focusedCtrl->altCommand.empty()) {
            Console::instance().execute(focusedCtrl->altCommand.c_str());
        } else if (focusedCtrl->onClick) {
            focusedCtrl->onClick();
        }
    }
    prevEnter = input.keysDown[SCANCODE_RETURN];

    if (input.keysDown[SCANCODE_ESCAPE] && !prevEsc) {
        focusedCtrl = nullptr;
    }
    prevEsc = input.keysDown[SCANCODE_ESCAPE];
}

// Create a GuiControl from a ScriptObject (and recursively create children)
GuiControl* GuiRenderer::soToGui(const std::string& name, GuiControl* parent) {
    auto& objs = ScriptEngine::instance().objects;
    auto it = objs.find(name);
    if (it == objs.end() || !(it->second->className.find("Gui") == 0 || it->second->className.find("Shell") == 0 || it->second->className == "GameTSCtrl"))
        return nullptr;
    // Already exists as a GuiControl?
    GuiControl* ctl = findControl(name);
    if (ctl) {
        if (parent && ctl->parent == canvas) {
            auto& cv = canvas->children;
            cv.erase(std::remove(cv.begin(), cv.end(), ctl), cv.end());
            parent->addChild(ctl);
        }
        return ctl;
    }
    ctl = new GuiControl;
    ctl->name = it->second->name;
    ctl->className = it->second->className;
    auto parsePair = [&](const std::string& key, float& a, float& b) {
        auto fi = it->second->fields.find(key);
        if (fi != it->second->fields.end()) { std::string s = fi->second.toString(); sscanf(s.c_str(), "%f %f", &a, &b); }
    };
    parsePair("position", ctl->posX, ctl->posY);
    parsePair("extent", ctl->extentX, ctl->extentY);
    auto fi = it->second->fields.find("text"); if (fi != it->second->fields.end()) ctl->text = fi->second.toString();
    fi = it->second->fields.find("bitmap"); if (fi != it->second->fields.end()) ctl->bitmap = fi->second.toString();
    fi = it->second->fields.find("command"); if (fi != it->second->fields.end()) ctl->command = fi->second.toString();
    fi = it->second->fields.find("altCommand"); if (fi != it->second->fields.end()) ctl->altCommand = fi->second.toString();
    fi = it->second->fields.find("profile"); if (fi != it->second->fields.end()) ctl->profileName = fi->second.toString();
    fi = it->second->fields.find("visible"); if (fi != it->second->fields.end()) ctl->visible = fi->second.toBool();
    fi = it->second->fields.find("groupNum"); if (fi != it->second->fields.end()) ctl->groupNum = (int)fi->second.toDouble();
    fi = it->second->fields.find("sel"); if (fi != it->second->fields.end()) ctl->checked = fi->second.toBool();
    if (ctl->className == "GuiCanvas") canvas = ctl;
    bool isClickable = ctl->className.find("Button") != std::string::npos || ctl->className == "GuiCheckBoxCtrl" || ctl->className == "GuiRadioCtrl" || ctl->className == "ShellBitmapButton" || ctl->className == "ShellToggleButton" || ctl->className == "ShellTabButton" || ctl->className == "GuiTextEditCtrl" || ctl->className == "ShellTextEditCtrl";
    if (!ctl->command.empty() && isClickable) {
        std::string cmd = ctl->command;
        // Intercept training START button to use C++ startLocalGame instead of script CreateServer path
        if (cmd.find("TrainingGui.startTraining") != std::string::npos) {
            ctl->onClick = []() {
                auto& g = Engine::instance().guiRenderer();
                auto* list = g.findControl("TrainingMissionList");
                if (list && list->selectedRow >= 0 && list->selectedRow < (int)list->listRows.size()) {
                    std::string row = list->listRows[list->selectedRow];
                    size_t tab = row.find('\t');
                    std::string mission = (tab != std::string::npos) ? row.substr(tab + 1) : row;
                    Console::instance().printf(LogLevel::Info, "Training: starting mission '%s'", mission.c_str());
                    Engine::instance().game().startLocalGame(mission.c_str());
                } else {
                    Console::instance().printf(LogLevel::Warn, "Training: no mission selected, starting default");
                    Engine::instance().game().startLocalGame();
                }
            };
        } else {
            ctl->onClick = [cmd]() { Console::instance().execute(cmd.c_str()); };
        }
    }
    // Recursively create children from ScriptObjects with parent == this name
    // NOTE: children must be created BEFORE adding to parent/canvas, so that
    // findControl doesn't find this control prematurely during child creation.
    for (auto& [n, obj] : objs) {
        auto pit = obj->internals.find("parent");
        if (pit != obj->internals.end() && pit->second.toString() == name) {
            soToGui(n, ctl);
        }
    }
    if (parent) {
        parent->addChild(ctl);
    } else if (ctl != canvas && canvas) {
        canvas->addChild(ctl);
    }
    if (canvas) {
        ctl->extentX = (ctl->extentX <= 100 && ctl->extentY <= 30) ? canvas->extentX : ctl->extentX;
        ctl->extentY = (ctl->extentX <= 100 && ctl->extentY <= 30) ? canvas->extentY : ctl->extentY;
    }
    return ctl;
}

void GuiRenderer::pushDialog(const std::string& name) {
    GuiControl* ctl = findControl(name);
    if (!ctl) {
        // Try creating the GuiControl from the ScriptObject on-the-fly
        auto& objs = ScriptEngine::instance().objects;
        auto it = objs.find(name);
        if (it != objs.end() && (it->second->className.find("Gui") == 0 || it->second->className.find("Shell") == 0 || it->second->className == "GameTSCtrl")) {
            ctl = soToGui(name, nullptr);
        }
    }
        if (ctl) {
            ctl->visible = true;
            dialogStack.push_back(ctl);
        callOnAddOnce(ctl);
            // Trigger onWake so script functions like LaunchToolbarDlg::onWake populate menus
            if (auto* ts = Engine::instance().script().ts()) {
                if (ts->hasFunction(name + "::onWake")) {
                    Console::instance().printf(LogLevel::Debug, "GUI: pushDialog calling onWake '%s'", name.c_str());
                    ts->callFunction(name + "::onWake", {});
                }
            }
            Console::instance().printf(LogLevel::Debug, "GUI: pushDialog %s (stack now %zu)", name.c_str(), dialogStack.size());
        } else {
        Console::instance().printf(LogLevel::Warn, "GUI: pushDialog '%s' not found", name.c_str());
    }
}

void GuiRenderer::popDialog(const std::string& name) {
    for (auto it = dialogStack.begin(); it != dialogStack.end(); ++it) {
        if ((*it)->name == name || name.empty()) {
            dialogStack.erase(it);
            Console::instance().printf(LogLevel::Debug, "GUI: popDialog %s (stack now %zu)", name.c_str(), dialogStack.size());
            return;
        }
    }
}

void GuiRenderer::callOnAddOnce(GuiControl* ctl) {
    if (!ctl || onAddCalled.count(ctl->name)) return;
    onAddCalled.insert(ctl->name);
    auto* ts = Engine::instance().script().ts();
    if (ts && ts->hasFunction(ctl->name + "::onAdd"))
        ts->callFunction(ctl->name + "::onAdd", {});
    for (auto* ch : ctl->children)
        callOnAddOnce(ch);
}

void GuiRenderer::setContent(const std::string& name) {
    GuiControl* ctl = soToGui(name, nullptr);
    if (ctl) {
        dialogStack.clear();
        dialogStack.push_back(ctl);
        callOnAddOnce(ctl);
        if (auto* ts = Engine::instance().script().ts()) {
            if (ts->hasFunction(name + "::onWake")) ts->callFunction(name + "::onWake", {});
        }
    } else {
        Console::instance().printf(LogLevel::Warn, "GUI: setContent '%s' not found", name.c_str());
    }
}

bool GuiRenderer::isDialogActive(const std::string& name) {
    for (auto* dlg : dialogStack)
        if (dlg->name == name) return true;
    return false;
}

GuiControl* GuiRenderer::findControl(const std::string& name) {
    if (!canvas) return nullptr;
    return canvas->findChild(name);
}
