#include "render/gui_renderer.h"
#include "render/shader.h"
#include "core/console.h"
#include "core/engine.h"
#include "script/script_engine.h"
#include <GL/glew.h>
#include <algorithm>
#include <unordered_map>
#include <cstdio>
#include <cmath>
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

GuiRenderer::GuiRenderer() {}
GuiRenderer::~GuiRenderer() {}

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
            auto ctl = controlMap[name];
            auto pit = obj->internals.find("parent");
            if (pit != obj->internals.end()) {
                std::string pname = pit->second.toString();
                auto pc = controlMap.find(pname);
                if (pc != controlMap.end()) {
                    pc->second->addChild(ctl);
                }
            } else if (ctl != canvas && canvas) {
                canvas->addChild(ctl);
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
    ortho.m[3][0] = -1.0f;
    ortho.m[3][1] = 1.0f;
    r.setProjection(ortho);
    r.setView(MatrixF{});

    // Render active dialog (top of stack)
    if (!dialogStack.empty()) {
        renderControl(dialogStack.back());
    }
    // When no dialog is active, render just the canvas background (no children)
    if (dialogStack.empty()) {
        // Only draw the canvas background, don't render all loaded GUIs
        r.drawBox({{0,0,0}, {1024,768,0}}, {0.15f,0.15f,0.2f,1});
    }



    // Restore projection and view
    r.setProjection(savedProj);
    r.setView(savedView);
}

struct ClipRect { float x, y, w, h; };
struct BmpCell { int x, y, w, h; };

// Profile lookup from loaded scripts
static ScriptObject* getProfile(const std::string& name) {
    auto& objs = ScriptEngine::instance().objects;
    auto it = objs.find(name);
    if (it != objs.end() && it->second->className == "GuiControlProfile")
        return it->second;
    return nullptr;
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
        uint32_t vao, vbo, ebo;
        glGenVertexArrays(1,&vao); glGenBuffers(1,&vbo); glGenBuffers(1,&ebo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER,vbo); glBufferData(GL_ARRAY_BUFFER,sizeof(verts),verts,GL_STREAM_DRAW);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(SV),0); glEnableVertexAttribArray(0);
        glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)12); glEnableVertexAttribArray(1);
        glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)20); glEnableVertexAttribArray(2);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo); glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(ids),ids,GL_STREAM_DRAW);
        glDisable(GL_CULL_FACE);
        glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,0);
        glDeleteVertexArrays(1,&vao); glDeleteBuffers(1,&vbo); glDeleteBuffers(1,&ebo);
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
        uint32_t vao2, vbo2, ebo2;
        glGenVertexArrays(1,&vao2); glGenBuffers(1,&vbo2); glGenBuffers(1,&ebo2);
        glBindVertexArray(vao2);
        glBindBuffer(GL_ARRAY_BUFFER,vbo2); glBufferData(GL_ARRAY_BUFFER,sizeof(verts),verts,GL_STREAM_DRAW);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(SV2),0); glEnableVertexAttribArray(0);
        glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(SV2),(void*)12); glEnableVertexAttribArray(1);
        glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,sizeof(SV2),(void*)20); glEnableVertexAttribArray(2);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo2); glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(ids),ids,GL_STREAM_DRAW);
        glDisable(GL_CULL_FACE);
        glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,0);
        glDeleteVertexArrays(1,&vao2); glDeleteBuffers(1,&vbo2); glDeleteBuffers(1,&ebo2);
        // Restore wrap
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, oldWrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, oldWrap);
    }
}

