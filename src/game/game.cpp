#include "game/game.h"
#include <GL/glew.h>
#include "game/demo.h"
#include "game/hud.h"
#include "game/physics.h"
#include "game/mission_parser.h"
#include "render/renderer.h"
#include "render/shader.h"
#include "render/glb_loader.h"
#include "render/gui_renderer.h"
#include "core/console.h"
#include "core/engine.h"
#include <algorithm>
#include "fs/file_system.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cctype>
#include <fstream>
#include <cstdlib>

// ─── Sound helpers ────────────────────────────────────────────────
static void playChatBeep() {
    static SoundBuffer* beepBuf = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        // Generate a 440Hz sine wave beep as WAV
        int sampleRate = 22050;
        int duration = 150; // ms
        int numSamples = sampleRate * duration / 1000;
        int dataSize = numSamples * 2; // 16-bit mono
        // Build WAV header
        struct WavHeader {
            char riff[4] = {'R','I','F','F'};
            uint32_t fileSize;
            char wave[4] = {'W','A','V','E'};
            char fmt[4] = {'f','m','t',' '};
            uint32_t fmtSize = 16;
            uint16_t audioFmt = 1; // PCM
            uint16_t channels = 1;
            uint32_t sampleRate;
            uint32_t byteRate;
            uint16_t blockAlign = 2;
            uint16_t bitsPerSample = 16;
            char dataHdr[4] = {'d','a','t','a'};
            uint32_t dataSize;
        };
        WavHeader hdr;
        hdr.sampleRate = sampleRate;
        hdr.byteRate = sampleRate * 2;
        hdr.dataSize = dataSize;
        hdr.fileSize = 36 + dataSize;
        std::vector<uint8_t> wav(sizeof(hdr) + dataSize);
        memcpy(wav.data(), &hdr, sizeof(hdr));
        // Generate sine wave samples
        int16_t* samples = (int16_t*)(wav.data() + sizeof(hdr));
        for (int i = 0; i < numSamples; i++) {
            float t = (float)i / sampleRate;
            float env = 1.0f - (float)i / numSamples; // fade out
            samples[i] = (int16_t)(sinf(t * 440.0f * 6.28318f) * 8000.0f * env);
        }
        // Apply fade-in
        for (int i = 0; i < 200 && i < numSamples; i++)
            samples[i] = (int16_t)(samples[i] * ((float)i / 200.0f));

        beepBuf = new SoundBuffer;
        if (!beepBuf->loadWav(wav.data(), wav.size())) {
            delete beepBuf;
            beepBuf = nullptr;
        }
    }
    if (!beepBuf) return;
    auto& audio = Engine::instance().audio();
    // Reuse a single source for all chat beeps instead of leaking one per call.
    static SoundSource* beepSrc = nullptr;
    if (!beepSrc) beepSrc = audio.createSource();
    if (beepSrc) {
        beepSrc->setVolume(0.3f);
        beepSrc->stop();
        beepSrc->play(beepBuf);
    }
}
#include <unistd.h>
#include <algorithm>
#include <unordered_map>
#include <set>

// ─── 3D to screen projection ─────────────────────────────────
static Point3F worldToScreen(const Point3F& worldPos, const MatrixF& view, const MatrixF& proj, int screenW, int screenH) {
    const float* v = &view.m[0][0];
    float cx = worldPos.x*v[0]+worldPos.y*v[4]+worldPos.z*v[8]+v[12];
    float cy = worldPos.x*v[1]+worldPos.y*v[5]+worldPos.z*v[9]+v[13];
    float cz = worldPos.x*v[2]+worldPos.y*v[6]+worldPos.z*v[10]+v[14];
    float cw = worldPos.x*v[3]+worldPos.y*v[7]+worldPos.z*v[11]+v[15];
    const float* p = &proj.m[0][0];
    float nx = cx*p[0]+cy*p[4]+cz*p[8]+cw*p[12];
    float ny = cx*p[1]+cy*p[5]+cz*p[9]+cw*p[13];
    float nz = cx*p[2]+cy*p[6]+cz*p[10]+cw*p[14];
    float nw = cx*p[3]+cy*p[7]+cz*p[11]+cw*p[15];
    if (nw == 0) return {-999, -999, 0};
    float invW = 1.0f / nw;
    return {(nx*invW*0.5f+0.5f)*screenW, (-ny*invW*0.5f+0.5f)*screenH, nz*invW};
}

// Forward declarations for demo ghost shape helpers
static const char* shapePathForClass(const std::string& className, const std::string& skinName);
static bool isRenderableGhostClass(const std::string& className);

Player::Player() {
    for (int i = 0; i < gWeaponCount; i++) {
        Weapon w;
        w.type = i;
        w.ammo = gWeaponTable[i].maxAmmo > 0 ? gWeaponTable[i].maxAmmo : 9999;
        w.fireTimer = 0;
        w.reloadTimer = 0;
        w.firing = false;
        w.reloading = false;
        weapons.push_back(w);
    }
}
Player::~Player() {}

void Player::update(float dt) {
    if (hp > 0) {
        updateAnimation(dt, false);
    }
}

void Player::loadModel() {
    if (modelLoaded) return;

    auto& fs = Engine::instance().fs();
    std::vector<std::string> paths = {
        "shapes/bioderm_light.dts",
        "shapes/bioderm_light.glb",
        "shapes/bioderm_medium.dts",
        "shapes/bioderm_medium.glb",
        "shapes/bioderm_light",
        "shapes/bioderm_medium",
    };
    for (auto& p : paths) {
        auto data = fs.read(p.c_str());
        if (!data.empty()) {
            modelShape.name = p;
            if (modelShape.load(data.data(), data.size())) {
                // Require reasonable mesh count for a character model
                if (modelShape.meshes.size() < (modelShape.nodes.size() / 4)) {
                    Console::instance().printf(LogLevel::Debug,
                        "Player: '%s' incomplete (%zu meshes), trying next", p.c_str(), modelShape.meshes.size());
                    modelShape = DTSShape{};
                    continue;
                }
                modelLoaded = true;
                Console::instance().printf(LogLevel::Info, "Player: loaded model '%s'", p.c_str());
                return;
            }
        }
    }
    Console::instance().printf(LogLevel::Warn, "Player: no model found, using box fallback");
    modelLoaded = true;
}

void Player::updateAnimation(float dt, bool jetting) {
    animTime += dt;

    AnimState newAnim;
    if (hp <= 0) {
        newAnim = Death;
    } else if (jetting && !onGround) {
        newAnim = Jet;
    } else if (!onGround) {
        newAnim = Jump;
    } else if (fabsf(vel.x) > 0.5f || fabsf(vel.z) > 0.5f) {
        newAnim = Run;
    } else {
        newAnim = Stand;
    }

    if (newAnim != anim) {
        anim = newAnim;
        animTime = 0;
    }
}

void Player::render() {
    loadModel();

    if (modelLoaded) {
        auto& r = Engine::instance().renderer();

        // Build model transform
        MatrixF model;
        Point3F ax = {0, 1, 0};
        model.setRotationAxis(ax, -rot.z);
        model.setTranslation(pos);
        // DTS shapes are Z-up; C converts to Y-up before the Y-up world transform
        r.setModel(model * Math::czUpToYUp());

        // Map animation state to animation name
        const char* animNames[] = { "stand", "run", "jump", "jet", "death" };
        const char* altNames[]  = { "idle",  "run", "jump", "jet", "die"   };
        int idx = (int)anim;

        if (idx >= 0 && idx <= 4) {
            // Search for primary animation; fall back to alt if missing
            auto& animList = modelShape.animations;
            bool found = false;
            for (auto& a : animList)
                if (a.name == animNames[idx]) { found = true; break; }
            modelShape.renderAnimation(found ? animNames[idx] : altNames[idx], animTime);
        } else {
            modelShape.render(0);
        }
    } else {
        // Placeholder - render a simple box
        auto& r = Engine::instance().renderer();
        Box3F box = {{pos.x - 0.4f, pos.y - 1.0f, pos.z - 0.4f},
                     {pos.x + 0.4f, pos.y + 1.0f, pos.z + 0.4f}};
        r.drawBox(box, {0, 1, 0, 1});
    }
}

void Player::applyMove(const Point3F& move, bool jump, bool jet) {
}

void Player::selectWeapon(int32_t idx) {
    if (idx >= 0 && idx < (int32_t)weapons.size()) {
        curWeapon = idx;
        weapons[curWeapon].reloading = false;
        weapons[curWeapon].reloadTimer = 0;
    }
}

void Player::fireWeapon(bool alt) {
    if (curWeapon < 0 || curWeapon >= (int32_t)weapons.size()) return;
    Weapon& w = weapons[curWeapon];
    const WeaponData& wd = gWeaponTable[w.type];
    if (!w.canFire(eng)) return;

    eng -= wd.energyCost;
    w.fireTimer = 1.0f / wd.fireRate;

    Point3F cpos = cameraPos();
    Point3F target = cameraTarget();
    Point3F dir = {target.x - cpos.x, target.y - cpos.y, target.z - cpos.z};
    float dlen = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dlen < 0.001f) return;
    dir.x /= dlen; dir.y /= dlen; dir.z /= dlen;

    Projectile p;
    p.pos = computeProjectileSpawn(cpos, dir);
    p.vel = {dir.x * wd.speed, dir.y * wd.speed, dir.z * wd.speed};
    p.type = wd.projectileType;
    p.damage = wd.damage;
    p.splashRadius = wd.splashRadius;
    p.lifetime = 5.0f;
    p.active = true;
    p.ownerId = 0;
    p.weaponType = curWeapon;

    if (wd.projectileType == ProjectileType::Hitscan) {
        p.lifetime = 2.0f;
        p.vel = {dir.x * 500.0f, dir.y * 500.0f, dir.z * 500.0f};
    }

    Engine::instance().game().world().spawnProjectile(p);

    // Play fire sound
    loadWeaponSounds(w);
    if (w.fireSound) {
        auto& audio = Engine::instance().audio();
        auto* src = audio.createSource();
        if (src) {
            src->setPosition(pos);
            src->positional = true;
            src->setVolume(0.5f);
            src->play(w.fireSound);
            // Sources auto-clean via audio system update
        }
    }

    if (wd.maxAmmo > 0) {
        w.ammo--;
        if (w.ammo <= 0) w.ammo = 0;
    }
}

void Player::weaponCycle(int32_t dir) {
    if (weapons.empty()) return;
    int32_t next = curWeapon + dir;
    if (next < 0) next = (int32_t)weapons.size() - 1;
    if (next >= (int32_t)weapons.size()) next = 0;
    selectWeapon(next);
}

void Player::applyDamage(float amount) {
    if (amount < 0) {
        // Healing
        hp -= amount; // amount is negative, so this adds
        if (hp > 100) hp = 100;
        return;
    }
    if (arm > 0) {
        float absorbed = Math::min(arm, amount * 0.6f);
        arm -= absorbed;
        hp -= amount - absorbed;
    } else {
        hp -= amount;
    }
    if (hp <= 0) {
        hp = 0;
        deaths++;
    }
}

void Player::respawn() {
    hp = 100.0f;
    eng = 100.0f;
    arm = 0.0f;
    vel = {0,0,0};
    pos = {0, 10, 0};
    onGround = false;
}

Point3F Player::cameraPos() const {
    return {pos.x, pos.y + eyeHeight, pos.z};
}

Point3F Player::cameraTarget() const {
    float cx = pos.x + std::sin(rot.z) * std::cos(rot.x);
    float cy = pos.y + eyeHeight + std::sin(rot.x);
    float cz = pos.z + std::cos(rot.z) * std::cos(rot.x);
    return {cx, cy, cz};
}

World::World() {}
World::~World() {}

