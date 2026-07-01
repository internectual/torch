#include "game/game.h"
#include <GL/glew.h>
#include "game/demo.h"
#include "game/physics.h"
#include "game/mission_parser.h"
#include "render/renderer.h"
#include "render/shader.h"
#include "render/glb_loader.h"
#include "core/console.h"
#include "core/engine.h"
#include "fs/file_system.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <unistd.h>
#include <algorithm>
#include <unordered_map>

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
    (void)dt;
}

void Player::loadModel() {
    if (modelLoaded) return;

    auto& fs = Engine::instance().fs();
    std::vector<std::string> paths = {
        "shapes/player/armor/light/light.dts",
        "shapes/player/armor/light/light.glb",
        "shapes/player/armor/light/light",
        "shapes/player/armor/medium/medium.dts",
        "shapes/player/armor/medium/medium.glb",
        "shapes/player/armor/medium/medium",
    };
    for (auto& p : paths) {
        auto data = fs.read(p.c_str());
        if (!data.empty()) {
            modelShape.name = p;
            if (modelShape.load(data.data(), data.size())) {
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
        r.setModel(model);

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
    // Movement handled by Physics system
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
    auto misData = fs.readText(misPath.c_str());

    if (misData.empty()) {
        // Try alternative path
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
        }
    }

    // Remove inactive projectiles
    projList.erase(
        std::remove_if(projList.begin(), projList.end(),
            [](const Projectile& p) { return !p.active || p.hasImpacted; }),
        projList.end()
    );

    // Advance animation time for world objects
    for (auto& obj : worldObjects) {
        if (!obj.animName.empty() && obj.shape && obj.shape->loaded)
            obj.animTime += dt;
    }
}

void World::render(const Point3F& cameraPos) {
    if (!loaded) return;

    auto& r = Engine::instance().renderer();

    // Render terrain
    if (terrainBlock.loaded) {
        ShaderManager::getTerrainShader()->bind();
        terrainBlock.render(cameraPos, fog.enabled, fog.color, fog.density, sunLightDirUsed ? &sunLightDir : nullptr);
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

    // Apply sun lighting direction from mission data
    if (sunLightDirUsed) {
        defShader->setUniform("uLightDir", sunLightDir);
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
            r.setModel(model);
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

    // Render projectiles
    float projSize = 0.15f;
    ColorF discColor = {1.0f, 0.6f, 0.1f, 1.0f};
    ColorF boltColor = {0.2f, 0.8f, 1.0f, 1.0f};
    ColorF grenadeColor = {0.3f, 1.0f, 0.3f, 1.0f};
    for (auto& p : projList) {
        if (!p.active) continue;
        ColorF col;
        switch (p.type) {
            case ProjectileType::Disc:    col = discColor;    break;
            case ProjectileType::Bolt:    col = boltColor;    break;
            case ProjectileType::Grenade:
            case ProjectileType::Mortar:  col = grenadeColor; break;
            default:                      col = {1,1,1,1};    break;
        }
        Box3F box = {{p.pos.x - projSize, p.pos.y - projSize, p.pos.z - projSize},
                     {p.pos.x + projSize, p.pos.y + projSize, p.pos.z + projSize}};
        r.drawBox(box, col);
    }

    // Render explosions
    for (auto& e : explosions) {
        float t = e.lifetime / e.maxLifetime;
        float alpha = t;
        float size = e.radius * (1.0f + (1.0f - t) * 2.0f);
        Box3F box = {{e.pos.x - size, e.pos.y - size, e.pos.z - size},
                     {e.pos.x + size, e.pos.y + size, e.pos.z + size}};
        ColorF col = {e.color.r, e.color.g, e.color.b, alpha * 0.5f};
        r.drawBox(box, col);
    }

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

    int tx = Math::clamp((int)((x - terrainBlock.worldOffset.x) / terrainBlock.squareSize), 0, terrainBlock.size - 1);
    int tz = Math::clamp((int)((z - terrainBlock.worldOffset.z) / terrainBlock.squareSize), 0, terrainBlock.size - 1);
    return terrainBlock.heights[tz * terrainBlock.size + tx] * terrainBlock.heightScale;
}

Game::Game() : pl(new Player), w(new World) {}
Game::~Game() { delete pl; delete w; }

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
        server.start(port);
    });

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

    return true;
}

void Game::shutdown() {
    auto& audio = Engine::instance().audio();
    if (ambientSource) audio.releaseSource(ambientSource);
    ambientSource = nullptr;
    ambientSound = nullptr;
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
            int blocksThisFrame;
            if (demoStepRequest) {
                blocksThisFrame = 1;
                demoStepRequest = false;
            } else if (demoFastForward || currentInput.jet) {
                demoJetHeld = currentInput.jet;
                blocksThisFrame = (int)(demoBlocksTotal * dt / demoTotalTime);
            } else {
                demoJetHeld = false;
                // Match real-time: catch up to target position
                int targetDone = (int)(demoTime / demoTotalTime * demoBlocksTotal);
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
                    setState(Menu);
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
                    // Store control object ghost index for highlight
                    if (pd.gameState.controlObjectGhostIndex >= 0)
                        controlGhostIndex = pd.gameState.controlObjectGhostIndex;
                }
                delete block;
            }

            // Debug: ghost stats every ~500 blocks
            if (demoPlaying && demoParser && (demoBlocksDone % 500) == 0 && demoBlocksDone > 0) {
                const GhostTracker& gt = demoParser->getGhostTracker();
                Console::instance().printf(LogLevel::Warn, "Blocks: %d, Ghosts: %d", demoBlocksDone, gt.size());
                if (gt.size() > 0) {
                    int withPos = 0;
                    for (int i : gt.getAllIndices()) {
                        auto* g = gt.getGhost(i);
                        if (g) {
                            if (g->position.x != 0 || g->position.y != 0 || g->position.z != 0) {
                                withPos++;
                                Console::instance().printf(LogLevel::Warn, "  HAS POS: Ghost[%d] class=%d '%s' pos=(%.1f %.1f %.1f)",
                                    i, g->classId, g->className.c_str(), g->position.x, g->position.y, g->position.z);
                            }
                        }
                    }
                    Console::instance().printf(LogLevel::Warn, "Ghosts: %d total, %d with pos", gt.size(), withPos);
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

            // Send move to server if connected
            if (cfg.online && activeConn && activeConn->state() >= Connection::Connected) {
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
                                (currentInput.fire ? 8 : 0);
                moveMsg.lookX = currentInput.lookDelta.x;
                moveMsg.lookY = currentInput.lookDelta.y;

                uint8_t buf[64];
                buf[0] = T2Protocol::GDT_Move;
                T2Protocol::encodeMove(buf + 1, sizeof(buf) - 1, moveMsg);
                activeConn->sendGamePacket(buf, 34, false);
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
        // Respawn after delay
        static float deathTimer = 0;
        deathTimer += dt;
        if (deathTimer > 3.0f) {
            deathTimer = 0;
            pl->respawn();
            setState(Playing);
        }
    }
}

void Game::render(float dt) {
    if (gameState != Playing && gameState != Dead) return;

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
            // Initialize orbit center from the first demo position
            if (!orbitCenterInit) {
                orbitCenter = demoCameraPos;
                orbitCenterInit = true;
            }
            // Smoothly track the action: orbit center follows demo camera
            float trackSpeed = 0.02f;
            orbitCenter.x += (demoCameraPos.x - orbitCenter.x) * trackSpeed;
            orbitCenter.y += (demoCameraPos.y + 20.0f - orbitCenter.y) * trackSpeed;
            orbitCenter.z += (demoCameraPos.z - orbitCenter.z) * trackSpeed;
            float rad = orbitAngle * (3.14159f / 180.0f);
            camPos.x = orbitCenter.x + sinf(rad) * orbitDistance;
            camPos.z = orbitCenter.z + cosf(rad) * orbitDistance;
            camPos.y = orbitCenter.y + orbitHeight;
            camTarget = orbitCenter;
            camTarget.y += 10.0f; // look slightly above center
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
    } else {
        camPos = pl->cameraPos();
        camTarget = pl->cameraTarget();
    }
    r.setCamera(camPos, camTarget, {0, 1, 0});
    w->render(camPos);
    if (!freeCamActive && !demoPlaying) pl->render();

    // Render test shape (loaded via testshape command)
    if (testShapeLoaded && testShape.loaded) {
        MatrixF model;
        model.identity();
        model.setTranslation({0, 0, -5});
        r.setModel(model);
        bool animated = false;
        if (!testShape.animations.empty()) {
            static float animTime = 0;
            animTime += dt;
            const auto& anim = testShape.animations[0];
            testShape.renderAnimation(anim.name.c_str(), fmodf(animTime, anim.duration));
            animated = true;
        }
        if (!animated) testShape.render(0);
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
                r.setModel(model);

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

    // 3D demo path trail
    if (demoPlaying && demoPathCount > 1) {
        // Full path in dim green
        r.drawLineStrip(demoPath, {0.2f, 0.8f, 0.2f, 0.6f});
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
        // Try loading missions in order - first one that exists wins
        const char* mapsToTry[] = {
            "Training1", "Training2", "Test",
            "Katabatic", "Damnation", "Rasp",
            "Oasis", "Gauntlet", "Desiccator",
            "Crater71", "Haven", "Tombstone",
            "Whiteout", "SolsDescent", "TreasureIsland",
            nullptr
        };

        // Try to find the first available mission from VL2 archives
        for (int i = 0; mapsToTry[i]; i++) {
            std::string p = std::string("missions/") + mapsToTry[i] + ".mis";
            auto test = Engine::instance().fs().read(p.c_str());
            if (!test.empty()) {
                missionPath = mapsToTry[i];
                Console::instance().printf(LogLevel::Info, "Found mission: %s", p.c_str());
                break;
            }
        }

        if (missionPath.empty()) {
            Console::instance().printf(LogLevel::Warn, "No .mis files found in archives, trying extracted paths");
            for (int i = 0; mapsToTry[i]; i++) {
                std::string p = std::string("@vl2/missions.vl2/") + mapsToTry[i] + ".mis";
                auto test = Engine::instance().fs().read(p.c_str());
                if (!test.empty()) {
                    missionPath = mapsToTry[i];
                    break;
                }
            }
        }

        if (missionPath.empty()) {
            missionPath = "Katabatic";
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
        if (spawnPos.y < h) spawnPos.y = h + 2.0f;
        pl->setPosition(spawnPos);
        setState(Playing);
        Console::instance().printf(LogLevel::Info, "Game started on '%s'", missionPath.c_str());

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
        setState(Menu);
    }
}

void Game::connectToServer(const char* host, uint16_t port) {
    cfg.serverHost = host;
    cfg.serverPort = port;
    cfg.online = true;

    Console::instance().printf(LogLevel::Info, "Connecting to %s:%d...", host, port);

    auto& net = Engine::instance().network();
    activeConn = net.createConnection();
    if (activeConn->connect(host, port)) {
        activeConn->setConnectCallback([this](bool success) {
            if (success) {
                Console::instance().printf(LogLevel::Info, "Connected!");
            } else {
                Console::instance().printf(LogLevel::Info, "Connection failed");
                setState(Menu);
            }
        });

        // Handle incoming packets
        activeConn->setPacketCallback([this](PacketType type, const uint8_t* data, size_t size) {
            if (type == PacketType::ConnectOK) {
                activeConn->setState(Connection::Connected);
                Console::instance().printf(LogLevel::Info, "Connection established, entering game");
                startLocalGame();
            } else if (type == PacketType::GameData && size > 0) {
                T2Protocol::UpdateMessage update;
                if (T2Protocol::decodeUpdate(data, size, update)) {
                    // Apply server update to local player
                    Point3F serverPos = {update.posX, update.posY, update.posZ};
                    if (pl) {
                        pl->setPosition(serverPos);
                        pl->setRotation({update.rotX, 0, update.rotZ});
                        // Health and energy are authoritative from server
                    }
                }
            }
        });
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
            return "shapes/medium_male.dts";
        if (skinName.find("heavy") != std::string::npos ||
            skinName.find("Heavy") != std::string::npos)
            return "shapes/heavy_male.dts";
        if (skinName.find("light") != std::string::npos ||
            skinName.find("Light") != std::string::npos)
            return "shapes/light_male.dts";
        return "shapes/light_male.dts";
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
        std::vector<std::string> defaultPaths = {
            "shapes/light_male.glb",
            "@vl2/TR2final105-client.vl2/shapes/TR2light_male.glb",
        };
        for (auto& dp : defaultPaths) {
            auto glbData = fs.read(dp.c_str());
            if (!glbData.empty()) {
                DTSShape shape;
                shape.name = className;
                if (shape.load(glbData.data(), glbData.size())) {
                    auto inserted = demoShapeCache.emplace(cacheKey, std::move(shape));
                    return inserted.first->second.loaded ? &inserted.first->second : nullptr;
                }
            }
        }
        demoShapeCache[cacheKey] = DTSShape{};
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
                            std::string blenderScript = std::string(getenv("HOME")) + "/t2-mapper/scripts/blender/dts2gltf.py";
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
    if (!ib.missionName.empty()) {
        std::string mapName = extractMapName(ib.missionName);
        if (!mapName.empty()) {
            Console::instance().printf(LogLevel::Info, "Loading mission map: %s", mapName.c_str());
            // Store current state before mission load
            State prevState = gameState;
            startLocalGame(mapName.c_str());
            // If mission loaded (Playing), keep it; restore otherwise
            if (gameState != Playing) {
                gameState = prevState;
                Console::instance().printf(LogLevel::Warn, "Mission load failed, playing without terrain");
            }
        }
    } else {
        Console::instance().printf(LogLevel::Warn, "No mission name in demo, playing without terrain");
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
    demoTotalTime = totalBlocks / 250.0f;
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
}