// Shell texture cache - tries .bm8 first (original T2 format with separator color)
static Texture* getShellTex(Renderer& r, const char* name) {
    std::string base = std::string("textures/gui/") + name;
    // Try .bm8 first (original T2 format, has proper separator color)
    auto tryExt = [&](const std::string& ext) -> Texture* {
        std::string p = base.substr(0, base.rfind('.')) + ext;
        if (p == base) p = base + ext;
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
        r.drawBox({{0,0,0}, {1024,768,0}}, {0.15f,0.15f,0.2f,1});
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
        cn == "ShellBitmapButton" || cn == "GuiBitmapButtonCtrl") {
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
        if (shTex && bmpCells && bmpCells->size() >= 3) {
            int state = 0; // 0=normal, 1=highlight, 2=pressed
            auto& cL = (*bmpCells)[state];         // left cap for this state
            auto& cF = (*bmpCells)[3 + state];     // fill/middle for this state
            auto& cR = (*bmpCells)[6 + state];     // right cap for this state
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
            r.drawRectFill({x-1, y-1, 0}, {x + ctl->extentX + 1, y + ctl->extentY + 1, 0}, btnBorder);
            r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, btnFill);
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
        if (font && !ctl->text.empty()) {
            ColorF tc{1,1,1,1};
            auto* prof = getProfile(ctl->profileName);
            if (prof) {
                auto fci = prof->fields.find("fontColor");
                if (fci != prof->fields.end()) parseColor(fci->second.toString(), tc);
                font = getProfileFont(prof);
            }
            float tx = x + 5, ty = y + (ctl->extentY - (float)font->charHeight) * 0.5f;
            float toX, toY;
            if (getTextOffset(prof, toX, toY)) { tx += toX; ty += toY; }
            font->render(ctl->text.c_str(), tx, ty, tc, 1.0f);
        }
    } else if (cn == "GuiTextCtrl" || cn == "GuiMLTextCtrl") {
        ColorF tc{1,1,1,1};
        auto* prof = getProfile(ctl->profileName);
        if (prof) { auto fi = prof->fields.find("fontColor"); if (fi != prof->fields.end()) parseColor(fi->second.toString(), tc); }
        if (font && !ctl->text.empty()) {
            std::string txt = ctl->text;
            float ch = 12;
            float lineY = y + 2;
            size_t pos = 0;
            while (pos < txt.size()) {
                size_t nl = txt.find('\n', pos);
                std::string line = (nl != std::string::npos) ? txt.substr(pos, nl - pos) : txt.substr(pos);
                float lw = (float)line.size() * ch * 0.7f;
                if (lw > ctl->extentX - 4) {
                    size_t brk = line.size();
                    while (brk > 0 && lw > ctl->extentX - 4) {
                        brk = line.rfind(' ', brk - 1);
                        if (brk == std::string::npos) break;
                        lw = (float)brk * ch * 0.7f;
                    }
                    if (brk != std::string::npos && brk > 0) {
                        font->render(line.substr(0, brk).c_str(), x + 2, lineY, tc, 1.0f);
                        lineY += ch;
                        line = line.substr(brk + 1);
                    }
                }
                font->render(line.c_str(), x + 2, lineY, tc, 1.0f);
                lineY += ch;
                pos = (nl != std::string::npos) ? nl + 1 : txt.size();
            }
        }
    } else if (cn == "GuiBitmapCtrl" || cn == "GuiChunkedBitmapCtrl") {
        std::string bmpPath;
        auto* prof = getProfile(ctl->profileName);
        if (prof) { auto bi = prof->fields.find("bitmap"); if (bi != prof->fields.end()) bmpPath = bi->second.toString(); }
        if (bmpPath.empty()) bmpPath = ctl->bitmap;
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
        }
    } else if (cn == "GuiListBoxCtrl" || cn == "ShellTextList" || cn == "GuiTextListCtrl") {
        ColorF fc{0.2f,0.2f,0.25f,1}, tc{0.8f,0.8f,1,1};
        auto* prof = getProfile(ctl->profileName);
        if (prof) { auto fi = prof->fields.find("fillColor"); if (fi != prof->fields.end()) parseColor(fi->second.toString(), fc); }
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, fc);
        if (font && !ctl->text.empty()) {
            auto* prof2 = getProfile(ctl->profileName);
            if (prof2) { auto fi = prof2->fields.find("fontColor"); if (fi != prof2->fields.end()) parseColor(fi->second.toString(), tc); }
            font->render(ctl->text.c_str(), x + 3, y + 2, tc, 1.0f);
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
        if (cbTex && cbTex->loaded)
            r.drawTexturedRect({x, y, 0}, {x + sz, y + sz, 0}, cbTex->id);
        else
            r.drawRectFill({x, y, 0}, {x + sz, y + sz, 0}, fc);
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
            auto fci = prof->fields.find("fontColor"); if (fci != prof->fields.end()) parseColor(fci->second.toString(), txc);
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
            GLint old; glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &old);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            auto* ss = ShaderManager::getSpriteShader(); if (ss) {
                ss->bind(); ss->setUniform("uProjection",r.projection); ss->setUniform("uView",r.view);
                ss->setUniform("uUseTexture",int32_t(1)); ss->setUniform("uTexture",int32_t(0));
                glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, fillTex->id);
                struct SV{float x,y,z;float u,v;float r,g,b,a;};
                SV v[4]={{x,y,0,0,0,1,1,1,1},{x+ctl->extentX,y,0,ctl->extentX/src.w,0,1,1,1,1},{x,y+ctl->extentY,0,0,ctl->extentY/src.h,1,1,1,1},{x+ctl->extentX,y+ctl->extentY,0,ctl->extentX/src.w,ctl->extentY/src.h,1,1,1,1}};
                uint32_t ids[]={0,1,2,1,3,2},vao,vbo,ebo;
                glGenVertexArrays(1,&vao);glGenBuffers(1,&vbo);glGenBuffers(1,&ebo);
                glBindVertexArray(vao);glBindBuffer(GL_ARRAY_BUFFER,vbo);glBufferData(GL_ARRAY_BUFFER,sizeof(v),v,GL_STREAM_DRAW);
                glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(SV),0);glEnableVertexAttribArray(0);
                glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)12);glEnableVertexAttribArray(1);
                glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)20);glEnableVertexAttribArray(2);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo);glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(ids),ids,GL_STREAM_DRAW);
                glDisable(GL_CULL_FACE);glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,0);
                glDeleteVertexArrays(1,&vao);glDeleteBuffers(1,&vbo);glDeleteBuffers(1,&ebo);
            }
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,old); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,old);
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
                    uint32_t ids[]={0,1,2,1,3,2},a,b,c;
                    glGenVertexArrays(1,&a);glGenBuffers(1,&b);glGenBuffers(1,&c);
                    glBindVertexArray(a);glBindBuffer(GL_ARRAY_BUFFER,b);glBufferData(GL_ARRAY_BUFFER,sizeof(v),v,GL_STREAM_DRAW);
                    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(SV),0);glEnableVertexAttribArray(0);
                    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)12);glEnableVertexAttribArray(1);
                    glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)20);glEnableVertexAttribArray(2);
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,c);glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(ids),ids,GL_STREAM_DRAW);
                    glDisable(GL_CULL_FACE);glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,0);
                    glDeleteVertexArrays(1,&a);glDeleteBuffers(1,&b);glDeleteBuffers(1,&c);
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
                        uint32_t a,b,c;glGenVertexArrays(1,&a);glGenBuffers(1,&b);glGenBuffers(1,&c);
                        glBindVertexArray(a);glBindBuffer(GL_ARRAY_BUFFER,b);glBufferData(GL_ARRAY_BUFFER,sizeof(v),v,GL_STREAM_DRAW);
                        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(SV),0);glEnableVertexAttribArray(0);
                        glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)12);glEnableVertexAttribArray(1);
                        glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)20);glEnableVertexAttribArray(2);
                        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,c);glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(ids),ids,GL_STREAM_DRAW);
                        glDisable(GL_CULL_FACE);glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,0);
                        glDeleteVertexArrays(1,&a);glDeleteBuffers(1,&b);glDeleteBuffers(1,&c);
                        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,ow);
                    } else {
                        SV v[4]={{qx,qy,0,u0,v0,1,1,1,1},{qx+qw,qy,0,u1,v0,1,1,1,1},{qx,qy+qh,0,u0,v1,1,1,1,1},{qx+qw,qy+qh,0,u1,v1,1,1,1,1}};
                        uint32_t a,b,c;glGenVertexArrays(1,&a);glGenBuffers(1,&b);glGenBuffers(1,&c);
                        glBindVertexArray(a);glBindBuffer(GL_ARRAY_BUFFER,b);glBufferData(GL_ARRAY_BUFFER,sizeof(v),v,GL_STREAM_DRAW);
                        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(SV),0);glEnableVertexAttribArray(0);
                        glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)12);glEnableVertexAttribArray(1);
                        glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,sizeof(SV),(void*)20);glEnableVertexAttribArray(2);
                        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,c);glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(ids),ids,GL_STREAM_DRAW);
                        glDisable(GL_CULL_FACE);glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,0);
                        glDeleteVertexArrays(1,&a);glDeleteBuffers(1,&b);glDeleteBuffers(1,&c);
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
        ColorF fc{0.3f,0.3f,0.4f,1}, bc{0.4f,0.4f,0.5f,1}, txc{1,1,1,1};
        std::string bmp;
        auto* prof = getProfile(ctl->profileName);
        if (prof) {
            auto fi = prof->fields.find("fillColor"); if (fi != prof->fields.end()) parseColor(fi->second.toString(), fc);
            auto fci = prof->fields.find("fontColor"); if (fci != prof->fields.end()) parseColor(fci->second.toString(), txc);
            auto bi = prof->fields.find("bitmap"); if (bi != prof->fields.end()) bmp = bi->second.toString();
        }
        Texture* tabTex = nullptr;
        if (!bmp.empty()) { tabTex = r.loadTexture((bmp + ".png").c_str()); if (!tabTex) tabTex = r.loadTexture(("textures/" + bmp + ".png").c_str()); }
        if (!tabTex) tabTex = getShellTex(r, "shll_button.png");
        if (tabTex && tabTex->loaded) {
            r.drawTexturedRect({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, tabTex->id);
        } else {
            r.drawRectFill({x-1, y-1, 0}, {x + ctl->extentX + 1, y + ctl->extentY + 1, 0}, bc);
            r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, fc);
        }
        if (font && !ctl->text.empty())
            font->render(ctl->text.c_str(), x + 4, y + (ctl->extentY - 16) * 0.5f, txc, 1.0f);
    } else if (cn == "GuiPopUpMenuCtrl" || cn == "ShellPopupMenu") {
        ColorF fc{0.25f,0.25f,0.3f,1}, txc{0.8f,0.8f,1,1};
        auto* prof = getProfile(ctl->profileName);
        if (prof) {
            auto fi = prof->fields.find("fillColor"); if (fi != prof->fields.end()) parseColor(fi->second.toString(), fc);
            auto fci = prof->fields.find("fontColor"); if (fci != prof->fields.end()) parseColor(fci->second.toString(), txc);
        }
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, fc);
        r.drawRectFill({x + ctl->extentX - 16, y + 4, 0}, {x + ctl->extentX - 4, y + ctl->extentY - 4, 0}, {0.5f,0.5f,0.6f,1});
        if (font && !ctl->text.empty())
            font->render(ctl->text.c_str(), x + 4, y + 2, txc, 1.0f);
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
    } else if (cn == "GuiScrollContentCtrl") {
        ColorF scc{0.15f,0.15f,0.2f,0.3f};
        auto* prof = getProfile(ctl->profileName);
        if (prof) { auto fi = prof->fields.find("fillColor"); if (fi != prof->fields.end()) parseColor(fi->second.toString(), scc); }
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, scc);
    } else {
        // Generic GuiControl with profile-aware fill
        ColorF gc{0.2f, 0.2f, 0.25f, 0.3f};
        auto* prof = getProfile(ctl->profileName);
        if (prof) {
            auto fi = prof->fields.find("fillColor"); if (fi != prof->fields.end()) parseColor(fi->second.toString(), gc);
            // Check if profile has opaque flag
            auto oi = prof->fields.find("opaque");
            if (oi != prof->fields.end()) { std::string ov = oi->second.toString(); if (ov == "true" || ov == "1") gc.a = 1.0f; }
        }
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, gc);
    }

    // Render children (propagate scroll offset)
    for (auto* child : ctl->children)
        renderControlRec(gr, child, canvas, scrollOfsX, scrollOfsY, clip);
}