bool World::load(const char* mapName) {
    Console::instance().printf(LogLevel::Info, "Loading map: %s", mapName);

    auto& fs = Engine::instance().fs();

    // Try to load mission file
    std::string misPath = std::string("missions/") + mapName + ".mis";
    std::string misData = fs.readText(misPath.c_str());

    if (misData.empty()) {
        // Try alternative case
        misPath = std::string("Missions/") + mapName + ".mis";
        misData = fs.readText(misPath.c_str());
    }

    if (!misData.empty()) {
        Console::instance().printf(LogLevel::Info, "Found mission: %s (%zu bytes, first 30: '%s')", misPath.c_str(), misData.size(),
            misData.substr(0, 30).c_str());

        auto objects = parseMisFile(misData);

        // Find TerrainBlock
        MisObject* terrainObj = findObject(objects, "TerrainBlock");
        if (terrainObj) {
            std::string terrainFile = getProp(terrainObj->props, "terrainfile");
            Console::instance().printf(LogLevel::Info, "  terrain file: '%s'", terrainFile.c_str());

            // Try loading the .ter file from various paths
            std::vector<std::string> terPaths = {
                terrainFile,
                "missions/" + terrainFile,
                "terrains/" + terrainFile,
                terrainFile + ".ter",
                "missions/" + terrainFile + ".ter",
                "terrains/" + terrainFile + ".ter"
            };

            // Read terrain positioning
            std::string sqStr = getProp(terrainObj->props, "squaresize");
            if (!sqStr.empty()) terrainBlock.squareSize = (float)std::atof(sqStr.c_str());
            Console::instance().printf(LogLevel::Debug, "  terrain squareSize: %.1f", terrainBlock.squareSize);
            std::string posStr = getProp(terrainObj->props, "position");
            if (!posStr.empty()) {
                float px, py, pz;
                if (sscanf(posStr.c_str(), "%f %f %f", &px, &py, &pz) == 3) {
                    terrainBlock.worldOffset = {px, py, pz};
                    Console::instance().printf(LogLevel::Debug, "  terrain position: %.1f %.1f %.1f", px, py, pz);
                }
            }

            for (auto& tp : terPaths) {
                auto terData = fs.read(tp.c_str());
                if (!terData.empty()) {
                    Console::instance().printf(LogLevel::Info, "  loaded terrain: %s", tp.c_str());
                    terrainBlock.load(terData.data(), terData.size());
                    break;
                }
            }

            if (!terrainBlock.loaded) {
                // Still try just .ter extension
                std::string tryPath = terrainFile;
                if (tryPath.size() < 4 || tryPath.substr(tryPath.size()-4) != ".ter")
                    tryPath += ".ter";
                auto terData = fs.read(tryPath.c_str());
                if (!terData.empty()) {
                    terrainBlock.load(terData.data(), terData.size());
                }
            }
        }

        // Find Sky
        MisObject* skyObj = findObject(objects, "Sky");
        if (skyObj) {
            skyMaterialList = getProp(skyObj->props, "materialList");
            Console::instance().printf(LogLevel::Info, "  has Sky object, materialList: '%s'", skyMaterialList.c_str());

            // Parse fog from Sky
            std::string fogStr = getProp(skyObj->props, "fogcolor");
            if (!fogStr.empty()) {
                float fr, fg, fb;
                if (sscanf(fogStr.c_str(), "%f %f %f", &fr, &fg, &fb) >= 3) {
                    fog.color = {fr, fg, fb, 1.0f};
                    fog.enabled = true;
                }
            }
            std::string fogDist = getProp(skyObj->props, "fogdistance");
            if (!fogDist.empty()) {
                fog.distance = (float)std::atof(fogDist.c_str());
                fog.density = 1.0f / fog.distance;
                fog.enabled = true;
            }
            Console::instance().printf(LogLevel::Debug, "  fog: enabled=%d color=(%.2f %.2f %.2f) density=%.4f dist=%.0f",
                fog.enabled, fog.color.r, fog.color.g, fog.color.b, fog.density, fog.distance);
        }

        // Parse Sun from mission for dynamic lighting
        MisObject* sunObj = findObject(objects, "Sun");
        if (sunObj) {
            std::string azStr = getProp(sunObj->props, "azimuth");
            std::string elStr = getProp(sunObj->props, "elevation");
            std::string colStr = getProp(sunObj->props, "color");
            if (!azStr.empty() && !elStr.empty()) {
                float azimuth = (float)std::atof(azStr.c_str()) * (3.14159f / 180.0f);
                float elevation = (float)std::atof(elStr.c_str()) * (3.14159f / 180.0f);
                sunLightDir.x = cosf(elevation) * sinf(azimuth);
                sunLightDir.y = sinf(elevation);
                sunLightDir.z = cosf(elevation) * cosf(azimuth);
                sunLightDirUsed = true;
                Console::instance().printf(LogLevel::Debug, "  sun: azimuth=%.0f elevation=%.0f dir=(%.2f %.2f %.2f)",
                    std::atof(azStr.c_str()), std::atof(elStr.c_str()), sunLightDir.x, sunLightDir.y, sunLightDir.z);
            }
            if (!colStr.empty()) {
                sscanf(colStr.c_str(), "%f %f %f", &sunColor.r, &sunColor.g, &sunColor.b);
                sunColorUsed = true;
            }
        }

        // Collect all unique shape names referenced in the mission
        std::vector<std::string> shapeNames;
        auto addShapeName = [&](const std::string& n) {
            if (n.empty()) return;
            // Clean trailing quote from mis parsing
            std::string clean = n;
            if (!clean.empty() && clean.back() == '"') clean.pop_back();
            if (clean.empty()) return;
            bool found = false;
            for (auto& s : shapeNames) if (s == clean) { found = true; break; }
            if (!found) shapeNames.push_back(clean);
        };

        for (auto& obj : objects) {
            if (obj.className == "InteriorInstance" || obj.className == "TSStatic" ||
                obj.className == "StaticShape") {
                addShapeName(getProp(obj.props, "shapename"));
                addShapeName(getProp(obj.props, "interiorFile"));
            }
        }

        // Load all unique shapes
        for (auto& shapeName : shapeNames) {
            DTSShape shape;
            shape.name = shapeName;

            size_t dot = shapeName.rfind('.');
            std::string base = (dot != std::string::npos) ? shapeName.substr(0, dot) : shapeName;
            std::string ext = (dot != std::string::npos) ? shapeName.substr(dot) : "";

            shape.isInterior = (ext == ".dif");

            // Search paths in priority order: native DTS/DIF first, then GLB conversion
            std::vector<std::string> searchPaths;
            std::string dir = shape.isInterior ? "interiors/" : "shapes/";

            // Try native format with original extension
            searchPaths.push_back(dir + base + ext);
            // Try native format with .dts/.dif extension matching type
            searchPaths.push_back(dir + base + (shape.isInterior ? ".dif" : ".dts"));
            // Try GLB conversion path
            searchPaths.push_back(dir + base + ".glb");
            // Try alternative dir
            if (!shape.isInterior)
                searchPaths.push_back("interiors/" + base + ".glb");
            // Try stripping TR2 prefix
            if (base.compare(0, 3, "TR2") == 0) {
                std::string stripped = base.substr(3);
                searchPaths.push_back(dir + stripped + ".glb");
                searchPaths.push_back(dir + stripped + ".dts");
                if (!shape.isInterior)
                    searchPaths.push_back("interiors/" + stripped + ".glb");
            }

            // Try loading without extensions (let DTSShape::load auto-detect)
            searchPaths.push_back(dir + base);

            // For interiors, also try explicit GLB paths that are skipped by the !shape.isInterior guards
            if (shape.isInterior) {
                searchPaths.push_back("interiors/" + base + ".glb");
                searchPaths.push_back(std::string("@vl2/interiors.vl2/interiors/") + base + ".glb");
            }

            std::vector<uint8_t> shapeData;
            std::string foundPath;
            for (auto& p : searchPaths) {
                shapeData = Engine::instance().fs().read(p.c_str());
                if (!shapeData.empty()) {
                    shape.load(shapeData.data(), shapeData.size());
                    if (shape.loaded) {
                        foundPath = p;
                        break;
                    }
                }
            }

    if (shape.loaded) {
        Console::instance().printf(LogLevel::Debug, "  loaded world shape: %s", shapeName.c_str());
            } else {
                Console::instance().printf(LogLevel::Debug, "  world shape not loaded: %s", shapeName.c_str());
            }

            shapes.push_back(std::move(shape));
        }

        // Find player spawn point from SpawnSphere objects
        for (auto& obj : objects) {
            if (obj.className == "SpawnSphere") {
                std::string sp = getProp(obj.props, "position");
                if (!sp.empty()) {
                    float sx, sy, sz;
                    if (sscanf(sp.c_str(), "%f %f %f", &sx, &sy, &sz) >= 3) {
                        playerSpawn = {sx, sy, sz};
                        Console::instance().printf(LogLevel::Debug, "  spawn point: (%.1f, %.1f, %.1f)", sx, sy, sz);
                        break;
                    }
                }
            }
        }

        // Build collision mesh from DIF interior shapes
        // Prefer hull collision data when available (more accurate)
        {
            std::vector<float> allVerts;
            std::vector<uint32_t> allIndices;
            uint32_t vertBase = 0;

            for (auto& s : shapes) {
                if (!s.isInterior || !s.loaded) continue;
                // Use hull collision data if available, otherwise fall back to visual mesh
                if (!s.collisionVerts.empty() && !s.collisionIndices.empty()) {
                    for (auto v : s.collisionVerts) allVerts.push_back(v);
                    for (auto idx : s.collisionIndices) allIndices.push_back(vertBase + idx);
                    vertBase += (uint32_t)s.collisionVerts.size() / 3;
                } else {
                    for (auto& mesh : s.meshes) {
                        for (auto& v : mesh.vertices) {
                            allVerts.push_back(v.pos.x);
                            allVerts.push_back(v.pos.y);
                            allVerts.push_back(v.pos.z);
                        }
                        for (auto idx : mesh.indices) {
                            allIndices.push_back(vertBase + idx);
                        }
                        vertBase += (uint32_t)mesh.vertices.size();
                    }
                }
            }

            if (!allIndices.empty()) {
                interiorCollision.addMesh(allVerts.data(), (int)allVerts.size(), allIndices.data(), (int)allIndices.size());
                interiorCollision.build();
                Console::instance().printf(LogLevel::Info, "Collision mesh built: %zu triangles", interiorCollision.triangles.size());
            }
        }

        // Place objects from mission, mapping to loaded shapes
        for (auto& obj : objects) {
            if (obj.className == "InteriorInstance" || obj.className == "TSStatic" ||
                obj.className == "StaticShape") {
                WorldObject wo;
                wo.pos = parsePos(getProp(obj.props, "position"));
                {
                    std::string rotStr = getProp(obj.props, "rotation");
                    float vals[4] = {0,0,1,0};
                    int count = sscanf(rotStr.c_str(), "%f %f %f %f", &vals[0], &vals[1], &vals[2], &vals[3]);
                    if (count >= 3) {
                        wo.rot = {vals[0], vals[1], vals[2]};
                        wo.rotAngleDeg = (count >= 4) ? vals[3] : 0;
                    }
                }
                wo.shapeName = getProp(obj.props, "shapename");
                if (wo.shapeName.empty())
                    wo.shapeName = getProp(obj.props, "interiorFile");
                // Clean trailing quote
                if (!wo.shapeName.empty() && wo.shapeName.back() == '"')
                    wo.shapeName.pop_back();
                wo.collidable = true;

                // Find matching shape
                for (auto& s : shapes) {
                    if (s.name == wo.shapeName) {
                        wo.shape = &s;
                        // Auto-detect animation: use first animation if present
                        if (!s.animations.empty())
                            wo.animName = s.animations[0].name;
                        break;
                    }
                }
                addObject(wo);
                if (wo.shape) {
                    Console::instance().printf(LogLevel::Debug, "  placed: %s at (%.1f, %.1f, %.1f)",
                        wo.shapeName.c_str(), wo.pos.x, wo.pos.y, wo.pos.z);
                }
            }
        }

        // Parse item pickups
        for (auto& obj : objects) {
            if (obj.className == "Item") {
                std::string db = getProp(obj.props, "datablock");
                std::string posStr = getProp(obj.props, "position");
                ItemPickup::Type type;
                if (db.find("Health") != std::string::npos || db.find("health") != std::string::npos)
                    type = ItemPickup::Health;
                else if (db.find("Energy") != std::string::npos || db.find("energy") != std::string::npos)
                    type = ItemPickup::Energy;
                else if (db.find("Ammo") != std::string::npos || db.find("ammo") != std::string::npos)
                    type = ItemPickup::Ammo;
                else
                    continue;

                ItemPickup item;
                item.pos = parsePos(posStr);
                item.type = type;
                item.respawnTimer = 0;
                item.active = true;
                items.push_back(item);
                Console::instance().printf(LogLevel::Debug, "  item: %s at (%.1f, %.1f, %.1f)",
                    db.c_str(), item.pos.x, item.pos.y, item.pos.z);
            }
        }
    } else {
        Console::instance().printf(LogLevel::Warn, "No mission file found for '%s', using procedural terrain", mapName);
    }

    // Generate terrain if not loaded
    if (!terrainBlock.loaded) {
        terrainBlock.load(nullptr, 0);
    }

    // Load sky from mission materialList
    std::vector<std::string> skyFaces;
    std::string emapPath;
    if (!skyMaterialList.empty()) {
        // Try to load the DML file
        std::string dmlPath = "textures/" + skyMaterialList;
        Console::instance().printf(LogLevel::Debug, "  reading DML: %s", dmlPath.c_str());
        auto dmlData = fs.read(dmlPath.c_str());
        Console::instance().printf(LogLevel::Debug, "  DML read returned %zu bytes", dmlData.size());
        if (dmlData.empty()) {
            dmlPath = skyMaterialList;
            Console::instance().printf(LogLevel::Debug, "  trying DML: %s", dmlPath.c_str());
            dmlData = fs.read(dmlPath.c_str());
            Console::instance().printf(LogLevel::Debug, "  DML read returned %zu bytes", dmlData.size());
        }
        if (!dmlData.empty()) {
            std::string dmlContent((const char*)dmlData.data(), dmlData.size());
            // Parse all lines from DML: first 6 = cubemap faces, 7th = emap, 8-10 = cloud layers
            std::vector<std::string> faceNames;
            size_t pos = 0;
            int lineIdx = 0;
            while (pos < dmlContent.size()) {
                while (pos < dmlContent.size() && (dmlContent[pos] == ' ' || dmlContent[pos] == '\t' || dmlContent[pos] == '\r')) pos++;
                if (pos >= dmlContent.size()) break;
                size_t end = pos;
                while (end < dmlContent.size() && dmlContent[end] != '\n') end++;
                std::string line = dmlContent.substr(pos, end - pos);
                while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
                if (!line.empty()) {
                    if (lineIdx < 6)
                        faceNames.push_back(line);
                    else if (lineIdx == 6)
                        emapPath = line;
                }
                lineIdx++;
                pos = end + 1;
            }

            Console::instance().printf(LogLevel::Debug, "  DML face names (%zu):", faceNames.size());
            for (auto& fn : faceNames) Console::instance().printf(LogLevel::Debug, "    '%s'", fn.c_str());

            if (faceNames.size() >= 6) {
                std::vector<std::string> exts = {".png", ".jpg", ".bm8"};
                for (auto& fn : faceNames) {
                    bool found = false;
                    for (auto& ext : exts) {
                        std::string texPath = "textures/" + fn + ext;
                        Console::instance().printf(LogLevel::Debug, "  trying: %s", texPath.c_str());
                        auto test = fs.read(texPath.c_str());
                        if (!test.empty()) {
                            Console::instance().printf(LogLevel::Debug, "  FOUND: %s (%zu bytes)", texPath.c_str(), test.size());
                            skyFaces.push_back(texPath);
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        for (auto& ext : exts) {
                            std::string texPath = fn + ext;
                            Console::instance().printf(LogLevel::Debug, "  trying (no prefix): %s", texPath.c_str());
                            auto test = fs.read(texPath.c_str());
                            if (!test.empty()) {
                                Console::instance().printf(LogLevel::Debug, "  FOUND: %s (%zu bytes)", texPath.c_str(), test.size());
                                skyFaces.push_back(texPath);
                                found = true;
                                break;
                            }
                        }
                    }
                    if (!found) {
                        Console::instance().printf(LogLevel::Debug, "  NOT FOUND: %s", fn.c_str());
                        skyFaces.clear(); break;
                    }
                }
            } else {
                Console::instance().printf(LogLevel::Warn, "  DML has < 6 face names (%zu)", faceNames.size());
            }
        }
    }

    // Fallback if DML-based loading failed
    if (skyFaces.size() < 6) {
        skyFaces.clear();
        std::vector<std::string> fallbackPaths = {
            "textures/skies/default/right.jpg", "skies/default/right.jpg",
            "textures/gui/splash.jpg"
        };
        for (auto& sp : fallbackPaths) {
            auto test = fs.read(sp.c_str());
            if (!test.empty()) {
                skyFaces = {
                    "textures/skies/default/right.jpg", "textures/skies/default/left.jpg",
                    "textures/skies/default/top.jpg", "textures/skies/default/bottom.jpg",
                    "textures/skies/default/front.jpg", "textures/skies/default/back.jpg"
                };
                break;
            }
        }
    }

    if (skyFaces.size() >= 6) {
        skyBox.load(skyFaces);
        Console::instance().printf(LogLevel::Info, "  sky loaded from: %s", skyMaterialList.c_str());

        // Load environment map (sphere map) from DML line 7
        if (!emapPath.empty()) {
            std::string emapFullPath;
            std::vector<std::string> exts = {".png", ".jpg", ".bm8"};
            // Try textures/<path>.<ext> first
            for (auto& ext : exts) {
                std::string p = "textures/" + emapPath + ext;
                Console::instance().printf(LogLevel::Debug, "  trying emap: %s", p.c_str());
                auto ed = fs.read(p.c_str());
                if (!ed.empty()) {
                    emapFullPath = p;
                    skyBox.emap.load(ed.data(), ed.size());
                    Console::instance().printf(LogLevel::Info, "  emap loaded: %s (%zu bytes)", p.c_str(), ed.size());
                    break;
                }
            }
            if (emapFullPath.empty()) {
                // Try without textures/ prefix
                for (auto& ext : exts) {
                    std::string p = emapPath + ext;
                    Console::instance().printf(LogLevel::Debug, "  trying emap (no prefix): %s", p.c_str());
                    auto ed = fs.read(p.c_str());
                    if (!ed.empty()) {
                        skyBox.emap.load(ed.data(), ed.size());
                        Console::instance().printf(LogLevel::Info, "  emap loaded: %s (%zu bytes)", p.c_str(), ed.size());
                        break;
                    }
                }
            }
            // Fallback: extract basename and search common sky directories
            if (!skyBox.emap.loaded && !emapPath.empty()) {
                size_t slash = emapPath.rfind('/');
                std::string baseName = (slash != std::string::npos) ? emapPath.substr(slash + 1) : emapPath;
                const char* searchDirs[] = {"ice/skies", "desert/skies", "badlands/skies",
                    "lush/skies", "lava/skies", "alpine/skies", "skies"};
                for (auto& dir : searchDirs) {
                    for (auto& ext : exts) {
                        std::string p = std::string("textures/") + dir + "/" + baseName + ext;
                        Console::instance().printf(LogLevel::Debug, "  trying emap fallback: %s", p.c_str());
                        auto ed = fs.read(p.c_str());
                        if (!ed.empty()) {
                            skyBox.emap.load(ed.data(), ed.size());
                            Console::instance().printf(LogLevel::Info, "  emap loaded (fallback): %s (%zu bytes)", p.c_str(), ed.size());
                            break;
                        }
                    }
                    if (skyBox.emap.loaded) break;
                }
            }

            if (!skyBox.emap.loaded) {
                Console::instance().printf(LogLevel::Debug, "  emap NOT FOUND: %s", emapPath.c_str());
            }
        }
    } else {
        Console::instance().printf(LogLevel::Info, "No sky textures found, generating default");
    }

    loaded = true;
    Console::instance().printf(LogLevel::Info, "Map loaded: %s", mapName);
    return true;
}

bool World::loadTerrain(const char* mapName) {
    // Headless terrain-only loader: parse the mission, find the TerrainBlock, and
    // load just the heightfield. Avoids shape/material/GL loading so a dedicated
    // server can register an authoritative ground-height callback.
    auto& fs = Engine::instance().fs();
    std::string misPath = std::string("missions/") + mapName + ".mis";
    std::string misData = fs.readText(misPath.c_str());
    if (misData.empty()) {
        misPath = std::string("Missions/") + mapName + ".mis";
        misData = fs.readText(misPath.c_str());
    }
    if (misData.empty()) return false;

    auto objects = parseMisFile(misData);
    MisObject* terrainObj = findObject(objects, "TerrainBlock");
    if (!terrainObj) return false;

    std::string terrainFile = getProp(terrainObj->props, "terrainfile");
    std::string sqStr = getProp(terrainObj->props, "squaresize");
    if (!sqStr.empty()) terrainBlock.squareSize = (float)std::atof(sqStr.c_str());
    std::string posStr = getProp(terrainObj->props, "position");
    if (!posStr.empty()) {
        float px, py, pz;
        if (sscanf(posStr.c_str(), "%f %f %f", &px, &py, &pz) == 3)
            terrainBlock.worldOffset = {px, py, pz};
    }

    std::vector<std::string> terPaths = {
        terrainFile,
        "missions/" + terrainFile,
        "terrains/" + terrainFile,
        terrainFile + ".ter",
        "missions/" + terrainFile + ".ter",
        "terrains/" + terrainFile + ".ter"
    };
    for (auto& tp : terPaths) {
        auto terData = fs.read(tp.c_str());
        if (!terData.empty()) { terrainBlock.load(terData.data(), terData.size()); break; }
    }
    if (!terrainBlock.loaded) {
        std::string tryPath = terrainFile;
        if (tryPath.size() < 4 || tryPath.substr(tryPath.size() - 4) != ".ter") tryPath += ".ter";
        auto terData = fs.read(tryPath.c_str());
        if (!terData.empty()) terrainBlock.load(terData.data(), terData.size());
    }
    if (terrainBlock.loaded)
        Console::instance().printf(LogLevel::Info, "Server terrain loaded from '%s'", mapName);
    return terrainBlock.loaded;
}

void World::update(float dt) {
    // Update item pickups
    for (auto& item : items) {
        if (!item.active) {
            item.respawnTimer -= dt;
            if (item.respawnTimer <= 0) item.active = true;
            continue;
        }

        // Check player proximity
        auto& game = Engine::instance().game();
        const Point3F& ppos = game.player().position();
        float dx = item.pos.x - ppos.x;
        float dy = item.pos.y - ppos.y;
        float dz = item.pos.z - ppos.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);

        if (dist < 2.0f) {
            item.active = false;
            item.respawnTimer = 15.0f;

            switch (item.type) {
                case ItemPickup::Health:
                    game.player().applyDamage(-25.0f); // negative = heal
                    break;
                case ItemPickup::Energy:
                    game.player().setEnergy(game.player().energy() + 25.0f);
                    break;
                case ItemPickup::Ammo: {
                    int32_t cw = game.player().currentWeapon();
                    if (cw >= 0 && cw < (int32_t)game.player().weaponCount())
                        game.player().weapon(cw).ammo += 15;
                    break;
                }
            }

            Console::instance().printf(LogLevel::Debug, "Picked up item at (%.1f, %.1f, %.1f)",
                item.pos.x, item.pos.y, item.pos.z);
        }
    }

    // Update explosions
    for (auto& e : explosions) {
        e.lifetime -= dt;
        e.radius += dt * 4.0f;
        // Check explosion against bots
        for (auto& b : bots) {
            if (!b.alive) continue;
            float dx = b.pos.x - e.pos.x;
            float dy = b.pos.y - e.pos.y;
            float dz = b.pos.z - e.pos.z;
            float dist = sqrtf(dx*dx + dy*dy + dz*dz);
            if (dist < e.radius) {
                float dmg = 30.0f * (1.0f - dist / e.radius);
                b.health -= dmg;
                if (b.health <= 0) { b.health = 0; b.alive = false; b.respawnTimer = 5.0f; }
                else { b.lastHitTime = Engine::instance().game().gameTime(); }
            }
        }
    }
    explosions.erase(
        std::remove_if(explosions.begin(), explosions.end(),
            [](const Explosion& e) { return e.lifetime <= 0; }),
        explosions.end()
    );

    // Update projectiles
    for (auto& p : projList) {
        if (!p.active) continue;
        updateProjectile(p, dt);

        // Spawn trail particles
        if (std::rand() % 3 == 0) {
            ColorF trailColor;
            switch (p.type) {
                case ProjectileType::Disc:    trailColor = {1.0f, 0.6f, 0.1f, 0.6f}; break;
                case ProjectileType::Bolt:    trailColor = {0.2f, 0.8f, 1.0f, 0.6f}; break;
                case ProjectileType::Grenade:
                case ProjectileType::Mortar:  trailColor = {0.3f, 1.0f, 0.3f, 0.6f}; break;
                default:                      trailColor = {1.0f, 1.0f, 0.5f, 0.6f}; break;
            }
            spawnTrail(p.pos, trailColor, 0.15f);
        }

        // Check for impact
            float groundH = 0;
            if (checkProjectileCollision(p, groundH)) {
                // Spawn explosion effect
                ColorF expColor;
                switch (p.type) {
                    case ProjectileType::Disc:    expColor = {1.0f, 0.6f, 0.1f, 1.0f}; break;
                    case ProjectileType::Bolt:    expColor = {0.2f, 0.8f, 1.0f, 1.0f}; break;
                    case ProjectileType::Grenade:
                    case ProjectileType::Mortar:  expColor = {0.3f, 1.0f, 0.3f, 1.0f}; break;
                    default:                      expColor = {1.0f, 1.0f, 0.5f, 1.0f}; break;
                }
                Explosion exp;
                exp.pos = p.pos;
                exp.lifetime = 0.5f;
                exp.maxLifetime = 0.5f;
                exp.radius = 1.0f;
                exp.color = expColor;
                explosions.push_back(exp);
                // Spawn particles
                spawnExplosion(p.pos, expColor, 1.5f, 25);

                // Play explosion sound
                if (p.weaponType >= 0 && p.weaponType < gWeaponCount) {
                    auto& audio = Engine::instance().audio();
                    const WeaponData& wd = gWeaponTable[p.weaponType];
                    if (wd.explosionSoundPath) {
                        auto* snd = audio.loadSound(wd.explosionSoundPath);
                        if (snd) {
                            auto* src = audio.createSource();
                            if (src) {
                                src->setPosition(p.pos);
                                src->positional = true;
                                src->setVolume(0.7f);
                                src->play(snd);
                            }
                        }
                    }
                }

                p.active = false;
                p.hasImpacted = true;
            }

            // Apply splash damage near impact
            if (p.hasImpacted && p.splashRadius > 0) {
            auto& game = Engine::instance().game();
            const Point3F& ppos = game.player().position();
            float dx = p.pos.x - ppos.x;
            float dy = p.pos.y - ppos.y;
            float dz = p.pos.z - ppos.z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist < p.splashRadius) {
                float factor = 1.0f - dist / p.splashRadius;
                game.player().applyDamage(p.damage * factor * 0.5f);
            }
            // Splash damage bots
            for (auto& b : game.world().bots) {
                if (!b.alive) continue;
                float bdx = p.pos.x - b.pos.x;
                float bdy = p.pos.y - b.pos.y;
                float bdz = p.pos.z - b.pos.z;
                float bdist = sqrtf(bdx*bdx + bdy*bdy + bdz*bdz);
                if (bdist < p.splashRadius) {
                    float factor = 1.0f - bdist / p.splashRadius;
                    b.health -= p.damage * factor;
                    if (b.health <= 0) { b.health = 0; b.alive = false; b.respawnTimer = 5.0f; } else { b.lastHitTime = Engine::instance().game().gameTime(); }
                }
            }
        }
    }

    // Remove inactive projectiles
    projList.erase(
        std::remove_if(projList.begin(), projList.end(),
            [](const Projectile& p) { return !p.active || p.hasImpacted; }),
        projList.end()
    );

    // Update bots
    auto& player = Engine::instance().game().player();
    Point3F ppos = player.position();
    for (auto& b : bots) {
        if (b.alive) {
            float dx = ppos.x - b.pos.x;
            float dz = ppos.z - b.pos.z;
            float distToPlayer = sqrtf(dx*dx + dz*dz); (void)distToPlayer;
            float timeSinceHit = dt > 0 ? (Engine::instance().game().gameTime() - b.lastHitTime) : 999.0f;

            if (timeSinceHit < 2.0f && b.health < 70) {
                // Flee from player when low health and recently hit
                float fleeAngle = atan2f(-dx, -dz);
                b.moveYaw = fleeAngle;
                b.pos.x += sinf(fleeAngle) * dt * 6.0f;
                b.pos.z += cosf(fleeAngle) * dt * 6.0f;
            } else if (timeSinceHit < 4.0f) {
                // Recently hit: face player and strafe
                float faceAngle = atan2f(dx, dz);
                b.moveYaw = faceAngle;
                float strafeAngle = faceAngle + 1.57f;
                b.pos.x += sinf(strafeAngle) * dt * 4.0f;
                b.pos.z += cosf(strafeAngle) * dt * 4.0f;
            } else {
                // Patrol: move in a circle around start position
                b.patrolOffset += dt * 2.0f;
                b.pos.x = b.startPos.x + sinf(b.patrolOffset) * 8.0f;
                b.pos.z = b.startPos.z + cosf(b.patrolOffset) * 8.0f;
                b.moveYaw = b.patrolOffset + 3.14159f;
            }
            // Stay near ground
            float th = Engine::instance().game().world().getHeight(b.pos.x, b.pos.z);
            if (b.pos.y < th + 0.5f) b.pos.y = th + 0.5f;
            b.animTime += dt;
        } else {
            b.respawnTimer -= dt;
            if (b.respawnTimer <= 0) {
                b.health = 100.0f;
                b.alive = true;
                b.pos = b.startPos;
            }
        }
    }

    // Advance animation time for world objects
    for (auto& obj : worldObjects) {
        if (!obj.animName.empty() && obj.shape && obj.shape->loaded)
            obj.animTime += dt;
    }

    // Update particles
    updateParticles(dt);
}

