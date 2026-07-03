#include "render/gui_renderer.h"
#include "core/console.h"
#include "core/engine.h"
#include "script/script_engine.h"
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
        r.drawRectFill({x-1, y-1, 0}, {x + ctl->extentX + 1, y + ctl->extentY + 1, 0}, {0.5f, 0.5f, 0.6f, 1});
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.25f, 0.25f, 0.35f, 1});
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
                    r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {1,1,1,1});
                }
            }
        }
    } else if (ctl->className == "GuiTextEditCtrl") {
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.3f, 0.3f, 0.35f, 1});
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.4f, 0.4f, 0.5f, 0.3f});
        if (font) {
            std::string display = ctl->text.empty() ? "..." : ctl->text;
            font->render(display.c_str(), x + 3, y + 2, {0.8f,0.8f,1,1}, 1.0f);
        }
    } else if (ctl->className == "GuiListBoxCtrl") {
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.2f, 0.2f, 0.25f, 1});
    } else if (ctl->className == "GuiScrollCtrl") {
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.18f, 0.18f, 0.22f, 1});
    } else if (ctl->className == "GuiCheckBoxCtrl" || ctl->className == "GuiRadioCtrl") {
        r.drawRectFill({x, y, 0}, {x + 16, y + 16, 0}, {0.5f, 0.5f, 0.6f, 1});
        if (font && !ctl->text.empty())
            font->render(ctl->text.c_str(), x + 20, y + 2, {1,1,1,1}, 1.0f);
    } else if (ctl->className == "GuiSliderCtrl") {
        float barY = y + ctl->extentY * 0.4f;
        float barH = ctl->extentY * 0.2f;
        r.drawRectFill({x, barY, 0}, {x + ctl->extentX, barY + barH, 0}, {0.4f,0.4f,0.5f,1});
        float knobX = x + ctl->extentX * 0.5f;
        r.drawRectFill({knobX-4, y, 0}, {knobX+4, y+ctl->extentY, 0}, {0.7f,0.7f,0.8f,1});
    } else if (ctl->className == "GuiConsole") {
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0, 0, 0, 0.75f});
        if (font) {
            auto& lines = Console::instance().getLog();
            float ly = y + ctl->extentY - 12;
            for (int i = (int)lines.size() - 1; i >= 0 && ly >= y; i--, ly -= 12) {
                ColorF col{0.7f, 0.7f, 0.7f, 0.9f};
                const std::string& line = lines[i];
                if (line.find("[ERROR]") == 0)       col = {1, 0.2f, 0.2f, 0.9f};
                else if (line.find("[WARN]") == 0)   col = {1, 0.8f, 0.2f, 0.9f};
                else if (line.find("[INFO]") == 0)   col = {0.7f, 0.7f, 1, 0.9f};
                else if (line.find("[DEBUG]") == 0)  col = {0.5f, 0.5f, 0.5f, 0.7f};
                font->render(line.c_str(), x + 4, ly, col, 1.5f);
            }
        }
    } else if (ctl->className == "GuiConsoleEditCtrl") {
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0, 0, 0, 0.85f});
        if (font) {
            std::string prompt = "> " + ctl->text;
            static float cursorTimer = 0;
            cursorTimer += 0.02f;
            bool cursorOn = ((int)(cursorTimer / 0.4f) % 2) == 0;
            if (cursorOn) prompt += '_';
            font->render(prompt.c_str(), x + 4, y + 3, {0, 1, 0, 1}, 1.5f);
        }
    } else {
        // Generic GuiControl
        r.drawRectFill({x, y, 0}, {x + ctl->extentX, y + ctl->extentY, 0}, {0.2f, 0.2f, 0.25f, 0.3f});
    }

    // Render children
    for (auto* child : ctl->children) renderControl(child);
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
