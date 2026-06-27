#pragma once
#include "core/math.h"
#include "audio/audio_system.h"
#include "render/renderer.h"
#include <vector>
#include <string>
#include <unordered_map>

class Game;

struct GameConfig {
    std::string playerName = "Player";
    std::string serverHost = "localhost";
    uint16_t serverPort = 28000;
    bool online = false;
    bool dedicated = false;
    float moveSpeed = 10.0f;
    float jumpSpeed = 8.0f;
    float jetSpeed = 12.0f;
};

class Player {
public:
    Player();
    ~Player();

    void update(float dt);
    void render();

    Point3F position() const { return pos; }
    Point3F rotation() const { return rot; }
    Point3F velocity() const { return vel; }

    void setPosition(const Point3F& p) { pos = p; }
    void setRotation(const Point3F& r) { rot = r; }

    void applyMove(const Point3F& move, bool jump, bool jet);
    void applyDamage(float amount);
    void respawn();

    float health() const { return hp; }
    float energy() const { return eng; }
    float armor() const { return arm; }

    // Camera
    Point3F cameraPos() const;
    Point3F cameraTarget() const;

    // HUD state
    int32_t kills = 0;
    int32_t deaths = 0;
    float score = 0.0f;

private:
    Point3F pos{0, 5, 0};
    Point3F rot{0, 0, 0};
    Point3F vel{0, 0, 0};
    float hp = 100.0f;
    float eng = 100.0f;
    float arm = 0.0f;
    bool onGround = true;
    float eyeHeight = 1.5f;
    float radius = 0.5f;
};

class World {
public:
    World();
    ~World();

    bool load(const char* mapName);
    void update(float dt);
    void render(const Point3F& cameraPos);

    TerrainBlock* terrain() { return &terrainBlock; }
    Sky* sky() { return &skyBox; }
    const Point3F& spawnPoint() const { return playerSpawn; }

    // Object management
    struct WorldObject {
        Point3F pos;
        Point3F rot; // raw axis-angle: (axisX, axisY, axisZ, angle) but angle stored in angleDeg
        float rotAngleDeg = 0;
        Point3F scale{1,1,1};
        std::string shapeName;
        DTSShape* shape{};
        bool collidable = true;
    };

    void addObject(const WorldObject& obj);
    const std::vector<WorldObject>& objects() const { return worldObjects; }

    float getHeight(float x, float z) const;

private:
    TerrainBlock terrainBlock;
    Sky skyBox;
    std::vector<WorldObject> worldObjects;
    std::vector<DTSShape> shapes;
    std::string skyMaterialList;
    Point3F playerSpawn{0, 5, 0};
    bool loaded = false;
};

class Game {
public:
    Game();
    ~Game();

    bool init();
    void shutdown();

    void update(float dt);
    void render(float dt);

    void startLocalGame(const char* map = nullptr);
    void connectToServer(const char* host, uint16_t port);

    GameConfig& config() { return cfg; }
    Player& player() { return *pl; }
    World& world() { return *w; }

    enum State {
        Menu,
        Loading,
        Playing,
        Dead,
        Scoreboard
    };

    State state() const { return gameState; }
    void setState(State s) { gameState = s; }

    bool isRunning() const { return gameState != Menu; }
    float gameTime() const { return time; }

    // Input handling
    struct InputMove {
        bool forward{}, backward{}, left{}, right{};
        bool jump{}, jet{}, fire{}, altFire{};
        bool zoom{}, reload{};
        Point3F lookDelta{};
    };

    void applyInput(const InputMove& input);

private:
    GameConfig cfg;
    Player* pl{};
    World* w{};
    State gameState = Menu;
    float time = 0;
    SoundSource* ambientSource{};
    SoundBuffer* ambientSound{};
    int32_t weatherType = 0; // 0=dry, 1=cold, 2=wet
    InputMove currentInput;
};