void World::render(const Point3F& cameraPos) {
    if (!loaded) return;

    auto& r = Engine::instance().renderer();

    // Render terrain
    if (terrainBlock.loaded) {
        ShaderManager::getTerrainShader()->bind();
        Point3F terrainLight = sunLightDirUsed ? sunLightDir : Point3F{0.5f, 0.7f, 0.5f};
        terrainBlock.render(cameraPos, fog.enabled, fog.color, fog.density, &terrainLight);
    }

    // Render world objects with default shader
    auto* defShader = ShaderManager::getDefaultShader();
    defShader->bind();
    defShader->setUniform("uCamPos", cameraPos);

    // Apply fog
    defShader->setUniform("uFogEnabled", (int32_t)(fog.enabled ? 1 : 0));
    if (fog.enabled) {
        defShader->setUniform("uFogColor", Point3F{fog.color.r, fog.color.g, fog.color.b});
        defShader->setUniform("uFogDensity", fog.density);
    }

    // Apply sun lighting direction from mission data (or default for demo/procedural)
    if (sunLightDirUsed) {
        defShader->setUniform("uLightDir", sunLightDir);
    } else {
        // Default sun: 45 degrees elevation, from upper-right
        defShader->setUniform("uLightDir", Point3F{0.5f, 0.7f, 0.5f});
    }

    // Bind environment map from sky (for reflections on shapes)
    if (skyBox.emap.loaded) {
        skyBox.emap.bind(2);
        defShader->setUniform("uEnvMap", (int32_t)2);
    }

    for (auto& obj : worldObjects) {
        if (obj.shape && obj.shape->loaded) {
            MatrixF model;
            if (obj.rotAngleDeg != 0 && (obj.rot.x != 0 || obj.rot.y != 0 || obj.rot.z != 0)) {
                // Axis-angle rotation
                Point3F axis = obj.rot;
                float len = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
                if (len > 0.0001f) {
                    axis.x /= len; axis.y /= len; axis.z /= len;
                    model.setRotationAxis(axis, Math::DEG2RAD(obj.rotAngleDeg));
                }
            }
            model.setTranslation(obj.pos);
            r.setModel(model * Math::czUpToYUp());
            if (!obj.animName.empty())
                obj.shape->renderAnimation(obj.animName.c_str(), obj.animTime);
            else
                obj.shape->render(0);
        }
    }

    // Render item pickups
    float time = Engine::instance().game().gameTime();
    for (auto& item : items) {
        if (!item.active) continue;
        float bob = sinf(time * 2.0f + item.pos.x * 0.1f) * 0.3f;
        ColorF col;
        switch (item.type) {
            case ItemPickup::Health: col = {0.2f, 1.0f, 0.2f, 1.0f}; break;
            case ItemPickup::Energy: col = {0.2f, 0.8f, 1.0f, 1.0f}; break;
            case ItemPickup::Ammo:   col = {1.0f, 0.8f, 0.2f, 1.0f}; break;
        }
        Box3F box = {{item.pos.x - 0.4f, item.pos.y - 0.4f + bob, item.pos.z - 0.4f},
                     {item.pos.x + 0.4f, item.pos.y + 0.4f + bob, item.pos.z + 0.4f}};
        r.drawBox(box, col);
    }

    // Render projectiles as sprites
    for (auto& p : projList) {
        if (!p.active) continue;
        ColorF col;
        switch (p.type) {
            case ProjectileType::Disc:    col = {1.0f, 0.6f, 0.1f, 1.0f}; break;
            case ProjectileType::Bolt:    col = {0.2f, 0.8f, 1.0f, 1.0f}; break;
            case ProjectileType::Grenade:
            case ProjectileType::Mortar:  col = {0.3f, 1.0f, 0.3f, 1.0f}; break;
            default:                      col = {1,1,1,1};    break;
        }
        r.drawSprite(p.pos, 0.3f, col);
    }

    // Render explosion boxes (legacy, still used for visual feedback)
    for (auto& e : explosions) {
        float t = e.lifetime / e.maxLifetime;
        float size = e.radius * (1.0f + (1.0f - t) * 2.0f);
        ColorF col = {e.color.r, e.color.g, e.color.b, t * 0.3f};
        r.drawSprite(e.pos, size * 0.5f, col);
    }

    // Render particles
    renderParticles();

    // Render sky
    skyBox.render(r.view, r.projection);

    // Simple water plane for procedural terrain (no mission loaded)
    if (!loaded || terrainBlock.loaded) {
        float waterLevel = 0.0f;
        float time = Engine::instance().game().gameTime();
        float waveOffset = sinf(time * 0.5f) * 0.15f;
        int gridRes = 16;
        float size = 2048.0f;
        float step = size / gridRes;
        Point3F cam = r.cameraPos;
        // Only render water within reasonable distance
        if (cam.y > waterLevel - 20.0f && cam.y < waterLevel + 50.0f) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            for (int z = 0; z < gridRes; z++) {
                for (int x = 0; x < gridRes; x++) {
                    float wx = -size/2 + x * step;
                    float wz = -size/2 + z * step;
                    // Only draw quads near camera
                    float dist = (wx - cam.x) * (wx - cam.x) + (wz - cam.z) * (wz - cam.z);
                    if (dist > 600.0f * 600.0f) continue;
                    float wy = waterLevel + waveOffset + sinf(wx * 0.01f + time) * 0.1f;
                    Box3F quad = {{wx, wy - 0.1f, wz}, {wx + step, wy + 0.1f, wz + step}};
                    r.drawBox(quad, {0.1f, 0.3f, 0.6f, 0.5f});
                }
            }
            glDisable(GL_BLEND);
        }
    }

    // Render bots
    auto* shader = ShaderManager::getDefaultShader();
    if (shader) shader->bind();
    for (auto& b : bots) {
        if (!b.alive) continue;
        if (!b.shape) {
            // Load shape on first render
            auto& fs = Engine::instance().fs();
            for (auto* p : {"shapes/bioderm_light.glb", "shapes/bioderm_light.dts", "shapes/light_male.glb"}) {
                auto d = fs.read(p);
                if (!d.empty()) {
                    DTSShape* s = new DTSShape;
                    s->name = "bot";
                    if (s->load(d.data(), d.size())) {
                        b.shape = s;
                        break;
                    }
                    delete s;
                }
            }
        }
        if (b.shape && b.shape->loaded) {
            MatrixF model;
            Point3F ax = {0, 1, 0};
            model.setRotationAxis(ax, b.moveYaw);
            model.setTranslation(b.pos);
            Engine::instance().renderer().setModel(model * Math::czUpToYUp());
            shader->setUniform("uUseTexture", (int32_t)0);
            shader->setUniform("uUseLightmap", (int32_t)0);
            b.shape->render(0);
        } else {
            // Fallback: colored box
            float hs = 0.8f;
            Box3F box = {{b.pos.x - hs, b.pos.y - 1.0f, b.pos.z - hs},
                         {b.pos.x + hs, b.pos.y + 1.0f, b.pos.z + hs}};
            ColorF col = b.health > 50 ? ColorF{0, 0.6f, 0, 1} : ColorF{0.8f, 0.2f, 0, 1};
            Engine::instance().renderer().drawBox(box, col);
        }
        // Health bar above bot
        {
            auto* font = Engine::instance().renderer().getFont();
            if (font) {
                Point3F above = {b.pos.x, b.pos.y + 2.5f, b.pos.z};
                Point3F screen = worldToScreen(above, Engine::instance().renderer().viewMatrix(),
                    Engine::instance().renderer().projectionMatrix(), 1024, 768);
                if (screen.z > 0 && screen.x >= 0 && screen.x <= 1024 && screen.y >= 0 && screen.y <= 768) {
                    float barW = 40, barH = 5;
                    float by = screen.y - 15;
                    Engine::instance().renderer().drawBox({{screen.x - barW/2 - 1, by - 1, 0},
                        {screen.x + barW/2 + 1, by + barH + 1, 0}}, {0,0,0,0.5f});
                    float hp = std::max(0.0f, b.health / 100.0f);
                    ColorF hc = hp > 0.5f ? ColorF{0,1,0,0.9f} : hp > 0.25f ? ColorF{1,1,0,0.9f} : ColorF{1,0,0,0.9f};
                    Engine::instance().renderer().drawBox({{screen.x - barW/2, by, 0},
                        {screen.x - barW/2 + barW * hp, by + barH, 0}}, hc);
                }
            }
        }
    }
}

void World::spawnBots(int count) {
    bots.clear();
    for (int i = 0; i < count; i++) {
        Bot b;
        b.startPos = {20.0f + (i % 5) * 15.0f, 5.0f, 20.0f + (i / 5) * 15.0f};
        b.pos = b.startPos;
        b.health = 100.0f;
        b.patrolOffset = i * 0.5f;
        b.moveYaw = 0;
        bots.push_back(b);
    }
    Console::instance().printf(LogLevel::Info, "Spawned %d bots", count);
}

void World::addObject(const WorldObject& obj) {
    worldObjects.push_back(obj);
}

void World::spawnProjectile(const Projectile& p) {
    projList.push_back(p);
}

float World::getHeight(float x, float z) const {
    // First check interior collision (buildings, etc.)
    if (interiorCollision.loaded) {
        float interiorH = interiorCollision.getHeight(x, z);
        if (interiorH > -1e9f) return interiorH;
    }

    // Fall back to terrain height
    if (!terrainBlock.loaded || terrainBlock.heights.empty()) return 0;

    return terrainBlock.sampleHeight(x, z);
}

// ─── Particle System ──────────────────────────────────────────

void World::spawnExplosion(const Point3F& pos, const ColorF& color, float radius, int count) {
    for (int i = 0; i < count; i++) {
        Particle p;
        float theta = ((float)std::rand() / RAND_MAX) * 3.14159f * 2.0f;
        float phi = ((float)std::rand() / RAND_MAX) * 3.14159f;
        float speed = ((float)std::rand() / RAND_MAX) * radius * 4.0f;
        p.pos = pos;
        p.vel = {sinf(phi) * cosf(theta) * speed, fabsf(cosf(phi)) * speed * 0.8f, sinf(phi) * sinf(theta) * speed};
        p.lifetime = 0.3f + ((float)std::rand() / RAND_MAX) * 0.6f;
        p.maxLifetime = p.lifetime;
        p.size = 0.3f + ((float)std::rand() / RAND_MAX) * 0.5f;
        p.color = color;
        p.active = true;
        // Fade alpha over lifetime
        if (particles.size() < 1000) particles.push_back(p);
    }
}

void World::spawnTrail(const Point3F& pos, const ColorF& color, float size) {
    Particle p;
    p.pos = pos;
    p.vel = {0, 0, 0};
    p.lifetime = 0.3f;
    p.maxLifetime = 0.3f;
    p.size = size;
    p.color = color;
    p.color.a = 0.5f;
    p.active = true;
    if (particles.size() < 1000) particles.push_back(p);
}

void World::updateParticles(float dt) {
    for (auto& p : particles) {
        if (!p.active) continue;
        p.lifetime -= dt;
        if (p.lifetime <= 0) { p.active = false; continue; }
        p.vel.y -= 5.0f * dt; // gravity
        p.pos.x += p.vel.x * dt;
        p.pos.y += p.vel.y * dt;
        p.pos.z += p.vel.z * dt;
        p.size += dt * 0.5f; // expand
        float t = p.lifetime / p.maxLifetime;
        p.color.a = t; // fade out
    }
    // Remove dead particles
    particles.erase(std::remove_if(particles.begin(), particles.end(),
        [](const Particle& p) { return !p.active; }), particles.end());
}

void World::renderParticles() {
    auto& r = Engine::instance().renderer();
    for (auto& p : particles) {
        if (!p.active) continue;
        r.drawSprite(p.pos, p.size, p.color);
    }
}

Game::Game() : pl(new Player), w(new World) {
    mMenu = new Menu;
    hud = new HUD;
}
Game::~Game() { delete pl; delete w; delete hud; }

bool Game::init() {
    Console::instance().printf(LogLevel::Info, "Game initialized");

    // Register game console commands
    auto& con = Console::instance();
    con.addCommand("startLocal", [this](int32_t argc, const char* const* argv) {
        startLocalGame();
    });

    con.addCommand("connect", [this](int32_t argc, const char* const* argv) {
        if (argc > 1) connectToServer(argv[1], argc > 2 ? (uint16_t)atoi(argv[2]) : 28000);
    });

    con.addCommand("startServer", [this](int32_t argc, const char* const* argv) {
        uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 28000;
        if (argc > 2) {
            Console::instance().setVariable("sv_mission", argv[2]);
        }
        // Wire terrain height callback for server-side collision
        server.setHeightCallback(+[](float x, float z, void* ctx) -> float {
            return static_cast<World*>(ctx)->getHeight(x, z);
        }, &w);
        server.start(port);
    }, "startServer [port] [mission] - Start a game server on the given port");

    con.addCommand("CreateServer", [this](int32_t argc, const char* const* argv) {
        // T2 script compatibility: CreateServer <mission> <type>
        const char* mission = (argc > 1) ? argv[1] : "test";
        startLocalGame(mission);
    }, "CreateServer <mission> [type] - Start a local server with mission");

    con.addCommand("loadMission", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) return;
        startLocalGame(argv[1]);
    }, "loadMission <name> - Load and start a mission");

    con.addCommand("startMission", [this](int32_t, const char* const*) {
        if (gameState == Loading || gameState == Playing) return;
        startLocalGame();
    }, "startMission - Start the selected mission");

    con.addCommand("playdemo", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) { Console::instance().printf(LogLevel::Warn, "Usage: playdemo <path>"); return; }
        playDemo(argv[1]);
    }, "playdemo <path> - Load and play a Tribes 2 demo file");

    con.addCommand("listdemos", [this](int32_t argc, const char* const* argv) {
        auto& fs = Engine::instance().fs();
        std::vector<std::string> files;
        fs.listFiles("*.demo", files);
        if (files.empty()) {
            Console::instance().printf(LogLevel::Info, "No demo files found (*.demo)");
        } else {
            Console::instance().printf(LogLevel::Info, "Demo files (%zu):", files.size());
            for (auto& f : files)
                Console::instance().printf(LogLevel::Info, "  %s", f.c_str());
        }
    }, "listdemos - List available demo files");

    con.addCommand("testshape", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) { Console::instance().printf(LogLevel::Warn, "Usage: testshape <glb_path>"); return; }
        auto& fs = Engine::instance().fs();
        auto data = fs.read(argv[1]);
        if (data.empty()) {
            Console::instance().printf(LogLevel::Warn, "testshape: file not found: %s", argv[1]);
            return;
        }
        testShape = DTSShape{};
        testShape.name = argv[1];
        if (!testShape.load(data.data(), data.size())) {
            Console::instance().printf(LogLevel::Warn, "testshape: failed to load shape");
            testShape = DTSShape{};
            return;
        }
        testShapeLoaded = true;
        Console::instance().printf(LogLevel::Info, "testshape: loaded '%s' (%zu meshes, %zu nodes, %zu anims)",
            argv[1], testShape.meshes.size(), testShape.nodes.size(), testShape.animations.size());
        if (!testShape.animations.empty()) {
            Console::instance().printf(LogLevel::Info, "  animations:");
            for (auto& a : testShape.animations)
                Console::instance().printf(LogLevel::Info, "    %s (%.1fs)", a.name.c_str(), a.duration);
        }
    }, "testshape <path> - Load and display a DTS/GLB shape");

    // ── Shape Viewer ─────────────────────────────────────────────────────
    con.addCommand("shapeviewer", [this](int32_t, const char* const*) {
        enterShapeViewer();
    }, "shapeviewer - Browse all .dts shapes from the data paths");

    con.addCommand("sv_next", [this](int32_t, const char* const*) {
        if (!shapeViewerActive) { Console::instance().printf(LogLevel::Warn, "shapeviewer not active"); return; }
        shapeViewerNext();
    }, "sv_next - Next shape in shape viewer");

    con.addCommand("sv_prev", [this](int32_t, const char* const*) {
        if (!shapeViewerActive) { Console::instance().printf(LogLevel::Warn, "shapeviewer not active"); return; }
        shapeViewerPrev();
    }, "sv_prev - Previous shape in shape viewer");

    con.addCommand("sv_ghosts", [this](int32_t, const char* const*) {
        Console::instance().printf(LogLevel::Info, "Total server ghosts: %zu", server.ghostCount());
    }, "sv_ghosts - List server ghost count");

    con.addCommand("sv_spawn", [this](int32_t argc, const char* const* argv) {
        if (argc < 4) { Console::instance().printf(LogLevel::Warn, "Usage: sv_spawn <classId> <x> <y> [z]"); return; }
        int classId = atoi(argv[1]);
        float x = (float)atof(argv[2]);
        float y = (float)atof(argv[3]);
        float z = (argc > 4) ? (float)atof(argv[4]) : 2.0f;
        uint32_t idx = server.spawnGhost(classId, x, y, z);
        if (idx > 0)
            Console::instance().printf(LogLevel::Info, "Spawned ghost idx=%u class=%d at (%.1f, %.1f, %.1f)",
                (unsigned)idx, classId, x, y, z);
    }, "sv_spawn <classId> <x> <y> [z] - Spawn a ghost on the server");

    con.addCommand("sv_removeghost", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) { Console::instance().printf(LogLevel::Warn, "Usage: sv_removeghost <index>"); return; }
        uint32_t idx = (uint32_t)atoi(argv[1]);
        if (server.removeGhost(idx))
            Console::instance().printf(LogLevel::Info, "Removed ghost idx=%u", (unsigned)idx);
        else
            Console::instance().printf(LogLevel::Warn, "Ghost idx=%u not found", (unsigned)idx);
    }, "sv_removeghost <index> - Remove a ghost on the server");

    con.addCommand("sv_addbot", [this](int32_t, const char* const*) {
        server.spawnBot();
        Console::instance().printf(LogLevel::Info, "Bot spawned");
    }, "sv_addbot - Spawn an AI bot on the server");

    con.addCommand("kick", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) { Console::instance().printf(LogLevel::Warn, "Usage: kick <clientId>"); return; }
        server.kickClient(atoi(argv[1]));
    }, "kick <clientId> - Kick a client by index");

    con.addCommand("ban", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) { Console::instance().printf(LogLevel::Warn, "Usage: ban <clientId>"); return; }
        server.banClient(atoi(argv[1]));
    }, "ban <clientId> - Ban a client by IP");

    con.addCommand("unbanall", [this](int32_t, const char* const*) {
        server.clearBans();
    }, "unbanall - Clear the ban list");

    con.addCommand("sv_map", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) { Console::instance().printf(LogLevel::Warn, "Usage: sv_map <mission>"); return; }
        server.changeMap(argv[1]);
    }, "sv_map <mission> - Change the current mission");

    con.addCommand("sv_gamemode", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) { Console::instance().printf(LogLevel::Warn, "Usage: sv_gamemode <0|1> (0=DM, 1=TDM)"); return; }
        server.setGameMode(atoi(argv[1]));
    }, "sv_gamemode <0|1> - Set game mode (0=Deathmatch, 1=Team Deathmatch)");

    con.addCommand("sv_nat", [this](int32_t, const char* const*) {
        Console::instance().printf(LogLevel::Info, "NAT relay: see server console");
    }, "sv_nat - Show NAT relay info");

    con.addCommand("record", [this](int32_t argc, const char* const* argv) {
        if (argc < 2) { Console::instance().printf(LogLevel::Warn, "Usage: record <path>"); return; }
        server.startRecording(argv[1]);
    }, "record <path> - Start recording server state to file");

    con.addCommand("stoprecord", [this](int32_t, const char* const*) {
        server.stopRecording();
    }, "stoprecord - Stop recording");

    return true;
}

