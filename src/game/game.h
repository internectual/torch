#pragma once
#include "core/math.h"
#include "audio/audio_system.h"
#include "render/renderer.h"
#include "game/collision.h"
#include "game/weapon.h"
#include "net/protocol.h"
#include "game/demo.h"
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

    enum AnimState { Stand, Run, Jump, Jet, Death };

    void update(float dt);
    void render();

    Point3F position() const { return pos; }
    Point3F rotation() const { return rot; }
    Point3F velocity() const { return vel; }

    void setPosition(const Point3F& p) { pos = p; }
    void setRotation(const Point3F& r) { rot = r; }
    void setEnergy(float e) { eng = e; }
    void setVelocity(const Point3F& v) { vel = v; }
    void setOnGround(bool g) { onGround = g; }

    void applyMove(const Point3F& move, bool jump, bool jet);
    void applyDamage(float amount);
    void respawn();

    float health() const { return hp; }
    float energy() const { return eng; }
    float armor() const { return arm; }
    bool isDead() const { return hp <= 0; }
    bool isOnGround() const { return onGround; }
    AnimState animState() const { return anim; }

    // Camera
    Point3F cameraPos() const;
    Point3F cameraTarget() const;

    // Weapons
    void selectWeapon(int32_t idx);
    void fireWeapon(bool alt);
    int32_t currentWeapon() const { return curWeapon; }
    const Weapon& weapon(int32_t idx) const { return weapons[idx]; }
    Weapon& weapon(int32_t idx) { return weapons[idx]; }
    int32_t weaponCount() const { return (int32_t)weapons.size(); }
    void weaponCycle(int32_t dir);

    // HUD state
    int32_t kills = 0;
    int32_t deaths = 0;
    float score = 0.0f;

    // Animation
    void updateAnimation(float dt, bool jetting);

    // Player model
    DTSShape modelShape;
    bool modelLoaded = false;

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
    AnimState anim = Stand;
    float animTime = 0;

    // Weapons
    std::vector<Weapon> weapons;
    int32_t curWeapon = 0;
    float fireCooldown = 0;

    void loadModel();
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
        std::string animName; // empty = static render; non-empty = play this animation
        float animTime = 0;
    };

    void addObject(const WorldObject& obj);
    const std::vector<WorldObject>& objects() const { return worldObjects; }

    float getHeight(float x, float z) const;
    const CollisionMesh& collision() const { return interiorCollision; }
    std::vector<Projectile>& projectiles() { return projList; }
    void spawnProjectile(const Projectile& p);