void GuiRenderer::update(float dt) {
    (void)dt;
    double now = Engine::instance().timer().now();
    for (auto it = events.begin(); it != events.end(); ) {
        if (now >= it->triggerTime) {
            Console::instance().execute(it->command.c_str());
            it = events.erase(it);
        } else {
            ++it;
        }
    }
}

void GuiRenderer::addSchedule(double delay, const std::string& command) {
    ScheduledEvent ev;
    ev.triggerTime = Engine::instance().timer().now() + delay;
    ev.command = command;
    events.push_back(ev);
}

GuiControl* GuiRenderer::hitTest(GuiControl* ctl, int mx, int my) {
    if (!ctl || !ctl->visible || !ctl->active) return nullptr;
    float x = ctl->posX;
    float y = ctl->posY;
    if (ctl->parent && ctl->parent != canvas) { x += ctl->parent->posX; y += ctl->parent->posY; }
    if (mx >= x && mx < x + ctl->extentX && my >= y && my < y + ctl->extentY) {
        // Check children first (top-most)
        for (auto* child : ctl->children) { auto* h = hitTest(child, mx, my); if (h) return h; }
        return ctl;
    }
    return nullptr;
}

bool GuiRenderer::handleInput(int x, int y, bool pressed) {
    auto* target = dialogStack.empty() ? canvas : activeDialog();
    auto* hit = hitTest(target, x, y);
    if (hit && hit->onClick && pressed) {
        hit->onClick();
        return true;
    }
    return false;
}

void GuiRenderer::pushDialog(const std::string& name) {
    GuiControl* ctl = findControl(name);
    if (ctl) {
        ctl->visible = true;
        dialogStack.push_back(ctl);
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

bool GuiRenderer::isDialogActive(const std::string& name) {
    for (auto* dlg : dialogStack)
        if (dlg->name == name) return true;
    return false;
}

GuiControl* GuiRenderer::findControl(const std::string& name) {
    if (!canvas) return nullptr;
    return canvas->findChild(name);
}