void Game::shutdown() {
    delete mMenu;
    mMenu = nullptr;
    auto& audio = Engine::instance().audio();
    if (ambientSource) audio.releaseSource(ambientSource);
    ambientSource = nullptr;
    ambientSound = nullptr;
}

// ─── AudioProfile scanner ────────────────────────────────────
// Scans game scripts for AudioProfile definitions and builds a
// profile-ID → sound-file-path mapping for demo playback.
struct AudioProfileEntry {
    std::string profileName;
    std::string filename;
};
static std::vector<AudioProfileEntry> s_audioProfiles;
static void scanAudioProfiles() {
    if (!s_audioProfiles.empty()) return;
    auto& fs = Engine::instance().fs();
    std::vector<std::string> scriptFiles;
    fs.listFiles("scripts/", scriptFiles);
    std::set<std::string> seen;
    for (auto& f : scriptFiles) {
        if (f.size() < 3 || f.substr(f.size() - 3) != ".cs") continue;
        if (f.find(".dso") != std::string::npos) continue;
        if (!seen.insert(f).second) continue;
        auto data = fs.readText(f.c_str());
        if (data.empty()) continue;
        const char* p = data.c_str();
        while (true) {
            const char* db = strstr(p, "datablock AudioProfile(");
            if (!db) break;
            p = db + 23;
            const char* np = strchr(p, ')');
            if (!np) break;
            std::string name(p, np - p);
            const char* ob = strchr(np, '{');
            if (!ob) break;
            const char* cb = strchr(ob, '}');
            if (!cb) break;
            std::string body(ob, cb - ob);
            const char* fk = body.c_str();
            const char* fnq = nullptr;
            while ((fk = strstr(fk, "filename"))) {
                const char* eq = strchr(fk, '=');
                if (!eq) { fk += 8; continue; }
                fnq = strchr(eq, '"');
                if (fnq) break;
                fk += 8;
            }
            if (fnq) {
                const char* fnq2 = strchr(fnq + 1, '"');
                if (fnq2) {
                    std::string fn(fnq + 1, fnq2 - fnq - 1);
                    if (!fn.empty()) s_audioProfiles.push_back({name, "audio/" + fn});
                }
            }
            p = cb + 1;
        }
    }
    Console::instance().printf(LogLevel::Info, "Audio: scanned %zu AudioProfiles from %zu scripts",
        s_audioProfiles.size(), seen.size());
}

