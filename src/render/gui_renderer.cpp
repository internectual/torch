#include "render/gui_renderer.h"
#include "core/console.h"
#include "core/engine.h"
#include "script/script_engine.h"
#include <algorithm>
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
                // Orphan controls go directly on canvas
                canvas->addChild(ctl);
            }
        }
    }

    if (!canvas) {
        canvas = new GuiControl;
        canvas->name = "Canvas";
        canvas->className = "GuiCanvas";
        canvas->extentX = 1024;
        canvas->extentY = 768;
    }

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

    // Background
    r.drawBox({{0,0,0}, {1024,768,0}}, {0.15f,0.15f,0.2f,1});

    renderControl(canvas);
}

void GuiRenderer::renderControl(GuiControl* ctl) {
    if (!ctl || !ctl->visible) return;

    auto& r = Engine::instance().renderer();
    auto* font = r.getFont();

    float x = ctl->posX;
    float y = ctl->posY;

    // Add parent offset
    GuiControl* p = ctl->parent;
    while (p && p != canvas) {
        x += p->posX;
        y += p->posY;
        p = p->parent;
    }

    // For GuiCanvas, just use full screen
    if (ctl->className == "GuiCanvas") {
        r.drawBox({{0,0,0}, {1024,768,0}}, {0.15f,0.15f,0.2f,1});
        for (auto* child : ctl->children) renderControl(child);
        return;
    }

    // Render based on class name
    if (ctl->className == "GuiButtonCtrl" || ctl->className == "GuiTextButtonCtrl") {
        r.drawBox({{x-1, y-1, 0}, {x + ctl->extentX + 1, y + ctl->extentY + 1, 0}}, {0.5f, 0.5f, 0.6f, 1});
        r.drawBox({{x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}}, {0.25f, 0.25f, 0.35f, 1});
        if (font && !ctl->text.empty())
            font->render(ctl->text.c_str(), x + 5, y + (ctl->extentY - 16) * 0.5f, {1,1,1,1}, 1.0f);
    } else if (ctl->className == "GuiTextCtrl") {
        if (font && !ctl->text.empty())
            font->render(ctl->text.c_str(), x + 2, y + 2, {1,1,1,1}, 1.0f);
    } else if (ctl->className == "GuiBitmapCtrl") {
        // Try to load bitmap
        if (!ctl->bitmap.empty()) {
            auto data = Engine::instance().fs().read(ctl->bitmap.c_str());
            if (data.empty()) {
                // Try common paths
                std::string b = "textures/" + ctl->bitmap;
                data = Engine::instance().fs().read(b.c_str());
            }
            if (!data.empty()) {
                Texture tex;
                tex.load(data.data(), data.size());
                if (tex.loaded) {
                    // Render using texture coordinates (simplified)
                    r.drawBox({{x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}}, {1,1,1,1});
                }
            }
        }
    } else if (ctl->className == "GuiTextEditCtrl") {
        r.drawBox({{x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}}, {0.3f, 0.3f, 0.35f, 1});
        r.drawBox({{x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}}, {0.4f, 0.4f, 0.5f, 0.3f});
        if (font) {
            std::string display = ctl->text.empty() ? "..." : ctl->text;
            font->render(display.c_str(), x + 3, y + 2, {0.8f,0.8f,1,1}, 1.0f);
        }
    } else if (ctl->className == "GuiListBoxCtrl") {
        r.drawBox({{x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}}, {0.2f, 0.2f, 0.25f, 1});
    } else if (ctl->className == "GuiScrollCtrl") {
        r.drawBox({{x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}}, {0.18f, 0.18f, 0.22f, 1});
    } else if (ctl->className == "GuiCheckBoxCtrl" || ctl->className == "GuiRadioCtrl") {
        r.drawBox({{x, y, 0}, {x + 16, y + 16, 0}}, {0.5f, 0.5f, 0.6f, 1});
        if (font && !ctl->text.empty())
            font->render(ctl->text.c_str(), x + 20, y + 2, {1,1,1,1}, 1.0f);
    } else if (ctl->className == "GuiSliderCtrl") {
        float barY = y + ctl->extentY * 0.4f;
        float barH = ctl->extentY * 0.2f;
        r.drawBox({{x, barY, 0}, {x + ctl->extentX, barY + barH, 0}}, {0.4f,0.4f,0.5f,1});
        float knobX = x + ctl->extentX * 0.5f;
        r.drawBox({{knobX-4, y, 0}, {knobX+4, y+ctl->extentY, 0}}, {0.7f,0.7f,0.8f,1});
    } else {
        // Generic GuiControl
        r.drawBox({{x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}}, {0.2f, 0.2f, 0.25f, 0.3f});
    }

    // Render children
    for (auto* child : ctl->children) renderControl(child);
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
    auto* hit = hitTest(canvas, x, y);
    if (hit && hit->onClick && pressed) {
        hit->onClick();
        return true;
    }
    return false;
}

GuiControl* GuiRenderer::findControl(const std::string& name) {
    if (!canvas) return nullptr;
    return canvas->findChild(name);
}
