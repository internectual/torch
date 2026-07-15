#pragma once
#include "core/math.h"
#include "audio/audio_system.h"
#include "render/renderer.h"
#include "game/collision.h"
#include "game/weapon.h"
#include "net/protocol.h"
#include "game/demo.h"
#include "game/hud.h"
#include <vector>
#include <string>
#include <deque>
#include <unordered_map>

class Menu;
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
    // Terrain-only load (no shapes/materials) — safe for headless dedicated servers
    // that only need authoritative ground heights for collision.
    bool loadTerrain(const char* mapName);
    void update(float dt);
    void render(const Point3F& cameraPos);

    TerrainBlock* terrain() { return &terrainBlock; }
    Sky* sky() { return &skyBox; }
    const Point3F& spawnPoint() const { return playerSpawn; }
    float waterLevel() const { return water.active ? water.level : 0.0f; }

    // Sun lighting from mission (public for demo playback override)
    Point3F sunLightDir{0.5f, 0.8f, 0.6f};
    ColorF sunColor{1, 1, 1, 1};
    bool sunLightDirUsed = false;
    bool sunColorUsed = false;

    // Fog from mission
    struct FogParams {
        bool enabled = true;
        ColorF color{0.75f, 0.8f, 0.85f, 1.0f};
        float density = 0.0015f;
        float distance = 666.0f;
    };
    FogParams fog;

    // Sky material list
    std::string skyMaterialList;

    // Datablock class name -> shape file path mapping (from .mis)
    std::unordered_map<std::string, std::string> datablockShapeMap;

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

    // Particle system (public for demo playback)
    struct Particle {
        Point3F pos;
        Point3F vel;
        float lifetime;
        float maxLifetime;
        float size;
        ColorF color;
        bool active = false;
    };
    std::vector<Particle> particles;
    void spawnExplosion(const Point3F& pos, const ColorF& color, float radius = 2.0f, int count = 20);
    void spawnTrail(const Point3F& pos, const ColorF& color, float size = 0.2f);
    void updateParticles(float dt);
    void renderParticles();

private:
    TerrainBlock terrainBlock;
    Sky skyBox;
    CollisionMesh interiorCollision;
    std::vector<WorldObject> worldObjects;
    std::vector<DTSShape> shapes;
    std::vector<Projectile> projList;

    // Temp explosion struct for API compatibility
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

    Point3F playerSpawn{0, 5, 0};
    bool loaded = false;

    // Precipitation system
    struct PrecipitationDrop {
        Point3F pos;
        Point3F vel;
        bool active = false;
    };
    struct PrecipitationState {
        int numDrops = 1024;
        float boxWidth = 200.0f;
        float boxHeight = 100.0f;
        float dropSize = 0.5f;
        float minSpeed = 1.5f;
        float maxSpeed = 2.0f;
        bool followCam = true;
        bool active = false;
        std::vector<PrecipitationDrop> drops;
    };
    PrecipitationState precipitation;
    void initPrecipitation(const PrecipitationState& state);
    void updatePrecipitation(float dt, const Point3F& camPos);
    void renderPrecipitation();

    // Water rendering
    struct WaterState {
        bool active = false;
        float level = 0.0f;           // Y position of water surface
        ColorF surfaceColor{0.1f, 0.3f, 0.6f, 0.5f};
        float opacity = 0.5f;
        float waveSpeed = 0.5f;
        float waveMagnitude = 0.15f;
        float size = 2048.0f;         // Coverage area
    };
    WaterState water;
    void renderWater();

    // Bots (simple AI targets)
    struct Bot {
        Point3F pos{0, 5, 0};
        Point3F startPos{0, 5, 0};
        float health = 100.0f;
        bool alive = true;
        float respawnTimer = 0;
        DTSShape* shape{};
        float patrolOffset = 0;
        float moveYaw = 0;
        float animTime = 0;
        float lastHitTime = -10.0f; // time of last damage (-10 = never)
    };
