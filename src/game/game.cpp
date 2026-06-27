#include "game/game.h"
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
#include <unordered_map>

Player::Player() {}
Player::~Player() {}

void Player::update(float dt) {
    // Physics handled by Game::update
}

void Player::render() {
    // Placeholder - render a simple box
    auto& r = Engine::instance().renderer();
    Box3F box = {{pos.x - 0.4f, pos.y - 1.0f, pos.z - 0.4f},
                 {pos.x + 0.4f, pos.y + 1.0f, pos.z + 0.4f}};
    r.drawBox(box, {0, 1, 0, 1});
}

void Player::applyMove(const Point3F& move, bool jump, bool jet) {
    // Movement handled by Physics system
}

void Player::applyDamage(float amount) {
    if (arm > 0) {
        float absorbed = Math::min(arm, amount * 0.6f);
        arm -= absorbed;
        hp -= amount - absorbed;
    } else {
        hp -= amount;
    }
    if (hp <= 0) { hp = 0; /* handle death */ }
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

            // Determine GLB path from shape name
            std::string glbPath;
            size_t dot = shapeName.rfind('.');
            std::string base = (dot != std::string::npos) ? shapeName.substr(0, dot) : shapeName;
            std::string ext = (dot != std::string::npos) ? shapeName.substr(dot) : "";

            if (ext == ".dif") {
                glbPath = "interiors/" + base + ".glb";
                shape.isInterior = true;
            } else {
                glbPath = "shapes/" + base + ".glb";
            }

            auto data = Engine::instance().fs().read(glbPath.c_str());
            if (data.empty() && ext != ".dif") {
                std::string altPath = "interiors/" + base + ".glb";
                data = Engine::instance().fs().read(altPath.c_str());
            }
            // Try stripping TR2 prefix (e.g. TR2heavy_male.dts -> heavy_male.glb)
            if (data.empty() && base.compare(0, 3, "TR2") == 0) {
                std::string stripped = base.substr(3);
                std::string tryPath = (ext == ".dif" ? "interiors/" : "shapes/") + stripped + ".glb";
                data = Engine::instance().fs().read(tryPath.c_str());
            }

            if (!data.empty()) {
                shape.loadGLB(data.data(), data.size());
            }

            if (shape.loaded) {
                Console::instance().printf(LogLevel::Debug, "  loaded shape: %s (%zu meshes, %zu textures)", shapeName.c_str(), shape.meshes.size(), shape.materialTextures.size());
            } else {
                Console::instance().printf(LogLevel::Debug, "  shape not found: %s", shapeName.c_str());
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
    } else {
        Console::instance().printf(LogLevel::Warn, "No mission file found for '%s', using procedural terrain", mapName);
    }

    // Generate terrain if not loaded
    if (!terrainBlock.loaded) {
        terrainBlock.load(nullptr, 0);
    }

    // Load sky from mission materialList
    std::vector<std::string> skyFaces;
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
            // Parse first 6 lines as cubemap face texture names
            std::vector<std::string> faceNames;
            size_t pos = 0;
            while (pos < dmlContent.size() && faceNames.size() < 6) {
                while (pos < dmlContent.size() && (dmlContent[pos] == ' ' || dmlContent[pos] == '\t' || dmlContent[pos] == '\r')) pos++;
                if (pos >= dmlContent.size()) break;
                size_t end = pos;
                while (end < dmlContent.size() && dmlContent[end] != '\n') end++;
                std::string line = dmlContent.substr(pos, end - pos);
                while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
                if (!line.empty()) faceNames.push_back(line);
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
    } else {
        Console::instance().printf(LogLevel::Info, "No sky textures found, generating default");
    }

    loaded = true;
    Console::instance().printf(LogLevel::Info, "Map loaded: %s", mapName);
    return true;
}

void World::update(float dt) {
    // Update world objects, animations, etc.
}

void World::render(const Point3F& cameraPos) {
    if (!loaded) return;

    auto& r = Engine::instance().renderer();

    // Render terrain
    if (terrainBlock.loaded) {
        ShaderManager::getTerrainShader()->bind();
        terrainBlock.render(cameraPos);
    }

    // Render world objects
    ShaderManager::getDefaultShader()->bind();
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
            obj.shape->render(0);
        }
    }

    // Render sky
    skyBox.render(r.view, r.projection);
}

void World::addObject(const WorldObject& obj) {
    worldObjects.push_back(obj);
}

float World::getHeight(float x, float z) const {
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
        // Update physics
        Physics physics;
        physics.update(pl, dt, currentInput);

        // Update world
        w->update(dt);

        // Update audio listener from player camera
        auto& audio = Engine::instance().audio();
        if (audio.config().enabled) {
            Point3F camPos = pl->cameraPos();
            Point3F camTarget = pl->cameraTarget();
            Point3F forward = {camTarget.x - camPos.x, camTarget.y - camPos.y, camTarget.z - camPos.z};
            float flen = std::sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
            if (flen > 0.0001f) { forward.x /= flen; forward.y /= flen; forward.z /= flen; }
            Point3F up = {0, 1, 0};
            audio.update(camPos, pl->velocity(), forward, up);
        }
    }
}

void Game::render(float dt) {
    if (gameState != Playing && gameState != Dead) return;

    auto& r = Engine::instance().renderer();
    r.beginFrame({0.3f, 0.5f, 0.8f, 1.0f});

    r.setCamera(pl->cameraPos(), pl->cameraTarget(), {0, 1, 0});
    w->render(pl->position());
    pl->render();

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
    Connection* conn = net.createConnection();
    if (conn->connect(host, port)) {
        conn->setConnectCallback([this](bool success) {
            if (success) {
                setState(Playing);
                Console::instance().printf(LogLevel::Info, "Connected!");
            } else {
                setState(Menu);
                Console::instance().printf(LogLevel::Info, "Connection failed");
            }
        });
    }
}

void Game::applyInput(const InputMove& input) {
    currentInput = input;
}