private:
    TerrainBlock terrainBlock;
    Sky skyBox;
    CollisionMesh interiorCollision;
    std::vector<WorldObject> worldObjects;
    std::vector<DTSShape> shapes;
    std::vector<Projectile> projList;

    struct Explosion {
        Point3F pos;
        float lifetime;
        float maxLifetime;
        float radius;
        ColorF color;
    };
    std::vector<Explosion> explosions;

    struct ItemPickup {
        Point3F pos;
        enum Type { Health, Energy, Ammo } type;
        float respawnTimer;
        bool active;
    };
    std::vector<ItemPickup> items;

    struct FogParams {
        bool enabled = true;
        ColorF color{0.75f, 0.8f, 0.85f, 1.0f}; // matches gradient sky horizon
        float density = 0.0015f;  // gentle fade
        float distance = 666.0f;
    };
    FogParams fog;

    // Sun lighting from mission
    Point3F sunLightDir{0.5f, 0.8f, 0.6f};
    ColorF sunColor{1, 1, 1, 1};
    bool sunLightDirUsed = false;
    bool sunColorUsed = false;

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
    void playDemo(const char* path);

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
    void togglePauseGame() { gamePaused = !gamePaused; }
    bool isGamePaused() const { return gamePaused; }

    bool isRunning() const { return gameState != Menu; }
    float gameTime() const { return time; }

    // Input handling
    struct InputMove {
        bool forward{}, backward{}, left{}, right{};
        bool jump{}, jet{}, fire{}, altFire{};
        bool zoom{}, reload{};
        bool freeCam{}, orbitCam{}, showScoreboard{};
        bool demoPause{}, demoStepFrame{}, demoShowEvents{};
        Point3F lookDelta{};
    };

    void applyInput(const InputMove& input);
    GameServer& gameServer() { return server; }
    bool scoreboardShown() const { return showScoreboard; }
    bool isDemoPlaying() const { return demoPlaying; }
    bool isDemoPaused() const { return demoPaused; }
    bool isDemoFastForward() const { return demoFastForward || demoJetHeld; }
    float getDemoTime() const { return demoTime; }
    float getDemoTotalTime() const { return demoTotalTime; }
    bool demoHasPosition() const { return demoHasPos; }
    int getControlGhostIndex() const { return controlGhostIndex; }
    float getDamageFlash() const { return damageFlash; }
    float getWhiteOut() const { return whiteOut; }
    void toggleDemoPause() { demoPaused = !demoPaused; }
    void toggleDemoEvents() { demoShowEvents = !demoShowEvents; }
    bool demoEventsShown() const { return demoShowEvents; }
    bool demoOrbitCamActive() const { return demoOrbitCam; }
    const std::vector<DemoTimedEvent>& getDemoEventLog() const { return demoEventLog; }
    void requestDemoStep() { demoStepRequest = true; }
    const std::vector<Point3F>& getDemoPath() const { return demoPath; }
    void setDemoFastForward(bool v) { demoFastForward = v; }
    DemoParser* getDemoParser() const { return demoParser; }
    int getDemoBlocksDone() const { return demoBlocksDone; }
    int getDemoBlocksTotal() const { return demoBlocksTotal; }
    DTSShape* getOrLoadDemoShape(const std::string& className, const std::string& skinName = "");

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
    bool freeCamActive = false;
    bool showScoreboard = false;
    Point3F freeCamPos{0, 10, 0};
    Point3F freeCamTarget{0, 10, -1};
    Point3F freeCamRot{0, 0, 0};
    GameServer server;
    Connection* activeConn{};

    // Demo playback
    DemoParser* demoParser{};
    bool demoPlaying = false;
    bool gamePaused = false;
    bool demoPaused = false;
    bool demoStepRequest = false;
    bool demoJetHeld = false; // Space key held (fast-forward indicator)
    std::unordered_map<std::string, DTSShape> demoShapeCache;
    DTSShape testShape;
    bool testShapeLoaded = false;
    bool demoFastForward = true;
    float demoTime = 0;
    float demoTotalTime = 0;
    int demoPacketsParsed = 0;
    int demoBlocksTotal = 0;
    int demoBlocksDone = 0;
    Point3F demoCameraPos{0, 5, 0};
    Point3F demoCameraTarget{0, 5, -1};
    Point3F demoPrevCameraPos{0, 5, 0};
    Point3F demoPrevCameraTarget{0, 5, -1};
    float demoMoveBlend = 1.0f;
    bool demoHasPos = false;
    int controlGhostIndex = -1;  // control object ghost index during demo
    int spectateGhostIndex = -1; // spectating a specific ghost (-1 = follow control object)
    float damageFlash = -1.0f;  // red screen flash during demo playback
    float whiteOut = -1.0f;     // white screen flash during demo playback
    float shakeIntensity = 0.0f; // camera shake for explosions
    Point3F shakeOffset{0,0,0};
    std::vector<DemoTimedEvent> demoEventLog;
    bool demoShowEvents = true;
    // Orbit camera for demo spectator mode
    bool demoOrbitCam = true;
    float orbitAngle = 0;
    float orbitDistance = 300.0f;
    float orbitHeight = 150.0f;
    Point3F orbitCenter{};
    bool orbitCenterInit = false;
    std::vector<Point3F> demoPath;
    int demoPathCount = 0;

    // Projectile trail system
    struct TrailPoint { float x, y, z; float life; };
    std::map<int, std::vector<TrailPoint>> demoTrails;
};