public:
    std::vector<Bot> bots;
    void spawnBots(int count);
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
    bool isTestShapeLoaded() const { return testShapeLoaded; }

    enum State {
        MenuScreen,
        Loading,
        Playing,
        Dead,
        Scoreboard
    };

    State state() const { return gameState; }
    void setState(State s) { gameState = s; }
    Connection* activeConnection() const { return activeConn; }
    void togglePauseGame() { gamePaused = !gamePaused; }
    bool isGamePaused() const { return gamePaused; }

    bool isRunning() const { return gameState != MenuScreen; }
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

    // Client-side prediction
    struct StoredMove {
        uint32_t seq;
        InputMove input;
        float dt;
    };
    uint32_t moveSeq = 0;
    std::deque<StoredMove> pendingMoves;
    void reconcile(const Point3F& serverPos, const Point3F& serverVel, uint32_t lastProcessedSeq);

    bool scoreboardShown() const { return showScoreboard; }
    bool isDemoPlaying() const { return demoPlaying; }
    bool isDemoPaused() const { return demoPaused; }
    bool isDemoFastForward() const { return demoFastForward || demoJetHeld; }
    float getDemoTime() const { return demoTime; }
    float getDemoTotalTime() const { return demoTotalTime; }
    bool demoHasPosition() const { return demoHasPos; }
    int getControlGhostIndex() const { return controlGhostIndex; }
    int getSpectateGhostIndex() const { return spectateGhostIndex; }
    float getDamageFlash() const { return damageFlash; }
    float getWhiteOut() const { return whiteOut; }
    bool isUnderwater() const {
        if (!demoPlaying) return false;
        return demoCameraPos.y < w->waterLevel();
    }
    void toggleDemoPause() { demoPaused = !demoPaused; }
    void toggleDemoEvents() { demoShowEvents = !demoShowEvents; }
    bool demoEventsShown() const { return demoShowEvents; }
    bool demoOrbitCamActive() const { return demoOrbitCam; }
    const std::vector<DemoTimedEvent>& getDemoEventLog() const { return demoEventLog; }
    void requestDemoStep() { demoStepRequest = true; }
    const std::vector<Point3F>& getDemoPath() const { return demoPath; }
    void setDemoFastForward(bool v) { demoFastForward = v; }
    DemoParser* getDemoParser() const { return demoParser; }
    Menu& menu() { return *mMenu; }
    int getDemoBlocksDone() const { return demoBlocksDone; }
    int getDemoBlocksTotal() const { return demoBlocksTotal; }
    DTSShape* getOrLoadDemoShape(const std::string& className, const std::string& skinName = "");

    // Live ghost accessors for HUD/scoreboard
    bool isConnected() const { return activeConn && activeConn->isConnected(); }
    std::vector<int> getLiveGhostIndices() const { return liveGhosts.getAllIndices(); }
    const GhostEntry* getLiveGhost(int idx) const { return liveGhosts.getGhost(idx); }

    // Shape viewer mode
    bool shapeViewerActive = false;
    std::vector<std::string> shapeViewerFiles;
    int shapeViewerIndex = 0;
    DTSShape shapeViewerShape;
    float shapeViewerYaw = 0.6f;
    float shapeViewerPitch = 0.25f;
    float shapeViewerAnimTime = 0;
    bool shapeViewerBoundsInit = false;
    Point3F shapeViewerCenter{0,0,0};
    float shapeViewerFitScale = 1.0f;
    void enterShapeViewer();
    void shapeViewerNext();
    void shapeViewerPrev();
    void shapeViewerLoadCurrent();
    bool isShapeViewerActive() const { return shapeViewerActive; }

private:
    GameConfig cfg;
    Player* pl{};
    World* w{};
    Menu* mMenu{};
    HUD* hud{};
    State gameState = MenuScreen;
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
    float demoCameraFov = -1.0f; // FOV from demo stream
    float shakeIntensity = 0.0f; // camera shake for explosions
    Point3F shakeOffset{0,0,0};
    std::vector<DemoTimedEvent> demoEventLog;
    bool demoShowEvents = true;
    // Orbit camera for demo spectator mode
    bool demoOrbitCam = false;
    bool demoFirstPersonCam = false;
    float orbitAngle = 0;
    float orbitDistance = 300.0f;
    float orbitHeight = 150.0f;
    Point3F orbitCenter{};
    bool orbitCenterInit = false;
    std::vector<Point3F> demoPath;
    int demoPathCount = 0;

    // Live spectator
    bool liveSpectateInit = false;
    bool liveSpectateRespawned = false;

    // Editor mode
    bool editorActive = false;
    int editorPlaceClass = 31; // classId to place

    // Projectile trail system
    struct TrailPoint { float x, y, z; float life; ColorF color; };
    std::map<int, std::vector<TrailPoint>> demoTrails;

    // Live network ghost tracking
    GhostTracker liveGhosts;
    // Received datablock tracking: classId → list of datablocks with payload
    struct ReceivedDatablock {
        T2Protocol::DatablockHeader hdr;
        std::vector<uint8_t> payload;
    };
    std::map<uint32_t, std::vector<ReceivedDatablock>> receivedDatablocks;
    const std::vector<ReceivedDatablock>* getDatablocksForClass(uint32_t classId) const {
        auto it = receivedDatablocks.find(classId);
        return it != receivedDatablocks.end() ? &it->second : nullptr;
    }
    // Ghost index assigned by server for this client's player
    uint32_t serverPlayerGhostIndex = 0;
    bool serverPlayerGhostSynced = false;
};