void Game::update(float dt) {
    time += dt;

    if (gameState == Playing) {
        // ─── Demo playback ──────────────────────────────────────
        if (demoPlaying) {
            if (demoPaused && !demoStepRequest) {
                demoJetHeld = false;
                return;
            }
            demoTime += dt;
            // Decay camera shake
            if (shakeIntensity > 0) {
                shakeIntensity = std::max(0.0f, shakeIntensity - dt * 8.0f);
                shakeOffset = {
                    (float)(rand() % 1000) / 500.0f - 1.0f,
                    (float)(rand() % 1000) / 500.0f - 1.0f,
                    (float)(rand() % 1000) / 500.0f - 1.0f,
                };
                shakeOffset.x *= shakeIntensity;
                shakeOffset.y *= shakeIntensity;
                shakeOffset.z *= shakeIntensity;
            } else {
                shakeOffset = {0,0,0};
            }
            int blocksThisFrame;
            if (demoStepRequest) {
                blocksThisFrame = 1;
                demoStepRequest = false;
            } else if (demoFastForward || currentInput.jet) {
                demoJetHeld = currentInput.jet;
                blocksThisFrame = (int)(demoBlocksTotal * dt / (demoTotalTime > 0 ? demoTotalTime : 1.0f));
            } else {
                demoJetHeld = false;
                // Match real-time: catch up to target position
                int targetDone = (int)(demoTime / (demoTotalTime > 0 ? demoTotalTime : 1.0f) * demoBlocksTotal);
                blocksThisFrame = targetDone - demoBlocksDone;
            }
            if (blocksThisFrame < 1) blocksThisFrame = 1;
            if (blocksThisFrame > 500) blocksThisFrame = 500;

            for (int i = 0; i < blocksThisFrame; i++) {
                DemoBlock* block = demoParser->nextBlock();
                if (!block) {
                    Console::instance().printf(LogLevel::Info,
                        "Demo playback complete: %d blocks in %.1f seconds",
                        demoBlocksDone, demoTime);
                    demoPlaying = false;
                    delete demoParser; demoParser = nullptr;
                    setState(MenuScreen);
                    return;
                }
                demoBlocksDone++;
                demoParser->setCurrentBlock(demoBlocksDone - 1);

                // Extract position data from move blocks
                if (block->type == T2Demo::BlockTypeMove && block->size >= 64) {
                    DemoMove move = demoParser->readRawMove(block->data.data(), block->data.size());
                    demoPrevCameraPos = demoCameraPos;
                    demoPrevCameraTarget = demoCameraTarget;
                    demoCameraPos = {move.x, move.y, move.z};
                    demoCameraTarget = {
                        move.x + std::sin(move.yaw) * std::cos(move.pitch),
                        move.y + std::sin(move.pitch),
                        move.z + std::cos(move.yaw) * std::cos(move.pitch)
                    };
                    demoMoveBlend = 0.0f;
                    demoHasPos = true;
                    demoPath.push_back({move.x, move.y, move.z});
                    demoPathCount = (int)demoPath.size();
                    // Move the player to demo position so physics/collision use it
                    pl->setPosition(demoCameraPos);
                    pl->setRotation({move.pitch, 0, move.yaw});
                }

                // Parse packet blocks (GameState, ghost updates, events)
                if (block->type == T2Demo::BlockTypePacket ||
                    block->type == T2Demo::BlockTypeSendPacket) {
                    PacketData pd = demoParser->parsePacket(block->data.data(), block->data.size(), demoBlocksDone - 1);
                    // Collect chat/server events for the event pane
                    for (const auto& ev : pd.events) {
                        // Handle audio events
                        if (ev.audioProfileId >= 0) {
                            auto& audio = Engine::instance().audio();
                            if (audio.config().enabled && audio.config().sfxVolume > 0) {
                                scanAudioProfiles();
                                if (ev.audioProfileId < (int)s_audioProfiles.size()) {
                                    const auto& entry = s_audioProfiles[ev.audioProfileId];
                                    auto* buf = audio.loadSound(entry.filename.c_str());
                                    if (buf) {
                                        auto* src = audio.createSource();
                                        if (src) {
                                            src->setVolume(0.3f);
                                            src->play(buf);
                                        }
                                    }
                                }
                            }
                            continue;
                        }
                        if (ev.message.empty()) continue;
                        DemoTimedEvent te;
                        te.time = demoTime;
                        te.text = ev.message;
                        te.ghostIndex = -1;
                        if (ev.classId == T2Demo::NetEventClassFirst + 22) {
                            te.type = 0; // chat message
                        } else if (ev.classId == T2Demo::NetEventClassFirst + 9) {
                            te.type = 1; // server command
                        } else {
                            te.type = 2; // system
                        }
                        // Try to find source player ghost for chat messages
                        if (te.type == 0 && !pd.ghosts.empty()) {
                            te.ghostIndex = pd.ghosts[0].index;
                        }
                        demoEventLog.push_back(te);
                    }
                    // Update compression point from GameState
                    if (pd.gameState.controlObjectDirty) {
                        // Full control object update with new ghost index
                        // (position comes from move blocks, not GameState)
                    } else if (pd.gameState.compressionPoint.x != 0 ||
                               pd.gameState.compressionPoint.y != 0 ||
                               pd.gameState.compressionPoint.z != 0) {
                         // Update compression point from partial control update
                         const Vec3& cp = pd.gameState.compressionPoint;
                         if (!demoHasPos) {
                             demoCameraPos = {cp.x, cp.y, cp.z};
                             demoCameraTarget = {cp.x, cp.y + 2.0f, cp.z};
                             demoHasPos = true;
                         }
                     }
                    // Store damage flash and whiteout for screen effects
                    damageFlash = pd.gameState.damageFlash;
                    whiteOut = pd.gameState.whiteOut;
                    // Camera shake on damage
                    if (pd.gameState.damageFlash > 0.5f)
                        shakeIntensity = std::max(shakeIntensity, pd.gameState.damageFlash * 3.0f);
                    // Store control object ghost index for highlight
                    if (pd.gameState.controlObjectGhostIndex >= 0)
                        controlGhostIndex = pd.gameState.controlObjectGhostIndex;
                }
                delete block;
            }

            // Debug: ghost stats every ~500 blocks
            if (demoPlaying && demoParser && (demoBlocksDone % 500) == 0 && demoBlocksDone > 0) {
                const GhostTracker& gt = demoParser->getGhostTracker();
                Console::instance().printf(LogLevel::Debug, "Blocks: %d, Ghosts: %d", demoBlocksDone, gt.size());
                if (gt.size() > 0) {
                    int withPos = 0;
                    for (int i : gt.getAllIndices()) {
                        auto* g = gt.getGhost(i);
                        if (g) {
                            if (g->position.x != 0 || g->position.y != 0 || g->position.z != 0) {
                                withPos++;
                                Console::instance().printf(LogLevel::Debug, "  HAS POS: Ghost[%d] class=%d '%s' pos=(%.1f %.1f %.1f)",
                                    i, g->classId, g->className.c_str(), g->position.x, g->position.y, g->position.z);
                            }
                        }
                    }
                    Console::instance().printf(LogLevel::Debug, "Ghosts: %d total, %d with pos", gt.size(), withPos);
                }
            }

            // Update window title with progress
            int pct = demoBlocksTotal > 0 ? (demoBlocksDone * 100 / demoBlocksTotal) : 0;
            char title[128];
            snprintf(title, sizeof(title), "Torch - Demo [%d%%] %d/%d blocks %d packets",
                pct, demoBlocksDone, demoBlocksTotal, demoPacketsParsed);
            Engine::instance().platform().setTitle(title);

            // Advance interpolation blend (smooth over ~150ms)
            if (demoMoveBlend < 1.0f) {
                demoMoveBlend = std::min(demoMoveBlend + dt * 6.0f, 1.0f);
            }

            // Free camera toggle during demos (F1)
            static bool prevDemoFreeCam = false;
            if (currentInput.freeCam && !prevDemoFreeCam) {
                freeCamActive = !freeCamActive;
                if (freeCamActive) {
                    freeCamPos = demoHasPos ? demoCameraPos : pl->cameraPos();
                    freeCamTarget = demoHasPos ? demoCameraTarget : pl->cameraTarget();
                }
            }
            prevDemoFreeCam = currentInput.freeCam;
            if (freeCamActive) {
                float camSpeed = 50.0f * dt;
                float yaw = freeCamRot.z;
                float pitch = freeCamRot.x;
                yaw += currentInput.lookDelta.y;
                pitch -= currentInput.lookDelta.x;
                if (pitch > 1.5f) pitch = 1.5f;
                if (pitch < -1.5f) pitch = -1.5f;
                freeCamRot = {pitch, 0, yaw};
                Point3F fwd = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
                Point3F right = {std::cos(yaw), 0, -std::sin(yaw)};
                if (currentInput.forward) { freeCamPos.x += fwd.x * camSpeed; freeCamPos.y += fwd.y * camSpeed; freeCamPos.z += fwd.z * camSpeed; }
                if (currentInput.backward) { freeCamPos.x -= fwd.x * camSpeed; freeCamPos.y -= fwd.y * camSpeed; freeCamPos.z -= fwd.z * camSpeed; }
                if (currentInput.left) { freeCamPos.x -= right.x * camSpeed; freeCamPos.z -= right.z * camSpeed; }
                if (currentInput.right) { freeCamPos.x += right.x * camSpeed; freeCamPos.z += right.z * camSpeed; }
                if (currentInput.jump) freeCamPos.y += camSpeed;
                if (currentInput.jet) freeCamPos.y -= camSpeed;
                freeCamTarget = {freeCamPos.x + fwd.x, freeCamPos.y + fwd.y, freeCamPos.z + fwd.z};
            } else if (demoOrbitCam) {
                float orbitSpeed = 60.0f * dt;
                bool orbitInput = currentInput.left || currentInput.right;
                if (currentInput.left) orbitAngle -= orbitSpeed;
                else if (currentInput.right) orbitAngle += orbitSpeed;
                // Auto-rotate when no input (gentle spin to show the scene)
                if (!orbitInput) orbitAngle += dt * 8.0f;
                if (currentInput.forward) orbitDistance = std::max(20.0f, orbitDistance - 50.0f * dt);
                if (currentInput.backward) orbitDistance = std::min(2000.0f, orbitDistance + 50.0f * dt);
                if (currentInput.jump) orbitHeight = std::min(500.0f, orbitHeight + 30.0f * dt);
                if (currentInput.jet) orbitHeight = std::max(20.0f, orbitHeight - 30.0f * dt);
            }

            return; // skip normal game logic during demo playback
        }

        // F1 toggle for free camera (edge-triggered)
        static bool prevFreeCam = false;
        if (currentInput.freeCam && !prevFreeCam) {
            freeCamActive = !freeCamActive;
            if (freeCamActive) {
                freeCamPos = pl->cameraPos();
                freeCamTarget = pl->cameraTarget();
                freeCamRot = pl->rotation();
            }
        }
        prevFreeCam = currentInput.freeCam;

        // F2 toggle for orbit camera
        static bool prevF2 = false;
        if (currentInput.orbitCam && !prevF2) {
            demoOrbitCam = !demoOrbitCam;
            if (freeCamActive) { freeCamActive = false; }
        }
        prevF2 = currentInput.orbitCam;

        // F3 toggle for editor mode
        auto& plat = Engine::instance().platform();
        static bool prevF3 = false;
        bool f3Down = plat.input().keysDown[SCANCODE_F3];
        if (f3Down && !prevF3) {
            editorActive = !editorActive;
            if (editorActive) {
                freeCamActive = true;
                Console::instance().printf(LogLevel::Info, "Editor mode %s", editorActive ? "ON" : "OFF");
            }
        }
        prevF3 = f3Down;

        // Editor: place ghost on left click, cycle class on scroll
        if (editorActive && freeCamActive) {
            static bool prevClick = false;
            bool click = plat.input().mouseButtons[0];
            if (click && !prevClick) {
                // Place a ghost at the camera's target position
                float dist = 20.0f;
                Point3F dir = {freeCamTarget.x - freeCamPos.x, freeCamTarget.y - freeCamPos.y, freeCamTarget.z - freeCamPos.z};
                float len = sqrtf(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
                if (len > 0.001f) { dir.x /= len; dir.y /= len; dir.z /= len; }
                Point3F placePos = {freeCamPos.x + dir.x * dist, freeCamPos.y + dir.y * dist, freeCamPos.z + dir.z * dist};
                server.spawnGhost(editorPlaceClass, placePos.x, placePos.y, placePos.z);
                Console::instance().printf(LogLevel::Info, "Placed ghost class=%d at (%.1f,%.1f,%.1f)",
                    editorPlaceClass, placePos.x, placePos.y, placePos.z);
            }
            prevClick = click;
            // Right click to remove nearest ghost
            static bool prevRight = false;
            bool right = plat.input().mouseButtons[1];
            if (right && !prevRight) {
                // Remove last spawned ghost
                if (server.ghostCount() > 0) {
                    // Simple approach: remove ghosts in reverse order via command
                    Console::instance().printf(LogLevel::Info, "Right-click: remove ghost. Use 'sv_removeghost <idx>'");
                }
            }
            prevRight = right;
            // Scroll wheel to cycle class
            static float prevScroll = 0;
            float scroll = plat.input().mouseWheel;
            if (scroll != prevScroll) {
                int delta = (scroll > prevScroll) ? 1 : -1;
                editorPlaceClass += delta;
                if (editorPlaceClass < 0) editorPlaceClass = 62;
                if (editorPlaceClass > 62) editorPlaceClass = 0;
                Console::instance().printf(LogLevel::Info, "Editor: placing class %d", editorPlaceClass);
            }
            prevScroll = scroll;
        }

        if (freeCamActive) {
            // Move free camera using WASD + mouse
            float camSpeed = 50.0f * dt;
            float yaw = freeCamRot.z;
            float pitch = freeCamRot.x;

            // Mouse look
            yaw += currentInput.lookDelta.y;
            pitch -= currentInput.lookDelta.x;
            if (pitch > 1.5f) pitch = 1.5f;
            if (pitch < -1.5f) pitch = -1.5f;
            freeCamRot = {pitch, 0, yaw};

            // Direction vectors
            Point3F fwd = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
            Point3F right = {std::cos(yaw), 0, -std::sin(yaw)};

            if (currentInput.forward) { freeCamPos.x += fwd.x * camSpeed; freeCamPos.y += fwd.y * camSpeed; freeCamPos.z += fwd.z * camSpeed; }
            if (currentInput.backward) { freeCamPos.x -= fwd.x * camSpeed; freeCamPos.y -= fwd.y * camSpeed; freeCamPos.z -= fwd.z * camSpeed; }
            if (currentInput.left) { freeCamPos.x -= right.x * camSpeed; freeCamPos.z -= right.z * camSpeed; }
            if (currentInput.right) { freeCamPos.x += right.x * camSpeed; freeCamPos.z += right.z * camSpeed; }
            if (currentInput.jump) freeCamPos.y += camSpeed;
            if (currentInput.jet) freeCamPos.y -= camSpeed;

            freeCamTarget = {freeCamPos.x + fwd.x, freeCamPos.y + fwd.y, freeCamPos.z + fwd.z};
        } else {
            // Update physics
            Physics physics;
            physics.update(pl, dt, currentInput);

            // Update player animation state
            pl->updateAnimation(dt, currentInput.jet);

            // Update weapon timers
            for (int i = 0; i < pl->weaponCount(); i++) {
                const_cast<Weapon&>(pl->weapon(i)).updateTimers(dt);
            }

            // Fire weapon
            if (currentInput.fire) {
                pl->fireWeapon(false);
            }
            if (currentInput.altFire) {
                pl->fireWeapon(true);
            }

            // ─── Chat input ──────────────────────────────────────
            if (cfg.online && activeConn && activeConn->state() >= Connection::Connected) {
                static bool chatActive = false;
                static std::string chatBuf;
                auto& plat = Engine::instance().platform();
                bool enterDown = plat.input().keysDown[SCANCODE_RETURN];
                bool escDown = plat.input().keysDown[SCANCODE_ESCAPE];
                if (!chatActive) {
                    static bool prevEnter = false;
                    if (enterDown && !prevEnter) {
                        chatActive = true;
                        chatBuf.clear();
                        plat.startTextInput();
                        plat.setRelativeMouse(false);
                        plat.showMouse(true);
                    }
                    prevEnter = enterDown;
                } else {
                    const std::string& ti = plat.input().textInput;
                    for (char c : ti) {
                        if (c >= 0x20 && c <= 0x7e && chatBuf.size() < 200)
                            chatBuf += c;
                    }
                    static bool prevEnter = false;
                    if (enterDown && !prevEnter && !chatBuf.empty()) {
                        // Send chat message
                        T2Protocol::ChatMessage chat;
                        snprintf(chat.sender, sizeof(chat.sender), "Player");
                        snprintf(chat.text, sizeof(chat.text), "%s", chatBuf.c_str());
                        uint8_t buf[512];
                        size_t len = T2Protocol::encodeChat(buf, sizeof(buf), chat);
                        if (len > 0) activeConn->sendGamePacket(buf, len, false);
                        chatBuf.clear();
                        chatActive = false;
                        plat.stopTextInput();
                        plat.setRelativeMouse(true);
                        plat.showMouse(false);
                    }
                    prevEnter = enterDown;
                    static bool prevEsc = false;
                    if (escDown && !prevEsc) {
                        chatActive = false;
                        chatBuf.clear();
                        plat.stopTextInput();
                        plat.setRelativeMouse(true);
                        plat.showMouse(false);
                    }
                    prevEsc = escDown;
                    // Backspace
                    static bool prevBS = false;
                    if (plat.input().keysDown[SCANCODE_BACKSPACE] && !prevBS && !chatBuf.empty())
                        chatBuf.pop_back();
                    prevBS = plat.input().keysDown[SCANCODE_BACKSPACE];
                    // Render chat buffer as overlay text
                    if (!chatBuf.empty()) {
                        hud->showMessage(chatBuf.c_str(), ColorF{1,1,1,1});
                    }
                }
            }

            // Client-side prediction: store move and send to server
            if (cfg.online && activeConn && activeConn->state() >= Connection::Connected) {
                uint32_t thisSeq = ++moveSeq;
                // Store input for later reconciliation
                pendingMoves.push_back({thisSeq, currentInput, dt});
                if (pendingMoves.size() > 128)
                    pendingMoves.pop_front();

                T2Protocol::MoveMessage moveMsg;
                Point3F ppos = pl->position();
                Point3F prot = pl->rotation();
                moveMsg.posX = ppos.x;
                moveMsg.posY = ppos.y;
                moveMsg.posZ = ppos.z;
                moveMsg.rotZ = prot.z;
                moveMsg.rotX = prot.x;
                moveMsg.flags = (currentInput.forward ? 1 : 0) |
                                (currentInput.jump ? 2 : 0) |
                                (currentInput.jet ? 4 : 0) |
                                (currentInput.fire ? 8 : 0) |
                                (currentInput.reload ? 16 : 0) |
                                (currentInput.left ? 32 : 0) |
                                (currentInput.right ? 64 : 0);
                moveMsg.lookX = currentInput.lookDelta.x;
                moveMsg.lookY = currentInput.lookDelta.y;
                moveMsg.seq = thisSeq;

                uint8_t buf[64];
                size_t moveLen = T2Protocol::encodeMove(buf, sizeof(buf), moveMsg);
                if (moveLen > 0) activeConn->sendGamePacket(buf, (int)moveLen, false);
            }

            // Reload
            if (currentInput.reload) {
                int32_t cw = pl->currentWeapon();
                if (cw >= 0 && cw < pl->weaponCount()) {
                    const_cast<Weapon&>(pl->weapon(cw)).reloading = true;
                    const_cast<Weapon&>(pl->weapon(cw)).reloadTimer = gWeaponTable[pl->weapon(cw).type].reloadTime;
                }
            }
        }

        // Death check (deaths tracked in Player::applyDamage)
        if (pl->health() <= 0 && gameState == Playing) {
            setState(Dead);
        }

        // Update world (projectiles, etc.)
        w->update(dt);

        // Update audio listener from camera
        auto& audio = Engine::instance().audio();
        if (audio.config().enabled) {
            Point3F camPos = freeCamActive ? freeCamPos : pl->cameraPos();
            Point3F camTarget = freeCamActive ? freeCamTarget : pl->cameraTarget();
            Point3F forward = {camTarget.x - camPos.x, camTarget.y - camPos.y, camTarget.z - camPos.z};
            float flen = std::sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
            if (flen > 0.0001f) { forward.x /= flen; forward.y /= flen; forward.z /= flen; }
            Point3F up = {0, 1, 0};
            audio.update(camPos, pl->velocity(), forward, up);
        }
    } else if (gameState == Dead) {
        // Spectator mode when online
        if (cfg.online && activeConn && activeConn->isConnected()) {
            if (!liveSpectateInit) {
                liveSpectateInit = true;
                freeCamActive = false;
                // Find first live ghost to spectate
                auto indices = liveGhosts.getAllIndices();
                if (!indices.empty()) {
                    spectateGhostIndex = indices[0];
                    // If it's our own ghost, skip to next
                    if ((uint32_t)spectateGhostIndex == serverPlayerGhostIndex && indices.size() > 1)
                        spectateGhostIndex = indices[1];
                }
            }
            // Cycle ghosts with right mouse / R key
            static bool prevCycle = false;
            bool cycleNow = currentInput.fire || currentInput.reload;
            if (cycleNow && !prevCycle) {
                auto indices = liveGhosts.getAllIndices();
                if (!indices.empty()) {
                    int cur = 0;
                    for (size_t i = 0; i < indices.size(); i++)
                        if (indices[i] == spectateGhostIndex) { cur = (int)i; break; }
                    cur = (cur + 1) % (int)indices.size();
                    spectateGhostIndex = indices[cur];
                    // Skip our own ghost
                    if ((uint32_t)spectateGhostIndex == serverPlayerGhostIndex && indices.size() > 1) {
                        cur = (cur + 1) % (int)indices.size();
                        spectateGhostIndex = indices[cur];
                    }
                }
            }
            prevCycle = cycleNow;

            // Toggle free cam
            static bool prevFree = false;
            if (currentInput.freeCam && !prevFree) {
                freeCamActive = !freeCamActive;
                if (freeCamActive) {
                    freeCamPos = {0, 10, 0};
                    freeCamTarget = {0, 10, -1};
                    freeCamRot = {0, 0, 0};
                }
            }
            prevFree = currentInput.freeCam;

            // Free cam movement
            if (freeCamActive) {
                float pitch = freeCamRot.x, yaw = freeCamRot.z;
                pitch -= currentInput.lookDelta.x;
                if (pitch > 1.5f) pitch = 1.5f;
                if (pitch < -1.5f) pitch = -1.5f;
                yaw -= currentInput.lookDelta.y;
                freeCamRot = {pitch, 0, yaw};
                float camSpeed = 30.0f * dt;
                Point3F fwd = {sinf(yaw)*cosf(pitch), sinf(pitch), cosf(yaw)*cosf(pitch)};
                Point3F right = {cosf(yaw), 0, -sinf(yaw)};
                if (currentInput.forward) { freeCamPos.x += fwd.x*camSpeed; freeCamPos.y += fwd.y*camSpeed; freeCamPos.z += fwd.z*camSpeed; }
                if (currentInput.backward) { freeCamPos.x -= fwd.x*camSpeed; freeCamPos.y -= fwd.y*camSpeed; freeCamPos.z -= fwd.z*camSpeed; }
                if (currentInput.left) { freeCamPos.x -= right.x*camSpeed; freeCamPos.z -= right.z*camSpeed; }
                if (currentInput.right) { freeCamPos.x += right.x*camSpeed; freeCamPos.z += right.z*camSpeed; }
                if (currentInput.jump) freeCamPos.y += camSpeed;
                if (currentInput.jet) freeCamPos.y -= camSpeed;
                freeCamTarget = {freeCamPos.x + fwd.x, freeCamPos.y + fwd.y, freeCamPos.z + fwd.z};
            }
        } else {
            // Offline: auto-respawn after delay
            static float deathTimer = 0;
            deathTimer += dt;
            if (deathTimer > 3.0f) {
                deathTimer = 0;
                liveSpectateInit = false;
                pl->respawn();
                setState(Playing);
            }
        }
    }
}

void Game::render(float dt) {
    if (gameState != Playing && gameState != Dead && !testShapeLoaded && !shapeViewerActive) return;

    auto& eng = Engine::instance();
    auto& r = eng.renderer();
    r.beginFrame({0.3f, 0.5f, 0.8f, 1.0f});

    Point3F camPos, camTarget;
    if (freeCamActive) {
        camPos = freeCamPos;
        camTarget = freeCamTarget;
    } else if (demoPlaying && demoHasPos) {
        // Orbit camera for spectator mode
        if (demoOrbitCam) {
            Point3F targetPos = demoCameraPos;
            // If spectating a specific ghost, track its position
            if (spectateGhostIndex >= 0 && demoParser) {
                const GhostEntry* g = demoParser->getGhostTracker().getGhost(spectateGhostIndex);
                if (g && (g->position.x != 0 || g->position.y != 0 || g->position.z != 0))
                    targetPos = {g->position.x, g->position.y, g->position.z};
            }
            // Initialize orbit center from target position
            if (!orbitCenterInit) {
                orbitCenter = targetPos;
                orbitCenterInit = true;
            }
            // Smoothly track the target
            float trackSpeed = 0.02f;
            orbitCenter.x += (targetPos.x - orbitCenter.x) * trackSpeed;
            orbitCenter.y += (targetPos.y + 20.0f - orbitCenter.y) * trackSpeed;
            orbitCenter.z += (targetPos.z - orbitCenter.z) * trackSpeed;
            float rad = orbitAngle * (3.14159f / 180.0f);
            camPos.x = orbitCenter.x + sinf(rad) * orbitDistance;
            camPos.z = orbitCenter.z + cosf(rad) * orbitDistance;
            camPos.y = orbitCenter.y + orbitHeight;
            camTarget = orbitCenter;
            camTarget.y += 10.0f; // look slightly above center
        } else if (!demoPlaying && cfg.online && gameState == Dead && liveGhosts.size() > 0) {
            // Live spectator: follow spectated ghost
            if (!liveGhosts.hasGhost(spectateGhostIndex)) {
                auto idxs = liveGhosts.getAllIndices();
                if (!idxs.empty()) spectateGhostIndex = idxs[0];
            }
            const GhostEntry* g = liveGhosts.getGhost(spectateGhostIndex);
            if (g && (g->position.x != 0 || g->position.y != 0 || g->position.z != 0)) {
                if (freeCamActive) {
                    camPos = freeCamPos;
                    camTarget = freeCamTarget;
                } else {
                    camPos = {g->position.x, g->position.y + 4.0f, g->position.z - 6.0f};
                    camTarget = Point3F{g->position.x, g->position.y, g->position.z};
                    camTarget.y += 2.0f;
                }
            } else {
                camPos = freeCamActive ? freeCamPos : Point3F{0, 10, 0};
                camTarget = freeCamActive ? freeCamTarget : Point3F{0, 10, -1};
            }
        } else if (demoMoveBlend < 1.0f) {
            float t = demoMoveBlend;
            camPos.x = demoPrevCameraPos.x + (demoCameraPos.x - demoPrevCameraPos.x) * t;
            camPos.y = demoPrevCameraPos.y + (demoCameraPos.y - demoPrevCameraPos.y) * t;
            camPos.z = demoPrevCameraPos.z + (demoCameraPos.z - demoPrevCameraPos.z) * t;
            camTarget.x = demoPrevCameraTarget.x + (demoCameraTarget.x - demoPrevCameraTarget.x) * t;
            camTarget.y = demoPrevCameraTarget.y + (demoCameraTarget.y - demoPrevCameraTarget.y) * t;
            camTarget.z = demoPrevCameraTarget.z + (demoCameraTarget.z - demoPrevCameraTarget.z) * t;
        } else {
            camPos = demoCameraPos;
            camTarget = demoCameraTarget;
        }
    } else if (eng.hasPreviewCam()) {
        camPos = eng.getPreviewCamPos();
        camTarget = eng.getPreviewCamTarget();
    } else if (pl) {
        camPos = pl->cameraPos();
        camTarget = pl->cameraTarget();
    } else {
        camPos = {0, 6, 0};
        camTarget = {0, 6, 1};
    }
    // Apply camera shake
    Point3F finalCam = {camPos.x + shakeOffset.x, camPos.y + shakeOffset.y, camPos.z + shakeOffset.z};
    r.setCamera(finalCam, camTarget, {0, 1, 0});

    // Shadow pass and scene rendering — skip in shape viewer / test shape mode
    if (!shapeViewerActive && !testShapeLoaded) {
        if (r.shadowEnabled()) {
            // Compute scene bounds from terrain
            Point3F sceneCenter = {0, 0, 0};
            float sceneRadius = 500.0f;
            auto* tb = w->terrain();
            if (tb && tb->loaded) {
                sceneCenter.x = tb->worldOffset.x + tb->size * tb->squareSize * 0.5f;
                sceneCenter.z = tb->worldOffset.z + tb->size * tb->squareSize * 0.5f;
                float maxH = 0;
                for (auto h : tb->heights) if (h > maxH) maxH = h;
                sceneCenter.y = maxH * tb->heightScale * 0.5f;
                sceneRadius = tb->size * tb->squareSize * 0.8f;
        }
        Point3F lightDir = r.sunDir;
        float llen = std::sqrt(lightDir.x * lightDir.x + lightDir.y * lightDir.y + lightDir.z * lightDir.z);
        if (llen > 0) { lightDir.x /= llen; lightDir.y /= llen; lightDir.z /= llen; }

        r.beginShadowPass(lightDir, sceneCenter, sceneRadius);

        // Render shadow casters with shadow depth shader
        auto* shadowShader = ShaderManager::getShadowShader();
        if (shadowShader) {
            shadowShader->bind();

            // Render terrain
            auto* tb2 = w->terrain();
            if (tb2 && tb2->loaded) {
                for (auto& mesh : tb2->meshes) {
                    shadowShader->setUniform("uLightMVP", r.lightViewProj());
                    mesh.render();
                }
            }

            // Render bots
            for (auto& b : w->bots) {
                if (!b.alive || b.respawnTimer > 0) continue;
                if (b.shape && b.shape->loaded) {
                    MatrixF model;
                    model.setTranslation(b.pos);
                    MatrixF mvp = r.lightViewProj() * model * Math::czUpToYUp();
                    shadowShader->setUniform("uLightMVP", mvp);
                    r.setModel(model * Math::czUpToYUp());
                    b.shape->render(0);
                }
            }
        }

        r.endShadowPass();

        // Bind shadow map to texture unit 5 and set uniforms for main pass
        float shadowStrength = 0.6f;
        {
            auto* defShader = ShaderManager::getDefaultShader();
            defShader->bind();
            glActiveTexture(GL_TEXTURE5);
            glBindTexture(GL_TEXTURE_2D, r.shadowDepthTex);
            defShader->setUniform("uShadowMap", (int32_t)5);
            defShader->setUniform("uShadowStrength", shadowStrength);
            defShader->setUniform("uShadowMatrix", r.shadowMatrix());

            auto* terrShader = ShaderManager::getTerrainShader();
            terrShader->bind();
            glActiveTexture(GL_TEXTURE5);
            glBindTexture(GL_TEXTURE_2D, r.shadowDepthTex);
            terrShader->setUniform("uShadowMap", (int32_t)5);
            terrShader->setUniform("uShadowStrength", shadowStrength);
            terrShader->setUniform("uShadowMatrix", r.shadowMatrix());
        }
    }

    w->render(camPos);
    if (pl && !freeCamActive && !demoPlaying && !testShapeLoaded) pl->render();
    } // end if (!shapeViewerActive && !testShapeLoaded)

    if (hud && gameState == Playing) hud->render(this);

    // Connection status overlay
    if (cfg.online && activeConn && activeConn->isConnected() && liveGhosts.size() == 0) {
        auto* font = r.getFont();
        if (font) font->render("Receiving game data...", 20, 100, {1, 1, 0, 1}, 2.0f);
    }
    if (gameState == Dead && cfg.online) {
        auto* font = r.getFont();
        if (font) {
            char buf[64];
            snprintf(buf, sizeof(buf), "SPECTATOR [%s]", freeCamActive ? "Free Cam" : "Follow");
            font->render(buf, 20, 20, {0, 1, 1, 1}, 2.0f);
            font->render("Fire/Reload: Cycle  F1: Free Cam", 20, 45, {0.5f, 0.8f, 1, 1}, 1.5f);
        }
    }

    // Render test shape (loaded via testshape command)
    if (testShapeLoaded && testShape.loaded) {
        auto* defShader = ShaderManager::getDefaultShader();
        defShader->bind();
        auto& plat = Engine::instance().platform();

        // Compute model bounds once to frame the camera
        static bool boundsInit = false;
        static Point3F center{0,0,0};
        static float fitScale = 1.0f;
        if (!boundsInit) {
            Point3F mn{1e9f,1e9f,1e9f}, mx{-1e9f,-1e9f,-1e9f};
            for (auto& m : testShape.meshes)
                for (auto& v : m.vertices) {
                    if (v.pos.x < mn.x) mn.x = v.pos.x; if (v.pos.y < mn.y) mn.y = v.pos.y; if (v.pos.z < mn.z) mn.z = v.pos.z;
                    if (v.pos.x > mx.x) mx.x = v.pos.x; if (v.pos.y > mx.y) mx.y = v.pos.y; if (v.pos.z > mx.z) mx.z = v.pos.z;
                }
            center = {(mn.x+mx.x)*0.5f, (mn.y+mx.y)*0.5f, (mn.z+mx.z)*0.5f};
            float dx = mx.x-mn.x, dy = mx.y-mn.y, dz = mx.z-mn.z;
            float radius = 0.5f * std::sqrt(dx*dx+dy*dy+dz*dz);
            fitScale = (radius > 1e-3f) ? (1.0f / radius) : 1.0f;
            boundsInit = true;
        }

        static float viewYaw = 0.6f, viewPitch = 0.25f;
        if (plat.input().mouseButtons[1]) {
            viewYaw   += plat.input().mouseDeltaX * 0.005f;
            viewPitch += plat.input().mouseDeltaY * 0.005f;
            if (viewPitch > 1.5f) viewPitch = 1.5f;
            if (viewPitch < -1.5f) viewPitch = -1.5f;
        }
        MatrixF ry; ry.setRotationY(viewYaw);
        MatrixF rx; rx.setRotationX(viewPitch);
        MatrixF sc; sc.setScale({fitScale, fitScale, fitScale});
        MatrixF tr; tr.setTranslation({-center.x, -center.y, -center.z});
        MatrixF model = ry * rx * Math::czUpToYUp() * sc * tr;
        r.setModel(model);

        // Framing camera looking at the (now origin-centered, unit-radius) model
        r.setCamera({0, 0, 2.6f}, {0, 0, 0}, {0, 1, 0});

        testShape.render(0);
    }

    // Render shape viewer (shapeviewer command)
    if (shapeViewerActive && shapeViewerShape.loaded) {
        auto* defShader = ShaderManager::getDefaultShader();
        defShader->bind();
        auto& plat = Engine::instance().platform();

        // Compute model bounds to frame the camera (reset when shape changes)
        // For skeletal models, transform vertices through node world transforms
        if (!shapeViewerBoundsInit) {
            Point3F mn{1e9f,1e9f,1e9f}, mx{-1e9f,-1e9f,-1e9f};
            const auto& nodeWorld = shapeViewerShape.defaultTransforms;
            for (size_t mi = 0; mi < shapeViewerShape.meshes.size(); mi++) {
                auto& m = shapeViewerShape.meshes[mi];
                // Get the node world transform for this mesh
                MatrixF nodeXform;
                nodeXform.identity();
                if (m.nodeIndex >= 0 && m.nodeIndex < (int)nodeWorld.size())
                    nodeXform = nodeWorld[m.nodeIndex];
                // Sample a fraction of vertices for bounds (avoid scanning all)
                int step = std::max(1, (int)m.vertices.size() / 16);
                for (size_t vi = 0; vi < m.vertices.size(); vi += step) {
                    Point3F wp = nodeXform.transform(m.vertices[vi].pos);
                    if (wp.x < mn.x) mn.x = wp.x; if (wp.y < mn.y) mn.y = wp.y; if (wp.z < mn.z) mn.z = wp.z;
                    if (wp.x > mx.x) mx.x = wp.x; if (wp.y > mx.y) mx.y = wp.y; if (wp.z > mx.z) mx.z = wp.z;
                }
            }
            shapeViewerCenter = {(mn.x+mx.x)*0.5f, (mn.y+mx.y)*0.5f, (mn.z+mx.z)*0.5f};
            float dx = mx.x-mn.x, dy = mx.y-mn.y, dz = mx.z-mn.z;
            float radius = 0.5f * std::sqrt(dx*dx+dy*dy+dz*dz);
            shapeViewerFitScale = (radius > 1e-3f) ? (1.0f / radius) : 1.0f;
            shapeViewerBoundsInit = true;
        }

        // Mouse orbit (left button)
        if (plat.input().mouseButtons[1]) {
            shapeViewerYaw   += plat.input().mouseDeltaX * 0.005f;
            shapeViewerPitch += plat.input().mouseDeltaY * 0.005f;
            if (shapeViewerPitch > 1.5f) shapeViewerPitch = 1.5f;
            if (shapeViewerPitch < -1.5f) shapeViewerPitch = -1.5f;
        }
        MatrixF ry; ry.setRotationY(shapeViewerYaw);
        MatrixF rx; rx.setRotationX(shapeViewerPitch);
        MatrixF sc; sc.setScale({shapeViewerFitScale, shapeViewerFitScale, shapeViewerFitScale});
        MatrixF tr; tr.setTranslation({-shapeViewerCenter.x, -shapeViewerCenter.y, -shapeViewerCenter.z});
        // Orbit in Y-up: ry*rx rotate in Y-up, then C converts Z-up->Y-up, then sc*tr center/scale in Z-up
        MatrixF model = ry * rx * Math::czUpToYUp() * sc * tr;
        r.setModel(model);

        r.setCamera({0, 0, 2.6f}, {0, 0, 0}, {0, 1, 0});

        shapeViewerShape.render(0);

        // HUD overlay
        auto* font = r.getFont();
        if (font) {
            char buf[256];
            snprintf(buf, sizeof(buf), "[%d/%zu] %s", shapeViewerIndex + 1, shapeViewerFiles.size(),
                shapeViewerFiles[shapeViewerIndex].c_str());
            font->render(buf, 20, 20, {1, 1, 0, 1}, 1.5f);

            if (!shapeViewerShape.animations.empty()) {
                const auto& anim = shapeViewerShape.animations[0];
                snprintf(buf, sizeof(buf), "Anim: %s (%.1fs)", anim.name.c_str(), anim.duration);
                font->render(buf, 20, 45, {0.7f, 0.9f, 1, 1}, 1.2f);
            }
            snprintf(buf, sizeof(buf), "Left/Right: cycle  Mouse: orbit  Esc: exit");
            font->render(buf, 20, 65, {0.5f, 0.7f, 0.8f, 1}, 1.0f);
        }
    }

    // Render demo ghost objects as 3D shapes
    if (demoPlaying && demoParser) {
        // Ensure default shader is bound for ghost rendering
        auto* defShader = ShaderManager::getDefaultShader();
        if (defShader) defShader->bind();

        const GhostTracker& gt = demoParser->getGhostTracker();
        std::vector<int> indices = gt.getAllIndices();
        for (int idx : indices) {
            const GhostEntry* g = gt.getGhost(idx);
            if (!g) continue;

            // Resolve player name from skin name if not already set
            if (g->playerName.empty() && !g->skinName.empty()) {
                const std::string& pn = demoParser->getPlayerNameForSkin(g->skinName);
                if (!pn.empty()) {
                    GhostEntry* mg = const_cast<GhostEntry*>(g);
                    mg->playerName = pn;
                }
            }
            Vec3 p = g->position;
            if (p.x == 0 && p.y == 0 && p.z == 0) continue; // no position data yet

            // Skip world-level objects already rendered by World
            if (!isRenderableGhostClass(g->className)) continue;

            // Interpolate position for smooth rendering (applies to both shape and fallback paths)
            GhostEntry* mg = const_cast<GhostEntry*>(g);
            Vec3 rp = p;
            if (!mg->hasRendered) {
                mg->renderPos = p;
                mg->renderRotation = g->rotation;
                mg->prevPosition = p;
            } else {
                float lerpFactor = 1.0f - expf(-12.0f * dt);
                mg->renderPos.x += (p.x - mg->renderPos.x) * lerpFactor;
                mg->renderPos.y += (p.y - mg->renderPos.y) * lerpFactor;
                mg->renderPos.z += (p.z - mg->renderPos.z) * lerpFactor;

                // Interpolate rotation via exponential smoothing + renormalization
                if (g->hasRotation) {
                    Vec4 target = g->rotation;
                    float dot = mg->renderRotation.x * target.x +
                                mg->renderRotation.y * target.y +
                                mg->renderRotation.z * target.z +
                                mg->renderRotation.w * target.w;
                    if (dot < 0) { target.x = -target.x; target.y = -target.y; target.z = -target.z; target.w = -target.w; }
                    mg->renderRotation.x += (target.x - mg->renderRotation.x) * lerpFactor;
                    mg->renderRotation.y += (target.y - mg->renderRotation.y) * lerpFactor;
                    mg->renderRotation.z += (target.z - mg->renderRotation.z) * lerpFactor;
                    mg->renderRotation.w += (target.w - mg->renderRotation.w) * lerpFactor;
                    float invLen = 1.0f / sqrtf(mg->renderRotation.x * mg->renderRotation.x +
                                                 mg->renderRotation.y * mg->renderRotation.y +
                                                 mg->renderRotation.z * mg->renderRotation.z +
                                                 mg->renderRotation.w * mg->renderRotation.w);
                    mg->renderRotation.x *= invLen; mg->renderRotation.y *= invLen;
                    mg->renderRotation.z *= invLen; mg->renderRotation.w *= invLen;
                }
            }
            rp = mg->renderPos;

            // Compute velocity from interpolated position (smooth)
            float dx = rp.x - mg->prevPosition.x;
            float dz = rp.z - mg->prevPosition.z;
            float dist = sqrtf(dx*dx + dz*dz);
            float speed = (dt > 0.001f) ? dist / dt : 0;
            mg->isMoving = (speed > 0.1f);
            if (mg->isMoving) {
                mg->moveYaw = atan2f(dx, dz);
                mg->animTime += dt;
            } else {
                mg->animTime = fmodf(mg->animTime, 10.0f) + dt * 0.3f; // slow idle
            }
            mg->prevPosition = rp;
            mg->hasRendered = true;

            // Try to get or load the DTS shape for this ghost class
            DTSShape* shape = const_cast<DTSShape*>(g->shape);
            if (!shape && g->className.empty() == false) {
                GhostEntry* mutableG = const_cast<GhostEntry*>(g);
                mutableG->shape = getOrLoadDemoShape(g->className, g->skinName);
                shape = mutableG->shape;
            }

            if (shape && shape->loaded) {
                // Apply skin textures on first render (player ghosts only)
                if (!mg->skinApplied && !g->skinName.empty() &&
                    (g->className == "Player" || g->className == "MPB")) {
                    if (shape->applySkin(g->skinName))
                        Console::instance().printf(LogLevel::Debug,
                            "Ghost[%d]: applied skin '%s' to shape '%s'",
                            idx, g->skinName.c_str(), shape->name.c_str());
                    mg->skinApplied = true;
                }

                // Build model matrix
                MatrixF model;
                if (g->hasRotation) {
                    QuatF q(mg->renderRotation.x, mg->renderRotation.y, mg->renderRotation.z, mg->renderRotation.w);
                    model = q.toMatrix();
                } else if (mg->isMoving || g->className == "Player" || g->className == "MPB") {
                    float yaw = mg->moveYaw;
                    model.setRotationAxis({0, 1, 0}, -yaw);
                } else {
                    model.identity();
                }
                model.setTranslation({rp.x, rp.y, rp.z});
                r.setModel(model * Math::czUpToYUp());

                // Apply skin-based tint color for player ghosts
                {
                    ColorF tint = {1, 1, 1, 1};
                    if (g->className == "Player" || g->className == "MPB") {
                        const std::string& sn = g->skinName;
                        // Only tint if skin textures weren't applied (white tint = no tint)
                        if (!mg->skinApplied) {
                            if (sn.find("red") != std::string::npos)
                                tint = {1.0f, 0.2f, 0.2f, 1.0f};
                            else if (sn.find("blue") != std::string::npos)
                                tint = {0.2f, 0.3f, 1.0f, 1.0f};
                            else if (sn.find("green") != std::string::npos)
                                tint = {0.2f, 0.8f, 0.2f, 1.0f};
                            else if (sn.find("yellow") != std::string::npos)
                                tint = {1.0f, 0.9f, 0.1f, 1.0f};
                            else if (sn.find("purple") != std::string::npos)
                                tint = {0.7f, 0.2f, 0.8f, 1.0f};
                            else if (sn.find("orange") != std::string::npos)
                                tint = {1.0f, 0.5f, 0.1f, 1.0f};
                            else if (sn.find("black") != std::string::npos)
                                tint = {0.3f, 0.3f, 0.3f, 1.0f};
                            else if (sn.find("white") != std::string::npos)
                                tint = {0.9f, 0.9f, 0.9f, 1.0f};
                        }
                    }
                    if (defShader) defShader->setUniform("uTint", tint);
                }

                // Pick animation: try multiple animation names per class
                const char* animName = nullptr;
                const char* altName = nullptr;
                bool isPlayer = (g->className == "Player" || g->className == "MPB");
                bool isVehicle = (g->className.find("Vehicle") != std::string::npos ||
                                  g->className == "Shrike" || g->className == "Turbograv" ||
                                  g->className == "Shield" || g->className == "Wildcat");
                if (isPlayer) {
                    animName = mg->isMoving ? "run" : "stand";
                    altName  = mg->isMoving ? "run" : "idle";
                } else if (isVehicle) {
                    animName = mg->isMoving ? "hover" : "float";
                    altName  = mg->isMoving ? "float" : "still";
                }
                if (animName) {
                    bool found = false;
                    for (auto& a : shape->animations)
                        if (a.name == animName) { found = true; break; }
                    shape->renderAnimation(found ? animName : (altName ? altName : animName), mg->animTime);
                } else {
                    shape->render(0);
                }
            } else {
                // Fallback: colored box with size based on class
                float size = 0.8f;
                if (g->className == "Shrike" || g->className == "FlyingVehicle" ||
                    g->className == "Turbograv" || g->className == "HoverVehicle" ||
                    g->className == "Shield" || g->className == "Vehicle")
                    size = 2.0f;
                else if (g->className == "Turret" || g->className == "Sentry" ||
                         g->className == "Generator")
                    size = 1.5f;
                Box3F box;
                box.min = {rp.x - size, rp.y - size, rp.z - size};
                box.max = {rp.x + size, rp.y + size, rp.z + size};
                float rcol = 0.3f + 0.7f * ((g->classId * 37) % 255) / 255.0f;
                float gcol = 0.3f + 0.7f * ((g->classId * 73) % 255) / 255.0f;
                float bcol = 0.3f + 0.7f * ((g->classId * 131) % 255) / 255.0f;
                r.drawBox(box, {rcol, gcol, bcol, 1.0f});
            }

            // Ground shadow for all renderable ghosts
            if (isRenderableGhostClass(g->className)) {
                float groundH = 0.0f;
                if (w->terrain() && w->terrain()->loaded)
                    groundH = w->getHeight(rp.x, rp.z);
                float shadowY = std::max(groundH, 0.0f);
                float shadowSize = (g->className == "Player" || g->className == "MPB") ? 0.8f : 1.5f;
                float distAboveGround = rp.y - shadowY;
                if (distAboveGround > 0 && distAboveGround < 50.0f) {
                    float shadowAlpha = std::max(0.05f, 0.4f - distAboveGround * 0.008f);
                    r.drawBox({{rp.x - shadowSize, shadowY + 0.1f, rp.z - shadowSize},
                               {rp.x + shadowSize, shadowY + 0.1f, rp.z + shadowSize}},
                              {0, 0, 0, shadowAlpha});
                }
            }

            // Highlight ring for control object (recording player)
            if (idx == controlGhostIndex) {
                float pulse = sinf(demoTime * 4.0f) * 0.3f + 0.7f;
                float ringY = rp.y - 0.5f;
                float ringR = 1.2f + pulse * 0.3f;
                int segments = 20;
                std::vector<Point3F> ring;
                for (int i = 0; i <= segments; i++) {
                    float a = (float)i / (float)segments * 6.28318f;
                    ring.push_back({rp.x + cosf(a) * ringR, ringY, rp.z + sinf(a) * ringR});
                }
                r.drawLineStrip(ring, {0.3f, 1.0f, 0.5f, 0.7f + pulse * 0.3f});
            }

            // Update projectile trail
            bool isProjectile = (g->className.find("Projectile") != std::string::npos ||
                g->className == "EnergyBolt" || g->className == "LinearFlare" ||
                g->className.find("Tracer") != std::string::npos);
            if (isProjectile) {
                auto& trail = demoTrails[idx];
                trail.push_back(TrailPoint{rp.x, rp.y, rp.z, 1.0f});
                if (trail.size() > 30) trail.erase(trail.begin());
            }
        }

        // Render projectile trails
        if (!demoTrails.empty()) {
            for (auto it = demoTrails.begin(); it != demoTrails.end(); ) {
                auto& pts = it->second;
                // Age and cull trail points
                for (int i = (int)pts.size() - 1; i >= 0; i--) {
                    pts[i].life -= dt * 2.0f;
                    if (pts[i].life <= 0) pts.erase(pts.begin() + i);
                }
                if (pts.empty()) { it = demoTrails.erase(it); continue; } else { ++it; }
                // Draw trail as a fading line strip with glow
                if (pts.size() >= 2) {
                    std::vector<Point3F> linePts;
                    for (auto& tp : pts) linePts.push_back({tp.x, tp.y, tp.z});
                    float alpha = pts.back().life;
                    // Glow layer (wider, fainter)
                    { std::vector<Point3F> glowPts = linePts;
                      float ga = alpha * 0.25f;
                      Point3F off = {0.3f, 0, 0.3f};
                      for (auto& p : glowPts) { p.x += off.x; p.z += off.z; }
                      r.drawLineStrip(glowPts, {0.2f, 0.6f, 1.0f, ga}); }
                    { std::vector<Point3F> glowPts = linePts;
                      float ga = alpha * 0.25f;
                      Point3F off = {-0.3f, 0, -0.3f};
                      for (auto& p : glowPts) { p.x += off.x; p.z += off.z; }
                      r.drawLineStrip(glowPts, {0.2f, 0.6f, 1.0f, ga}); }
                    // Core line
                    r.drawLineStrip(linePts, {0.5f, 1.0f, 1.0f, alpha * 0.9f});
                }
            }
        }

        // Spectator HUD: name tags and health bars above ghosts
        if (demoPlaying && demoParser) {
            auto* font = r.getFont();
            if (font) {
                int screenW = 1024, screenH = 768;
                for (int idx : indices) {
                    const GhostEntry* g = gt.getGhost(idx);
                    if (!g) continue;
                    if (g->position.x == 0 && g->position.y == 0 && g->position.z == 0) continue;
                    if (!isRenderableGhostClass(g->className)) continue;
                    Point3F above = {g->renderPos.x, g->renderPos.y + 2.5f, g->renderPos.z};
                    Point3F screen = worldToScreen(above, r.viewMatrix(), r.projectionMatrix(), screenW, screenH);
                    if (screen.x < 0 || screen.x > screenW || screen.y < 0 || screen.y > screenH) continue;
                    ColorF col{1, 1, 1, 1};
                    std::string sn = g->skinName;
                    for (auto& c : sn) c = (char)tolower(c);
                    if (sn.find("red") != std::string::npos) col = {1, 0.2f, 0.2f, 1};
                    else if (sn.find("blue") != std::string::npos) col = {0.2f, 0.3f, 1, 1};
                    else if (sn.find("green") != std::string::npos) col = {0.2f, 0.8f, 0.2f, 1};
                    std::string label = g->className;
                    if (!g->playerName.empty()) label = g->playerName;
                    font->render(label.c_str(), screen.x - 30, screen.y - 20, col, 1.2f);
                    float barW = 50, barH = 6;
                    float bx = screen.x - barW/2;
                    float by = screen.y + 2;
                    r.drawBox({{bx-1, by-1, 0}, {bx+barW+1, by+barH+1, 0}}, {0, 0, 0, 0.6f});
                    float healthFrac = (g->health > 0) ? (g->health / 100.0f) : 1.0f;
                    ColorF healthCol = healthFrac > 0.5f ? ColorF{0, 1, 0, 0.8f} :
                                      healthFrac > 0.25f ? ColorF{1, 1, 0, 0.8f} : ColorF{1, 0, 0, 0.8f};
                    r.drawBox({{bx, by, 0}, {bx + barW * healthFrac, by + barH, 0}}, healthCol);
                    float ey2 = by + barH + 1;
                    float energyFrac = g->energy / 100.0f;
                    r.drawBox({{bx-1, ey2-1, 0}, {bx+barW+1, ey2+barH+1, 0}}, {0, 0, 0, 0.6f});
                    r.drawBox({{bx, ey2, 0}, {bx + barW * energyFrac, ey2 + barH, 0}}, {0.3f, 0.5f, 1, 0.8f});
                }
            }
        }
    }

    // Render live network ghosts (multiplayer)
    if (!demoPlaying && liveGhosts.size() > 0 && activeConn && activeConn->isConnected()) {
        auto* defShader = ShaderManager::getDefaultShader();
        if (defShader) defShader->bind();

        std::vector<int> indices = liveGhosts.getAllIndices();
        for (int idx : indices) {
            // Skip our own player ghost (we render locally via pl->render)
            if (serverPlayerGhostSynced && (uint32_t)idx == serverPlayerGhostIndex) continue;
            GhostEntry* g = liveGhosts.getMutableGhost(idx);
            if (!g) continue;
            Vec3 p = g->position;
            if (p.x == 0 && p.y == 0 && p.z == 0) continue;
            if (!isRenderableGhostClass(g->className)) continue;

            // Smooth interpolation
            Vec3 rp = p;
            if (!g->hasRendered) {
                g->renderPos = p;
                g->renderRotation = g->rotation;
                g->prevPosition = p;
                g->hasRendered = true;
            } else {
                float lerpFactor = 1.0f - expf(-12.0f * dt);
                g->renderPos.x += (p.x - g->renderPos.x) * lerpFactor;
                g->renderPos.y += (p.y - g->renderPos.y) * lerpFactor;
                g->renderPos.z += (p.z - g->renderPos.z) * lerpFactor;

                // Interpolate rotation
                if (g->hasRotation) {
                    Vec4 target = g->rotation;
                    float dot = g->renderRotation.x * target.x +
                                g->renderRotation.y * target.y +
                                g->renderRotation.z * target.z +
                                g->renderRotation.w * target.w;
                    if (dot < 0) { target.x = -target.x; target.y = -target.y; target.z = -target.z; target.w = -target.w; }
                    g->renderRotation.x += (target.x - g->renderRotation.x) * lerpFactor;
                    g->renderRotation.y += (target.y - g->renderRotation.y) * lerpFactor;
                    g->renderRotation.z += (target.z - g->renderRotation.z) * lerpFactor;
                    g->renderRotation.w += (target.w - g->renderRotation.w) * lerpFactor;
                    float invLen = 1.0f / sqrtf(g->renderRotation.x * g->renderRotation.x +
                                                 g->renderRotation.y * g->renderRotation.y +
                                                 g->renderRotation.z * g->renderRotation.z +
                                                 g->renderRotation.w * g->renderRotation.w);
                    g->renderRotation.x *= invLen; g->renderRotation.y *= invLen;
                    g->renderRotation.z *= invLen; g->renderRotation.w *= invLen;
                }
            }
            rp = g->renderPos;

            // Try to load a shape for this ghost class
            if (!g->shape) {
                g->shape = getOrLoadDemoShape(g->className, g->skinName);
            }

            if (g->shape && g->shape->loaded) {
                MatrixF model;
                if (g->hasRotation) {
                    QuatF q(g->renderRotation.x, g->renderRotation.y, g->renderRotation.z, g->renderRotation.w);
                    model = q.toMatrix();
                }
                model.setTranslation({rp.x, rp.y, rp.z});
                r.setModel(model * Math::czUpToYUp());
                g->shape->render(0);
            } else {
                // Fallback box
                float size = 0.8f;
                Box3F box;
                box.min = {rp.x - size, rp.y - size, rp.z - size};
                box.max = {rp.x + size, rp.y + size, rp.z + size};
                float rcol = 0.3f + 0.7f * ((g->classId * 37) % 255) / 255.0f;
                float gcol = 0.3f + 0.7f * ((g->classId * 73) % 255) / 255.0f;
                float bcol = 0.3f + 0.7f * ((g->classId * 131) % 255) / 255.0f;
                r.drawBox(box, {rcol, gcol, bcol, 1.0f});
            }
        }

        // Spectator HUD for live ghosts
        if (!demoPlaying && activeConn && activeConn->isConnected() && liveGhosts.size() > 0) {
            auto* font = r.getFont();
            if (font) {
                std::vector<int> hudIndices = liveGhosts.getAllIndices();
                int screenW = 1024, screenH = 768;
                for (int idx : hudIndices) {
                    if (serverPlayerGhostSynced && (uint32_t)idx == serverPlayerGhostIndex) continue;
                    const GhostEntry* g = liveGhosts.getGhost(idx);
                    if (!g) continue;
                    if (g->position.x == 0 && g->position.y == 0 && g->position.z == 0) continue;
                    if (!isRenderableGhostClass(g->className)) continue;
                    Point3F above = {g->renderPos.x, g->renderPos.y + 2.5f, g->renderPos.z};
                    Point3F screen = worldToScreen(above, r.viewMatrix(), r.projectionMatrix(), screenW, screenH);
                    if (screen.x < 0 || screen.x > screenW || screen.y < 0 || screen.y > screenH) continue;
                    ColorF col{1, 1, 1, 1};
                    std::string label = g->className;
                    font->render(label.c_str(), screen.x - 30, screen.y - 20, col, 1.2f);
                    float barW = 50, barH = 6;
                    float bx = screen.x - barW/2;
                    float by = screen.y + 2;
                    r.drawBox({{bx-1, by-1, 0}, {bx+barW+1, by+barH+1, 0}}, {0, 0, 0, 0.6f});
                    float healthFrac = (g->health > 0) ? (g->health / 100.0f) : 1.0f;
                    ColorF healthCol = healthFrac > 0.5f ? ColorF{0, 1, 0, 0.8f} :
                                      healthFrac > 0.25f ? ColorF{1, 1, 0, 0.8f} : ColorF{1, 0, 0, 0.8f};
                    r.drawBox({{bx, by, 0}, {bx + barW * healthFrac, by + barH, 0}}, healthCol);
                    float ey2 = by + barH + 1;
                    float energyFrac = (g->energy > 0) ? (g->energy / 100.0f) : 1.0f;
                    r.drawBox({{bx-1, ey2-1, 0}, {bx+barW+1, ey2+barH+1, 0}}, {0, 0, 0, 0.6f});
                    r.drawBox({{bx, ey2, 0}, {bx + barW * energyFrac, ey2 + barH, 0}}, {0.3f, 0.5f, 1, 0.8f});
                }
            }
        }
    }

    // 3D demo path trail
    if (demoPlaying && demoPathCount > 1) {
        // Full path in dim green
        r.drawLineStrip(demoPath, {0.2f, 0.8f, 0.2f, 0.6f});
    }

    // GUI overlay (console, dialogs, etc.)
    {
        auto& eng2 = Engine::instance();
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        eng2.guiRenderer().render();
        glEnable(GL_DEPTH_TEST);
    }

    r.endFrame();
}

void Game::startLocalGame(const char* map) {
    Console::instance().printf(LogLevel::Info, "Starting local game");
    setState(Loading);

    std::string missionPath;

    if (map && map[0]) {
        missionPath = map;
        Console::instance().printf(LogLevel::Info, "Using specified mission: %s", missionPath.c_str());
    } else {
        // Dynamically discover available missions
        auto& fs = Engine::instance().fs();
        std::vector<std::string> allEntries;
        fs.listFiles("missions/", allEntries);
        std::vector<std::string> foundMissions;
        for (auto& e : allEntries) {
            size_t dot = e.rfind('.');
            std::string ext = (dot != std::string::npos) ? e.substr(dot) : "";
            if (ext == ".mis" || ext == ".misPK") {
                size_t slash = e.rfind('/');
                std::string base = (slash != std::string::npos) ? e.substr(slash + 1) : e;
                size_t edot = base.rfind('.');
                if (edot != std::string::npos) base = base.substr(0, edot);
                if (!base.empty()) foundMissions.push_back(base);
            }
        }

        if (!foundMissions.empty()) {
            std::sort(foundMissions.begin(), foundMissions.end());
            // Prefer Training1, then Training2, then Katabatic
            std::vector<std::string> priority = {"Training1", "Training2", "Katabatic"};
            missionPath = foundMissions[0];
            for (auto& p : priority)
                if (std::find(foundMissions.begin(), foundMissions.end(), p) != foundMissions.end())
                    { missionPath = p; break; }
            Console::instance().printf(LogLevel::Info, "Found %zu missions, loading: %s", foundMissions.size(), missionPath.c_str());
        } else {
            // Fallback to hardcoded list if dynamic discovery fails
            const char* mapsToTry[] = {
                "Training1", "Training2", "Katabatic", "Damnation", nullptr
            };
            for (int i = 0; mapsToTry[i]; i++) {
                std::string p = std::string("missions/") + mapsToTry[i] + ".mis";
                if (!fs.read(p.c_str()).empty()) {
                    missionPath = mapsToTry[i];
                    Console::instance().printf(LogLevel::Info, "Found mission (fallback): %s", p.c_str());
                    break;
                }
            }
        }

        if (missionPath.empty()) {
            missionPath = "Training1";
            Console::instance().printf(LogLevel::Info, "Using default mission: %s", missionPath.c_str());
        }
    }

    // Classify weather by mission name for ambient audio selection
    weatherType = 0; // dry
    if (missionPath.find("Whiteout") != std::string::npos ||
        missionPath.find("SolsDescent") != std::string::npos)
        weatherType = 1; // cold/windy
    else if (missionPath.find("Training2") != std::string::npos ||
             missionPath.find("Swamp") != std::string::npos)
        weatherType = 2; // wet

    if (w->load(missionPath.c_str())) {
        pl->respawn();
        // Use mission spawn point, or fall back to above terrain center
        Point3F spawnPos = w->spawnPoint();
        float h = w->getHeight(spawnPos.x, spawnPos.z);
        if (spawnPos.y < h + 0.5f) spawnPos.y = h + 2.0f;
        else if (spawnPos.y > h + 10.0f) spawnPos.y = h + 2.0f; // bring down from sky spawns
        pl->setPosition(spawnPos);
        setState(Playing);
        Console::instance().printf(LogLevel::Info, "Game started on '%s'", missionPath.c_str());

        // Spawn practice bots
        w->spawnBots(6);

        // Clear TS GUI dialogs and switch to in-game view
        auto& gui = Engine::instance().guiRenderer();
        gui.setContent("PlayGui");
        gui.pushDialog("PlayGui");

        // Start ambient audio
        auto& audio = Engine::instance().audio();
        if (audio.config().enabled) {
            const char* ambPath;
            if (weatherType == 1)      ambPath = "audio/fx/environment/coldwind1.wav";
            else if (weatherType == 2) ambPath = "audio/fx/environment/wetwind.wav";
            else                        ambPath = "audio/fx/environment/drywind.wav";
            ambientSound = audio.loadSound(ambPath);
            if (ambientSound) {
                ambientSource = audio.createSource();
                ambientSource->setLooping(true);
                ambientSource->setVolume(0.3f);
                ambientSource->play(ambientSound);
                Console::instance().printf(LogLevel::Info, "Ambient: %s", ambPath);
            }
        }
    } else {
        Console::instance().printf(LogLevel::Error, "Failed to load map '%s'", missionPath.c_str());
        setState(MenuScreen);
    }
}

void Game::connectToServer(const char* host, uint16_t port) {
    cfg.serverHost = host;
    cfg.serverPort = port;
    cfg.online = true;

    Console::instance().printf(LogLevel::Info, "Connecting to %s:%d...", host, port);
    // Show connecting message
    if (hud) hud->showMessage("Connecting...", ColorF{1, 1, 0, 1});

    auto& net = Engine::instance().network();
    activeConn = net.createConnection();
    if (activeConn->connect(host, port)) {
        activeConn->setConnectCallback([this](bool success) {
            if (success) {
                Console::instance().printf(LogLevel::Info, "Connected!");
                // Send player name to server
                std::string nameCmd = std::string("sv_name ") + cfg.playerName;
                uint8_t buf[256];
                buf[0] = T2Protocol::GDT_Command;
                uint16_t nl = (uint16_t)nameCmd.size();
                buf[1] = (uint8_t)(nl & 0xFF);
                buf[2] = (uint8_t)(nl >> 8);
                memcpy(buf + 3, nameCmd.data(), nl);
                activeConn->sendGamePacket(buf, 3 + nl, false);
            } else {
                Console::instance().printf(LogLevel::Info, "Connection failed");
                liveSpectateInit = false;
                spectateGhostIndex = -1;
                liveGhosts.clear();
                setState(MenuScreen);
            }
        });

        // Handle incoming packets
        activeConn->setPacketCallback([this](PacketType type, const uint8_t* data, size_t size) {
            if (type == PacketType::ConnectOK) {
                activeConn->setState(Connection::Connected);
                Console::instance().printf(LogLevel::Info, "Connection established, entering game");
                startLocalGame();
            } else if (type == PacketType::GameData && size > 0) {
                // Check for command packet
                if (data[0] == T2Protocol::GDT_Command && size >= 3) {
                    uint16_t cmdLen = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
                    if (cmdLen > 0 && (size_t)(3 + cmdLen) <= size) {
                        std::string cmd((const char*)data + 3, cmdLen);
                        Console::instance().printf(LogLevel::Debug, "Net received command: %s", cmd.c_str());
                        Console::instance().execute(cmd.c_str());
                    }
                    return;
                }

                // Handle datablock packets
                if (data[0] == T2Protocol::GDT_Datablock) {
                    T2Protocol::DatablockHeader hdr;
                    const uint8_t* payload;
                    size_t payloadLen;
                    if (T2Protocol::decodeDatablock(data, size, hdr, payload, payloadLen)) {
                        ReceivedDatablock rdb;
                        rdb.hdr = hdr;
                        rdb.payload.assign(payload, payload + payloadLen);
                        receivedDatablocks[hdr.classId].push_back(std::move(rdb));
                        Console::instance().printf(LogLevel::Debug,
                            "Received datablock: class=%u obj=%u idx=%u/%u (%zu bytes)",
                            (unsigned)hdr.classId, (unsigned)hdr.objectId,
                            (unsigned)hdr.index, (unsigned)hdr.total, payloadLen);
                    }
                    return;
                }

                // Handle chat messages
                if (data[0] == T2Protocol::GDT_ChatMessage) {
                    T2Protocol::ChatMessage chat;
                    if (T2Protocol::decodeChat(data, size, chat)) {
                        Console::instance().printf(LogLevel::Info, "[CHAT] %s: %s", chat.sender, chat.text);
                        playChatBeep();
                    }
                    return;
                }

                // Handle game state packets
                if (data[0] == T2Protocol::GDT_GameState) {
                    T2Protocol::GameStateMessage gs;
                    if (T2Protocol::decodeGameState(data, size, gs)) {
                        serverPlayerGhostIndex = gs.controlObjectGhostIndex;
                        serverPlayerGhostSynced = true;
                        Console::instance().printf(LogLevel::Info,
                            "GameState: control ghost idx=%u", (unsigned)gs.controlObjectGhostIndex);
                    }
                    return;
                }

                // Handle ghost packets (server batches multiple ghosts into one datagram — loop over them)
                if (data[0] == T2Protocol::GDT_Ghost || data[0] == T2Protocol::GDT_GhostAlways) {
                    const uint8_t* gp = data;
                    size_t grem = size;
                    while (grem > 0 && (gp[0] == T2Protocol::GDT_Ghost || gp[0] == T2Protocol::GDT_GhostAlways)) {
                        T2Protocol::GhostMessage gm;
                        if (!T2Protocol::decodeGhostHeader(gp, grem, gm)) break;
                        size_t hdrSize = 1 + 4 + 1 + 4; // GDT + index + type + classId
                        const uint8_t* ghostPayload = gp + hdrSize;
                        size_t ghostPayloadLen = (grem > hdrSize) ? grem - hdrSize : 0;

                        if (gm.type == T2Protocol::Ghost_Delete) {
                            liveGhosts.deleteGhost((int)gm.index);
                            Console::instance().printf(LogLevel::Debug, "Ghost delete idx=%u", (unsigned)gm.index);
                        } else if (gm.type == T2Protocol::Ghost_Create) {
                            if (!liveGhosts.hasGhost((int)gm.index)) {
                                std::string cn;
                                if (gm.classId >= 0 && gm.classId < T2Demo::NetObjectClassCount)
                                    cn = T2Demo::NetObjectClassNames[gm.classId];
                                else
                                    cn = "Class" + std::to_string(gm.classId);
                                liveGhosts.createGhost((int)gm.index, gm.classId, cn);
                                // Look up datablock and apply config
                                auto dbs = getDatablocksForClass((uint32_t)gm.classId);
                                if (dbs && !dbs->empty()) {
                                    auto* ge = liveGhosts.getMutableGhost((int)gm.index);
                                    if (ge) {
                                        const auto& payload = dbs->front().payload;
                                        if (payload.size() >= 6) {
                                            float hp;
                                            memcpy(&hp, payload.data(), 4);
                                            ge->maxHealth = hp;
                                            uint16_t nameLen;
                                            memcpy(&nameLen, payload.data() + 4, 2);
                                            if (nameLen > 0 && (size_t)(6 + nameLen) <= payload.size()) {
                                                ge->shapeName.assign((const char*)payload.data() + 6, nameLen);
                                            }
                                        }
                                    }
                                }
                                // Read position from payload
                                if (ghostPayloadLen >= 24) {
                                    float px, py, pz, rx, rz, hp;
                                    uint32_t p = 0;
                                    memcpy(&px, ghostPayload + p, 4); p += 4;
                                    memcpy(&py, ghostPayload + p, 4); p += 4;
                                    memcpy(&pz, ghostPayload + p, 4); p += 4;
                                    memcpy(&rx, ghostPayload + p, 4); p += 4;
                                    memcpy(&rz, ghostPayload + p, 4); p += 4;
                                    memcpy(&hp, ghostPayload + p, 4); p += 4;
                                    auto* ge = liveGhosts.getMutableGhost((int)gm.index);
                                    if (ge) {
                                        ge->position = {px, py, pz};
                                        float halfYaw = rz * 0.5f;
                                        float halfPitch = rx * 0.5f;
                                        float cy = cosf(halfYaw), sy = sinf(halfYaw);
                                        float cp = cosf(halfPitch), sp = sinf(halfPitch);
                                        ge->rotation = {sp * cy, cp * sy, sp * sy, cp * cy};
                                        ge->hasRotation = true;
                                        ge->health = hp;
                                        // Parse kills/deaths/team/name (payload: 24=pos, 8=k/d, 4=team, name)
                                        if (ghostPayloadLen >= 36) {
                                            float fk, fd, fteam;
                                            memcpy(&fk, ghostPayload + p, 4); p += 4;
                                            memcpy(&fd, ghostPayload + p, 4); p += 4;
                                            memcpy(&fteam, ghostPayload + p, 4); p += 4;
                                            ge->kills = (int32_t)fk;
                                            ge->deaths = (int32_t)fd;
                                            ge->teamId = (int32_t)fteam;
                                            // Parse player name if present (null-terminated, bounded)
                                            if (ghostPayloadLen > p && ghostPayload[p] != 0) {
                                                size_t maxName = ghostPayloadLen - p;
                                                size_t nl = 0;
                                                while (nl < maxName && ghostPayload[p + nl] != 0) nl++;
                                                ge->playerName.assign((const char*)ghostPayload + p, nl);
                                            }
                                        }
                                    }
                                }
                                Console::instance().printf(LogLevel::Debug,
                                    "Ghost create idx=%u class=%d (%s)", (unsigned)gm.index, gm.classId, cn.c_str());
                            }
                        } else if (gm.type == T2Protocol::Ghost_Update) {
                            auto* ge = liveGhosts.getMutableGhost((int)gm.index);
                            if (ge && ghostPayloadLen >= 24) {
                                float px, py, pz, rx, rz, hp;
                                uint32_t p = 0;
                                memcpy(&px, ghostPayload + p, 4); p += 4;
                                memcpy(&py, ghostPayload + p, 4); p += 4;
                                memcpy(&pz, ghostPayload + p, 4); p += 4;
                                memcpy(&rx, ghostPayload + p, 4); p += 4;
                                memcpy(&rz, ghostPayload + p, 4); p += 4;
                                memcpy(&hp, ghostPayload + p, 4); p += 4;
                                ge->position = {px, py, pz};
                                float halfYaw = rz * 0.5f;
                                float halfPitch = rx * 0.5f;
                                float cy = cosf(halfYaw), sy = sinf(halfYaw);
                                float cp = cosf(halfPitch), sp = sinf(halfPitch);
                                ge->rotation = {sp * cy, cp * sy, sp * sy, cp * cy};
                                 ge->hasRotation = true;
                                 ge->health = hp;
                                 // Parse kills/deaths/team/name
                                 if (ghostPayloadLen >= 36) {
                                     float fk, fd, fteam;
                                     memcpy(&fk, ghostPayload + p, 4); p += 4;
                                     memcpy(&fd, ghostPayload + p, 4); p += 4;
                                     memcpy(&fteam, ghostPayload + p, 4); p += 4;
                                      ge->kills = (int32_t)fk;
                                      ge->deaths = (int32_t)fd;
                                      ge->teamId = (int32_t)fteam;
                                      if (ghostPayloadLen > p && ghostPayload[p] != 0) {
                                          size_t maxName = ghostPayloadLen - p;
                                          size_t nl = 0;
                                          while (nl < maxName && ghostPayload[p + nl] != 0) nl++;
                                          ge->playerName.assign((const char*)ghostPayload + p, nl);
                                      }
                                  }
                             }
                         }
                         // Advance to the next ghost in the batch (variable-length null-terminated name).
                         size_t consumed = hdrSize;
                         if (gm.type != T2Protocol::Ghost_Delete && ghostPayloadLen >= 36) {
                             size_t avail = ghostPayloadLen - 36;
                             const uint8_t* np = ghostPayload + 36;
                             size_t nl = 0;
                             while (nl < avail && np[nl] != 0) nl++;
                             consumed += 36 + (nl < avail ? nl + 1 : avail);
                         } else {
                             consumed += ghostPayloadLen;
                         }
                         if (consumed > grem) break;
                         gp += consumed;
                         grem -= consumed;
                     }
                     return;
                     }

                     T2Protocol::UpdateMessage update;
                if (T2Protocol::decodeUpdate(data, size, update)) {
                    // Reconcile: set to server state then replay pending moves
                    Point3F serverPos = {update.posX, update.posY, update.posZ};
                    Point3F serverVel = {update.velX, update.velY, update.velZ};
                    if (pl) {
                        reconcile(serverPos, serverVel, update.lastMoveSeq);
                        pl->setRotation({update.rotX, 0, update.rotZ});
                    }
                }
            }
        });
    }
}

void Game::reconcile(const Point3F& serverPos, const Point3F& serverVel, uint32_t lastProcessedSeq) {
    if (!pl) return;

    // Pop all moves that were processed by server
    while (!pendingMoves.empty() && pendingMoves.front().seq <= lastProcessedSeq)
        pendingMoves.pop_front();

    // Set player to authoritative server state
    pl->setPosition(serverPos);
    pl->setVelocity(serverVel);

    // Re-apply pending moves to stay ahead of server
    Physics physics;
    for (auto& move : pendingMoves) {
        // Reconstruct InputMove from stored data
        physics.update(pl, move.dt, move.input);
    }
}

static std::string extractMapName(const std::string& missionPath) {
    // Extract base name from paths like:
    // "Missions/Katabatic.mis", "base/missions/Training1.mis", "@vl2/missions.vl2/Katabatic.mis"
    std::string name = missionPath;
    auto slash = name.rfind('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    auto dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    // Remove trailing whitespace
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    return name;
}

// ─── Demo ghost shape mapping ─────────────────────────────────
// Maps T2 ghost class names to DTS/GLB shape file paths
static const char* shapePathForClass(const std::string& className, const std::string& skinName) {
    if (className == "Player" || className == "MPB") {
        if (skinName.find("medium") != std::string::npos ||
            skinName.find("Medium") != std::string::npos)
            return "shapes/bioderm_medium.dts";
        if (skinName.find("heavy") != std::string::npos ||
            skinName.find("Heavy") != std::string::npos)
            return "shapes/bioderm_heavy.dts";
        return "shapes/bioderm_light.dts";
    }
    if (className == "FlyingVehicle" || className == "Shrike")
        return "shapes/vehicle_air_scout.dts";
    if (className == "HoverVehicle" || className == "Turbograv" || className == "Wildcat")
        return "shapes/vehicle_grav_scout.dts";
    if (className == "WheeledVehicle" || className == "Shield" || className == "Vehicle")
        return "shapes/vehicle_land_mpbase.dts";
    if (className == "Item" || className == "Mine")
        return "shapes/deploy_inventory.dts";
    if (className == "Turret" || className == "Sentry")
        return "shapes/turret_sentry.dts";
    if (className == "Sensor")
        return "shapes/deploy_sensor_pulse.dts";
    if (className == "Camera")
        return "shapes/camera.dts";
    if (className == "BeaconObject")
        return "shapes/beacon.dts";
    if (className == "Debris")
        return "shapes/debris_generic.dts";
    if (className == "Generator")
        return "shapes/station_generator_large.dts";
    if (className == "ForceFieldBare")
        return "shapes/station_inv_human.dts";
    if (className == "Marker" || className == "WayPoint" || className == "SpawnSphere")
        return "shapes/gravemarker_1.dts";
    if (className == "WaterBlock")
        return "shapes/effect_plasma_explosion.dts";
    if (className == "Projectile" || className == "EnergyProjectile")
        return "shapes/energy_bolt.dts";
    if (className == "GrenadeProjectile" || className == "FlareProjectile")
        return "shapes/grenade.dts";
    if (className == "BombProjectile")
        return "shapes/bomb.dts";
    if (className == "Flag")
        return "shapes/flag.dts";
    if (className == "LinearProjectile" || className == "TracerProjectile" ||
        className == "LinearFlareProjectile" || className == "SeekerProjectile" ||
        className == "SniperProjectile" || className == "ShockLanceProjectile")
        return "shapes/energy_bolt.dts";
    if (className == "RepairProjectile" || className == "ELFProjectile")
        return "shapes/energy_bolt.dts";
    if (className == "Splash")
        return "shapes/effect_plasma_explosion.dts";
    // For unknown classes, try a path based on the class name
    return nullptr;
}

// Returns true if a ghost with this class name should be rendered as a 3D model
// (as opposed to being a world-level object already rendered by World::render)
static bool isRenderableGhostClass(const std::string& className) {
    if (className == "InteriorInstance" || className == "StaticShape" ||
        className == "ScopeAlwaysShape" || className == "TSStatic" ||
        className == "TerrainBlock" || className == "Sky" || className == "Sun" ||
        className == "Lightning")
        return false;
    return true;
}

DTSShape* Game::getOrLoadDemoShape(const std::string& className, const std::string& skinName) {
    // Build a cache key that differentiates armor variants for the same class
    std::string cacheKey = className;
    if ((className == "Player" || className == "MPB") && !skinName.empty()) {
        // Include armor type in cache key to avoid mixing light/medium/heavy for same class
        if (skinName.find("medium") != std::string::npos ||
            skinName.find("Medium") != std::string::npos)
            cacheKey = "Player_medium";
        else if (skinName.find("heavy") != std::string::npos ||
                 skinName.find("Heavy") != std::string::npos)
            cacheKey = "Player_heavy";
        else
            cacheKey = "Player_light";
    }

    // Use cached shape if available
    auto it = demoShapeCache.find(cacheKey);
    if (it != demoShapeCache.end())
        return it->second.loaded ? &it->second : nullptr;

    // Find the shape path for this class
    auto& fs = Engine::instance().fs();
    const char* path = shapePathForClass(className, skinName);
    if (!path) {
        // Try class-name-based paths for unknown classes
        std::string lower = className;
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
        std::vector<std::string> candidates = {
            "shapes/" + lower + ".dts",
            "shapes/" + lower + ".glb",
            "shapes/" + lower,
            "shapes/" + className + ".dts",
            "shapes/" + className + ".glb",
        };
        for (auto& c : candidates) {
            auto d = fs.read(c.c_str());
            if (!d.empty()) {
                DTSShape s;
                s.name = className;
                if (s.load(d.data(), d.size())) {
                    auto ins = demoShapeCache.emplace(cacheKey, std::move(s));
                    return ins.first->second.loaded ? &ins.first->second : nullptr;
                }
            }
        }
        // Try generic fallback shapes
        for (auto& f : {"shapes/bioderm_light.glb", "shapes/bomb.dts"}) {
            auto d = fs.read(f);
            if (!d.empty()) {
                DTSShape s;
                s.name = className;
                if (s.load(d.data(), d.size())) {
                    auto ins = demoShapeCache.emplace(cacheKey, std::move(s));
                    return ins.first->second.loaded ? &ins.first->second : nullptr;
                }
            }
        }
        demoShapeCache[cacheKey] = DTSShape{};
        Console::instance().printf(LogLevel::Debug, "Demo: no shape for class '%s'", className.c_str());
        return nullptr;
    }

    // Try loading with auto-detection (DTS or GLB)
    std::vector<uint8_t> data = fs.read(path);
    if (data.empty()) {
        // Try common variations
        std::string base = path;
        auto dot = base.rfind('.');
        if (dot != std::string::npos) base = base.substr(0, dot);
        std::vector<std::string> variants = {
            base,                    // auto-detect
            base + ".dts",
            base + ".glb",
            std::string("shapes/") + className + "/" + className + ".dts",
            std::string("shapes/") + className + "/" + className + ".glb",
        };
        for (auto& v : variants) {
            data = fs.read(v.c_str());
            if (!data.empty()) break;
        }
    }

    if (data.empty()) {
        demoShapeCache[cacheKey] = DTSShape{};
        Console::instance().printf(LogLevel::Debug, "Demo: no shape found for class '%s' (key=%s)", className.c_str(), cacheKey.c_str());
        return nullptr;
    }

    DTSShape shape;
    shape.name = className;
    if (!shape.load(data.data(), data.size())) {
        bool glbLoaded = false;
        // Extract base shape name from the DTS path
        std::string shapeName = path;
        {
            auto dot = shapeName.rfind('.');
            if (dot != std::string::npos) shapeName = shapeName.substr(0, dot);
            auto slash = shapeName.rfind('/');
            if (slash != std::string::npos) shapeName = shapeName.substr(slash + 1);
        }
        // Try several GLB paths
        std::vector<std::string> glbPaths = {
            std::string("@vl2/TR2final105-client.vl2/shapes/TR2") + shapeName + ".glb",
            std::string("@vl2/TR2final105-client.vl2/shapes/") + shapeName + ".glb",
            std::string("shapes/") + shapeName + ".glb",
        };
        for (auto& gp : glbPaths) {
            auto glbData = fs.read(gp.c_str());
            if (!glbData.empty() && shape.load(glbData.data(), glbData.size())) {
                glbLoaded = true;
                break;
            }
        }
        if (!glbLoaded) {
            // On-demand DTS→GLB conversion via Blender (like .cs→.dso compilation)
            std::string glbCachePath = std::string("/tmp/torch_glb_") + shapeName + ".glb";
            // Check if already cached from a previous conversion
            auto cachedGlb = fs.read(glbCachePath.c_str());
            if (!cachedGlb.empty()) {
                if (shape.load(cachedGlb.data(), cachedGlb.size())) glbLoaded = true;
            }
            if (!glbLoaded) {
                // Try running Blender to convert the DTS file
                std::string dtsPath = std::string("shapes/") + shapeName + ".dts";
                auto dtsData = fs.read(dtsPath.c_str());
                if (!dtsData.empty()) {
                    // Write DTS to temp file for Blender
                    std::string tmpDts = std::string("/tmp/torch_convert_") + shapeName + ".dts";
                    FILE* tf = fopen(tmpDts.c_str(), "wb");
                    if (tf) {
                        fwrite(dtsData.data(), 1, dtsData.size(), tf);
                        fclose(tf);
                        // Check if Blender is available
                        int blenderAvail = system("which blender >/dev/null 2>&1");
                        if (blenderAvail == 0) {
                            const char* home = getenv("HOME");
                            std::string homeDir = home ? home : "/tmp";
                            std::string blenderScript = homeDir + "/t2-mapper/scripts/blender/dts2gltf.py";
                            std::string cmd = "blender --background --python \"" + blenderScript + "\" -- \"" + tmpDts + "\" 2>/dev/null";
                            int ret = system(cmd.c_str());
                            if (ret == 0) {
                                // Read the generated GLB (Blender outputs beside the DTS)
                                std::string genGlb = std::string("/tmp/torch_convert_") + shapeName + ".glb";
                                FILE* gf = fopen(genGlb.c_str(), "rb");
                                if (gf) {
                                    fseek(gf, 0, SEEK_END);
                                    size_t gs = ftell(gf);
                                    fseek(gf, 0, SEEK_SET);
                                    std::vector<uint8_t> glbData(gs);
                                    fread(glbData.data(), 1, gs, gf);
                                    fclose(gf);
                                    if (shape.load(glbData.data(), glbData.size())) {
                                        glbLoaded = true;
                                        // Cache for next time
                                        FILE* cf = fopen(glbCachePath.c_str(), "wb");
                                        if (cf) { fwrite(glbData.data(), 1, glbData.size(), cf); fclose(cf); }
                                    }
                                    unlink(genGlb.c_str());
                                }
                            }
                        } else {
                            Console::instance().printf(LogLevel::Debug, "  Blender not found - install Blender + io_scene_dtst3d for auto DTS→GLB conversion");
                        }
                        unlink(tmpDts.c_str());
                    }
                }
            }
            demoShapeCache[cacheKey] = DTSShape{};
            Console::instance().printf(LogLevel::Debug, "Demo: failed to load shape for class '%s'", className.c_str());
            return nullptr;
        }
    }

    Console::instance().printf(LogLevel::Debug, "Demo: loaded shape for '%s' (%zu meshes)",
        path, shape.meshes.size());

    auto inserted = demoShapeCache.emplace(cacheKey, std::move(shape));
    return inserted.first->second.loaded ? &inserted.first->second : nullptr;
}

void Game::playDemo(const char* path) {
    Console::instance().printf(LogLevel::Info, "Loading demo: %s", path);

    // Clean up previous parser
    if (demoParser) { delete demoParser; demoParser = nullptr; }
    demoParser = new DemoParser;

    if (!demoParser->loadFile(path)) {
        Console::instance().printf(LogLevel::Error, "Failed to load demo file");
        delete demoParser; demoParser = nullptr;
        return;
    }

    const auto& hdr = demoParser->getHeader();
    const auto& ib = demoParser->getInitialBlock();

    Console::instance().printf(LogLevel::Info, "  Identity: %s", hdr.identString.c_str());
    Console::instance().printf(LogLevel::Info, "  Protocol: 0x%08X", (unsigned)hdr.protocolVersion);
    Console::instance().printf(LogLevel::Info, "  InitBlock: %u bytes", (unsigned)hdr.initialBlockSize);
    Console::instance().printf(LogLevel::Info, "  Mission: %s", ib.missionName.empty() ? "(unknown)" : ib.missionName.c_str());

    // Try to load the mission terrain for visual playback
    // Fall back to Training1 if mission name is unknown or unavailable
    std::string loadMap;
    if (!ib.missionName.empty()) {
        loadMap = extractMapName(ib.missionName);
        // Verify the mission file actually exists
        if (!loadMap.empty()) {
            auto& fs = Engine::instance().fs();
            std::string testPath = std::string("missions/") + loadMap + ".mis";
            if (!fs.fileExists(testPath.c_str())) {
                loadMap.clear();
            }
        }
    }
    // Try to guess mission from demo filename
    if (loadMap.empty()) {
        std::string fname = path;
        auto slash = fname.rfind('/');
        if (slash != std::string::npos) fname = fname.substr(slash + 1);
        auto dot = fname.rfind('.');
        if (dot != std::string::npos) fname = fname.substr(0, dot);

        // Try: text before first underscore (common: Map_Player_vs_Player.rec)
        std::string candidate;
        auto us = fname.find('_');
        if (us != std::string::npos) candidate = fname.substr(0, us);
        // Try: text after last underscore (common: Player_vs_Player_Map.rec)
        auto lus = fname.rfind('_');
        if (lus != std::string::npos && lus != us) {
            std::string c2 = fname.substr(lus + 1);
            // Prefer shorter (map names are usually shorter than player names)
            if (candidate.empty() || c2.size() < candidate.size()) candidate = c2;
        }
        // Try: last space-separated word
        auto sp = fname.rfind(' ');
        if (sp != std::string::npos) {
            std::string c3 = fname.substr(sp + 1);
            if (candidate.empty() || c3.size() < candidate.size()) candidate = c3;
        }
        if (!candidate.empty()) {
            candidate[0] = (char)toupper((unsigned char)candidate[0]);
            // Match known abbreviations to full mission names
            static const char* abbrevMatch[][2] = {
                {"Kata", "Katabatic"}, {"Mino", "Minotaur"}, {"Pande", "Pandemonium"},
                {"Dessi", "Desiccator"}, {"Boss", "Boss"}, {"BB", "BeachBlitz"},
                {"DX", "DeathBirdsFly"}, {"Drifts", "Drifts"}, {"RC", "Rollercoaster"},
                {"SH", "Stonehenge"}, {"HO", "Haven"}, {"LD", "LastDance"},
                {"DOTA", "DeathOfTheAges"}, {"DBS", "DeathBirdsFly"},
                {"TWL", "TWL"}, {"WO", "Whiteout"}, {"DBF", "DeathBirdsFly"},
                {"Mag", "Magmatic"}, {"Kata", "Katabatic"}, {"Beggars", "BeggarsRun"},
                {"Stone", "Stonehenge"}, {"Slap", "Slapdash"},
                {"Tomb", "Tombstone"}, {"Quag", "Quagmire"},
                {"River", "RiverDance"}, {"Wild", "Wilderzone"},
                {"Sanc", "Sanctuary"}, {"Cine", "Cinerous"},
                {"Feign", "Feign"}, {"Drorck", "Drorck"},
                {"Harp", "Harvester"}, {"Ramp", "Ramparts"},
                {"Dam", "Damnation"}, {"Soul", "SoulFire"},
                {nullptr, nullptr}
            };
            for (int i = 0; abbrevMatch[i][0]; i++) {
                bool match = true;
                for (size_t ci = 0; abbrevMatch[i][0][ci]; ci++) {
                    if (ci >= candidate.size() || tolower((unsigned char)candidate[ci]) != tolower((unsigned char)abbrevMatch[i][0][ci])) { match = false; break; }
                }
                if (match && candidate.size() == strlen(abbrevMatch[i][0])) {
                    candidate = abbrevMatch[i][1];
                    break;
                }
            }
            loadMap = candidate;
        }
    }
    // If extraction gave garbage, try the demo filename
    if (loadMap.empty() || loadMap.find_first_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") == std::string::npos) {
        std::string fname = path;
        auto slash = fname.rfind('/');
        if (slash != std::string::npos) fname = fname.substr(slash + 1);
        auto dot = fname.rfind('.');
        if (dot != std::string::npos) fname = fname.substr(0, dot);
        // Check if filename looks like a mission name (starts with uppercase letter)
        if (!fname.empty() && isalpha((unsigned char)fname[0])) {
            fname[0] = (char)toupper((unsigned char)fname[0]);
            loadMap = fname;
        }
    }
    if (loadMap.empty()) {
        loadMap = "Training1";
        Console::instance().printf(LogLevel::Info, "Unknown mission, defaulting to '%s' for terrain", loadMap.c_str());
    }

    Console::instance().printf(LogLevel::Info, "Loading mission map: %s", loadMap.c_str());
    State prevState = gameState;
    startLocalGame(loadMap.c_str());
    if (gameState != Playing) {
        gameState = prevState;
        Console::instance().printf(LogLevel::Warn, "Mission load failed, playing without terrain");
    }

    // Reset demo path history
    demoPath.clear();
    demoPathCount = 0;
    demoTrails.clear();

    // Reset stats
    demoPacketsParsed = 0;
    demoTime = 0;
    demoEventLog.clear();
    int totalBlocks = demoParser->getBlockCount();
    demoTotalTime = totalBlocks > 0 ? totalBlocks / 250.0f : 1.0f;
    demoBlocksTotal = totalBlocks;
    demoBlocksDone = 0;
    demoFastForward = false; // real-time when invoked from console

    Console::instance().printf(LogLevel::Info, "  Total blocks: %d (est. %.1f seconds)", totalBlocks, demoTotalTime);
    Console::instance().printf(LogLevel::Info, "Demo loaded, starting playback...");
    demoPlaying = true;
    setState(Playing);
}

void Game::applyInput(const InputMove& input) {
    // Edge detection for demo controls (track previous key state)
    static bool prevPauseKey = false;
    static bool prevStepKey = false;
    static bool prevEventKey = false;

    currentInput = input;
    showScoreboard = input.showScoreboard;

    // Demo pause toggle on rising edge of P key
    if (demoPlaying && input.demoPause && !prevPauseKey)
        toggleDemoPause();
    prevPauseKey = input.demoPause;

    // Demo step frame on rising edge of . key
    if (demoPlaying && input.demoStepFrame && !prevStepKey)
        requestDemoStep();
    prevStepKey = input.demoStepFrame;

    // Demo event log toggle on rising edge of E key
    if (demoPlaying && input.demoShowEvents && !prevEventKey)
        toggleDemoEvents();
    prevEventKey = input.demoShowEvents;

    // Spectate cycle on reload key (R) during demo playback
    static bool prevReload = false;
    if (demoPlaying && input.reload && !prevReload) {
        if (demoParser) {
            auto indices = demoParser->getGhostTracker().getAllIndices();
            // Find all Player/MPB class ghosts
            std::vector<int> players;
            for (int i : indices) {
                const GhostEntry* g = demoParser->getGhostTracker().getGhost(i);
                if (g && (g->className == "Player" || g->className == "MPB"))
                    players.push_back(i);
            }
            if (!players.empty()) {
                // Find current spectate index (or control index) in player list
                int current = (spectateGhostIndex >= 0) ? spectateGhostIndex : controlGhostIndex;
                auto it = std::find(players.begin(), players.end(), current);
                if (it != players.end() && ++it != players.end())
                    spectateGhostIndex = *it;
                else
                    spectateGhostIndex = players[0];
                Console::instance().printf(LogLevel::Info, "Spectating ghost %d", spectateGhostIndex);
            }
        }
    }
    prevReload = input.reload;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Shape Viewer — browse all .dts shapes from the data paths
// ═══════════════════════════════════════════════════════════════════════════

#include <filesystem>
namespace fs = std::filesystem;

void Game::enterShapeViewer() {
    shapeViewerFiles.clear();
    shapeViewerIndex = 0;
    shapeViewerActive = true;
    shapeViewerYaw = 0.6f;
    shapeViewerPitch = 0.25f;
    shapeViewerAnimTime = 0;

    // Scan all mounted archives for .dts files
    auto& fsys = Engine::instance().fs();
    std::vector<std::string> allFiles;
    fsys.listFiles(nullptr, allFiles);  // nullptr = list all files

    for (auto& f : allFiles) {
        if (f.size() > 4 && f.rfind(".dts") == f.size() - 4) {
            shapeViewerFiles.push_back(f);
        }
    }

    // Also scan the filesystem data directories directly
    for (auto& baseDir : {"t2-linux/base", "base"}) {
        std::error_code ec;
        if (!fs::is_directory(baseDir, ec)) continue;
        for (auto& entry : fs::recursive_directory_iterator(baseDir, fs::directory_options::skip_permission_denied, ec)) {
            if (!entry.is_regular_file()) continue;
            auto& p = entry.path();
            if (p.extension() == ".dts") {
                std::string rel = p.string();
                for (auto& prefix : {"t2-linux/base/", "base/"}) {
                    auto pos = rel.find(prefix);
                    if (pos != std::string::npos) {
                        rel = rel.substr(pos + strlen(prefix));
                        break;
                    }
                }
                // Deduplicate
                bool dup = false;
                for (auto& existing : shapeViewerFiles)
                    if (existing == rel) { dup = true; break; }
                if (!dup) shapeViewerFiles.push_back(rel);
            }
        }
    }

    // Sort and deduplicate
    std::sort(shapeViewerFiles.begin(), shapeViewerFiles.end());
    shapeViewerFiles.erase(std::unique(shapeViewerFiles.begin(), shapeViewerFiles.end()), shapeViewerFiles.end());

    Console::instance().printf(LogLevel::Info, "Shape Viewer: found %zu .dts files", shapeViewerFiles.size());

    // Start on bioderm if found
    for (int i = 0; i < (int)shapeViewerFiles.size(); i++) {
        if (shapeViewerFiles[i].find("bioderm") != std::string::npos) {
            shapeViewerIndex = i;
            break;
        }
    }

    shapeViewerLoadCurrent();
}

void Game::shapeViewerNext() {
    if (shapeViewerFiles.empty()) return;
    shapeViewerIndex = (shapeViewerIndex + 1) % (int)shapeViewerFiles.size();
    shapeViewerLoadCurrent();
}

void Game::shapeViewerPrev() {
    if (shapeViewerFiles.empty()) return;
    shapeViewerIndex = (shapeViewerIndex - 1 + (int)shapeViewerFiles.size()) % (int)shapeViewerFiles.size();
    shapeViewerLoadCurrent();
}

void Game::shapeViewerLoadCurrent() {
    if (shapeViewerFiles.empty()) return;
    auto& fsys = Engine::instance().fs();
    const std::string& path = shapeViewerFiles[shapeViewerIndex];

    auto data = fsys.read(path.c_str());
    if (data.empty()) {
        Console::instance().printf(LogLevel::Warn, "Shape Viewer: cannot read '%s'", path.c_str());
        return;
    }

    shapeViewerShape = DTSShape{};
    shapeViewerShape.name = path;
    if (!shapeViewerShape.load(data.data(), data.size())) {
        Console::instance().printf(LogLevel::Warn, "Shape Viewer: failed to load '%s'", path.c_str());
        return;
    }

    shapeViewerAnimTime = 0;
    // Reset bounds for camera framing
    shapeViewerBoundsInit = false;

    Console::instance().printf(LogLevel::Info, "Shape Viewer [%d/%zu]: '%s' (%zu meshes, %zu nodes, %zu anims)",
        shapeViewerIndex + 1, (int)shapeViewerFiles.size(), path.c_str(),
        shapeViewerShape.meshes.size(), shapeViewerShape.nodes.size(),
        shapeViewerShape.animations.size());
}
