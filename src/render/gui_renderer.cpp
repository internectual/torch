#include "render/gui_renderer.h"
#include "core/console.h"
#include "core/engine.h"
#include "script/script_engine.h"
#include <GL/glew.h>
#include <algorithm>
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
        if (obj->className.find("Gui") == 0) {
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
            it = obj->fields.find("visible");
            if (it != obj->fields.end()) ctl->visible = it->second.toBool();

            controlMap[name] = ctl;

            if (ctl->className == "GuiCanvas") {
                canvas = ctl;
            }

            // Wire command execution for clickable controls
            if (!ctl->command.empty() &&
                (ctl->className.find("Button") != std::string::npos ||
                 ctl->className == "GuiCheckBoxCtrl" ||
                 ctl->className == "GuiRadioCtrl")) {
                std::string cmd = ctl->command;
                ctl->onClick = [cmd]() {
                    Console::instance().printf(LogLevel::Info, "GUI: %s", cmd.c_str());
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
        if (obj->className.find("Gui") == 0) {
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
    // When no dialog is active, render the full canvas tree
    if (dialogStack.empty()) {
        renderControl(canvas);
    }



    // Restore projection and view
    r.setProjection(savedProj);
    r.setView(savedView);
}

struct ClipRect { float x, y, w, h; };

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

    // Button types
    if (cn == "GuiButtonCtrl" || cn == "GuiTextButtonCtrl" ||
        cn == "ShellBitmapButton" || cn == "GuiBitmapButtonCtrl") {
        r.drawRectFill({x-1, y-1, 0}, {x + ctl->extentX + 1, y + ctl->extentY + 1, 0}, {0.5f, 0.5f, 0.6f, 1});
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.25f, 0.25f, 0.35f, 1});
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
        if (font && !ctl->text.empty())
            font->render(ctl->text.c_str(), x + 5, y + (ctl->extentY - 16) * 0.5f, {1,1,1,1}, 1.0f);
    } else if (cn == "GuiTextCtrl" || cn == "GuiMLTextCtrl") {
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
                        font->render(line.substr(0, brk).c_str(), x + 2, lineY, {1,1,1,1}, 1.0f);
                        lineY += ch;
                        line = line.substr(brk + 1);
                    }
                }
                font->render(line.c_str(), x + 2, lineY, {1,1,1,1}, 1.0f);
                lineY += ch;
                pos = (nl != std::string::npos) ? nl + 1 : txt.size();
            }
        }
    } else if (cn == "GuiBitmapCtrl" || cn == "GuiChunkedBitmapCtrl") {
        if (!ctl->bitmap.empty()) {
            // Strip gui/ prefix if present, then look in textures/gui/
            std::string fname = ctl->bitmap;
            if (fname.find("gui/") == 0) fname = fname.substr(4);
            auto sl = fname.rfind('/');
            std::string baseName = (sl != std::string::npos) ? fname.substr(sl + 1) : fname;
            auto dot = baseName.rfind('.');
            std::string stem = (dot != std::string::npos) ? baseName.substr(0, dot) : baseName;
            Texture* tex = nullptr;
            // Try exact match first
            tex = r.loadTexture(("textures/gui/" + baseName).c_str());
            // Try capitalizing first letter
            if (!tex && !stem.empty()) {
                std::string cap = stem;
                cap[0] = (char)toupper(cap[0]);
                for (auto& ext : {".png", ".jpg", ".bm8"}) {
                    tex = r.loadTexture(("textures/gui/" + cap + ext).c_str());
                    if (tex) break;
                }
            }
            if (tex && tex->loaded)
                r.drawTexturedRect({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, tex->id);
        }
    } else if (cn == "GuiTextEditCtrl" || cn == "ShellTextEditCtrl" ||
               cn == "GuiLoginPasswordCtrl") {
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.3f, 0.3f, 0.35f, 1});
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.4f, 0.4f, 0.5f, 0.3f});
        if (font) {
            std::string display = ctl->text.empty() ? "..." : ctl->text;
            font->render(display.c_str(), x + 3, y + 2, {0.8f,0.8f,1,1}, 1.0f);
        }
    } else if (cn == "GuiListBoxCtrl" || cn == "ShellTextList" || cn == "GuiTextListCtrl") {
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.2f, 0.2f, 0.25f, 1});
        if (font && !ctl->text.empty())
            font->render(ctl->text.c_str(), x + 3, y + 2, {0.8f,0.8f,1,1}, 1.0f);
    } else if (cn == "GuiCheckBoxCtrl" || cn == "GuiRadioCtrl" ||
               cn == "ShellToggleButton" || cn == "ShellRadioButton") {
        float sz = 16;
        r.drawRectFill({x, y, 0}, {x + sz, y + sz, 0}, {0.5f, 0.5f, 0.6f, 1});
        if (font && !ctl->text.empty())
            font->render(ctl->text.c_str(), x + sz + 4, y + 2, {1,1,1,1}, 1.0f);
    } else if (cn == "GuiSliderCtrl" || cn == "ShellSliderCtrl") {
        float barY = y + ctl->extentY * 0.4f;
        float barH = ctl->extentY * 0.2f;
        r.drawRectFill({x, barY, 0}, {x + ctl->extentX, barY + barH, 0}, {0.4f,0.4f,0.5f,1});
        float knobX = x + ctl->extentX * 0.5f;
        r.drawRectFill({knobX-4, y, 0}, {knobX+4, y+ctl->extentY, 0}, {0.7f,0.7f,0.8f,1});
    } else if (cn == "GuiProgressCtrl") {
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.2f, 0.2f, 0.25f, 1});
        float progress = 0.5f;
        r.drawRectFill({x, y, 0}, {x + ctl->extentX * progress, y + ctl->extentY, 0}, {0.0f, 0.4f, 0.8f, 1});
    } else if (cn == "GuiConsole") {
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0, 0, 0, 0.75f});
        if (font) {
            // Find scroll offsets from parent scroll control
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
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0, 0, 0, 0.85f});
        if (font) {
            std::string prompt = "> " + ctl->text;
            static float cursorTimer = 0;
            cursorTimer += 0.02f;
            bool cursorOn = ((int)(cursorTimer / 0.4f) % 2) == 0;
            if (cursorOn) prompt += '_';
            font->render(prompt.c_str(), x + 4, y + 3, {0, 1, 0, 1}, 1.5f);
        }
    } else if (cn == "ShellPaneCtrl" || cn == "GuiPaneControl" ||
               cn.find("ShellPane") == 0 || cn == "ShellDlgFrame") {
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.15f, 0.15f, 0.2f, 0.9f});
        r.drawBox({{x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}}, {0.3f, 0.3f, 0.4f, 1});
        if (!ctl->text.empty() && font)
            font->render(ctl->text.c_str(), x + 6, y + 4, {1, 1, 1, 1}, 1.5f);
    } else if (cn == "ShellTabButton" || cn == "GuiTabPageCtrl") {
        r.drawRectFill({x-1, y-1, 0}, {x + ctl->extentX + 1, y + ctl->extentY + 1, 0}, {0.4f, 0.4f, 0.5f, 1});
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.3f, 0.3f, 0.4f, 1});
        if (font && !ctl->text.empty())
            font->render(ctl->text.c_str(), x + 4, y + (ctl->extentY - 16) * 0.5f, {1,1,1,1}, 1.0f);
    } else if (cn == "GuiPopUpMenuCtrl" || cn == "ShellPopupMenu") {
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.25f, 0.25f, 0.3f, 1});
        r.drawRectFill({x + ctl->extentX - 16, y + 4, 0}, {x + ctl->extentX - 4, y + ctl->extentY - 4, 0}, {0.5f,0.5f,0.6f,1});
        if (font && !ctl->text.empty())
            font->render(ctl->text.c_str(), x + 4, y + 2, {0.8f,0.8f,1,1}, 1.0f);
    } else if (cn == "GuiScrollCtrl" || cn == "ShellScrollCtrl") {
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.18f, 0.18f, 0.22f, 1});

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

        // Scrollbar thumb (vertical)
        if (maxScroll > 0) {
            float thumbH = ctl->extentY * (ctl->extentY / ctl->contentH);
            if (thumbH < 16) thumbH = 16;
            if (thumbH > ctl->extentY) thumbH = ctl->extentY;
            float thumbY = (1.0f - ctl->scrollY / maxScroll) * (ctl->extentY - thumbH);
            float sbW = 8;
            r.drawRectFill({x + ctl->extentX - sbW, y + thumbY, 0},
                          {x + ctl->extentX, y + thumbY + thumbH, 0},
                          {0.5f, 0.5f, 0.6f, 0.7f});
        }
        // Horizontal scrollbar
        float maxScrollX = ctl->contentW - ctl->extentX;
        if (maxScrollX > 0) {
            ctl->scrollX = (ctl->scrollX < 0) ? 0 : ctl->scrollX;
            if (ctl->scrollX > maxScrollX) ctl->scrollX = maxScrollX;
            float thumbW = ctl->extentX * (ctl->extentX / ctl->contentW);
            if (thumbW < 16) thumbW = 16;
            if (thumbW > ctl->extentX) thumbW = ctl->extentX;
            float thumbX = (1.0f - ctl->scrollX / maxScrollX) * (ctl->extentX - thumbW);
            float sbH = 8;
            float sbY2 = y + ctl->extentY - sbH;
            r.drawRectFill({x + thumbX, sbY2, 0}, {x + thumbX + thumbW, sbY2 + sbH, 0},
                          {0.5f, 0.5f, 0.6f, 0.7f});
        }
        return; // Don't do default child rendering below
    } else if (cn == "GuiScrollContentCtrl") {
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.15f, 0.15f, 0.2f, 0.3f});
    } else {
        // Generic GuiControl
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.2f, 0.2f, 0.25f, 0.3f});
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
    auto* hit = hitTest(activeDialog(), x, y);
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
