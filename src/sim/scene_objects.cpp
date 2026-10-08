// Scene objects the server ghosts to every client (all Ghostable |
// ScopeAlways): packUpdate as the retail Tribes 2 client reads it (the demo
// reader), fields and defaults from the engine's initPersistFields.
#include "sim/net_object.h"
#include "sim/sim_state.h"
#include "sim/game_connection.h"
#include "sim/engine_classes.h"
#include "sim/game_base.h"
#include "sim/engine_crc.h"
#include "sim/server_container.h"
#include "sim/datablock_pack.h"
#include "script/torquescript.h"
#include "core/engine.h"
#include "script/script_engine.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <strings.h>

namespace {

enum { InitMask = 1u << 0 };

uint32_t fileCrc(const std::string& path) {
    if (!Engine::instance().filesys) return 0;
    std::vector<uint8_t> data;
    if (!Engine::instance().fs().readFile(path.c_str(), data)) return 0;
    return EngineCrc::calculate(data.data(), data.size());
}

// terrData.cc
class TerrainBlockObject : public SceneObject {
public:
    TerrainBlockObject() { scopeAlways = true; ServerContainer::invalidate(); }
    ~TerrainBlockObject() override { ServerContainer::invalidate(); }
    const char* netClassName() const override { return "TerrainBlock"; }
    std::string terrainFile, detailTexture;
    int32_t squareSize = 8;
    std::vector<int32_t> emptySquareRuns;
    uint32_t crc = 0;
    void readFields() override {
        SceneObject::readFields();
        terrainFile = Fields::string(script, "terrainFile");
        detailTexture = Fields::string(script, "detailTexture");
        squareSize = Fields::s32(script, "squareSize", 8);
        emptySquareRuns = Fields::s32Vector(script, "emptySquares");
        crc = fileCrc("terrains/" + terrainFile);
    }
    uint32_t packUpdate(GameConnection&, uint32_t mask, TorqueBitWriter& w) override {
        auto runs = [&] {
            w.writeU32((uint32_t)emptySquareRuns.size());
            for (int32_t run : emptySquareRuns) w.writeU32((uint32_t)run);
        };
        if (w.writeFlag(mask & InitMask)) {
            w.writeU32(crc);
            w.writeString(terrainFile);
            w.writeString(detailTexture);
            w.writeU32((uint32_t)squareSize);
            runs();
        } else if (w.writeFlag(mask & (1u << 1))) { // EmptyMask
            runs();
        }
        return 0;
    }
};

// sky.cc (retail layout: the demo reader's readSkyData)
class SkyObject : public SceneObject {
public:
    enum SkyMasks : uint32_t {
        VisibilityMask = 1u << 1, StormCloudMask = 1u << 2, StormFogMask = 1u << 3, StormRealFogMask = 1u << 4,
        WindMask = 1u << 5, StormCloudsOnMask = 1u << 6, StormFogOnMask = 1u << 7,
    };
    enum StormState { isDone = 0, comingIn = 1, goingOut = 2 };
    SkyObject() { scopeAlways = true; }
    const char* netClassName() const override { return "Sky"; }
    void readFields() override {
        SceneObject::readFields();
        const auto w = Fields::point(script, "windVelocity", {1, 0, 0});
        wind = {w[0], w[1], w[2]};
    }
    uint32_t packUpdate(GameConnection&, uint32_t mask, TorqueBitWriter& w) override {
        if (w.writeFlag(mask & InitMask)) {
            w.writeString(Fields::string(script, "materialList"));
            const auto fog = Fields::color(script, "fogColor", {0, 0, 0, 1});
            w.writeF32(fog[0]); w.writeF32(fog[1]); w.writeF32(fog[2]);
            // Sky::onAdd: fog volumes up to the first 0 or -1 visible distance.
            std::vector<std::array<float, 6>> volumes;
            for (int i = 1; i <= 3; ++i) {
                const std::string n = std::to_string(i);
                const auto v = Fields::point(script, ("fogVolume" + n).c_str(), {-1, 0, 0});
                if (v[0] == -1 || v[0] == 0) break;
                const auto c = Fields::color(script, ("fogVolumeColor" + n).c_str(), {0, 0, 0, 1});
                volumes.push_back({v[0], v[1], v[2], c[0], c[1], c[2]});
            }
            w.writeU32((uint32_t)volumes.size());
            w.writeInt(Fields::boolean(script, "useSkyTextures", true) ? 1 : 0, 8); // stream->write(bool)
            w.writeInt(Fields::boolean(script, "renderBottomTexture", false) ? 1 : 0, 8);
            const auto solid = Fields::color(script, "SkySolidColor", {0, 0, 0, 1});
            w.writeF32(solid[0]); w.writeF32(solid[1]); w.writeF32(solid[2]);
            w.writeInt(Fields::boolean(script, "windEffectPrecipitation", false) ? 1 : 0, 8);
            for (const auto& v : volumes) for (float f : v) w.writeF32(f);
            for (int i = 0; i < 3; ++i) {
                w.writeString(Fields::string(script, ("cloudText" + std::to_string(i + 1)).c_str()));
                w.writeF32(Fields::f32(script, ("cloudHeightPer" + std::to_string(i)).c_str(),
                           Fields::f32(script, ("cloudHeightPer[" + std::to_string(i) + "]").c_str(), 0)));
                w.writeF32(Fields::f32(script, ("cloudSpeed" + std::to_string(i + 1)).c_str(), 0));
            }
            w.writePoint({wind.x, wind.y, wind.z});
            w.writeU32((uint32_t)fogVolume);
            // A storm fog in progress.
            const uint32_t stormTimeDiff = currentTimeMS() - fogStartTime;
            if (w.writeFlag((float)stormTimeDiff / 1000.0f < fogTime)) {
                w.writeF32(fogPercentage);
                w.writeF32(fogTime);
                w.writeU32((uint32_t)fogState);
                w.writeU32(stormTimeDiff);
                w.writeF32(fogEndPercentage);
            }
        }
        if (w.writeFlag(mask & StormCloudsOnMask)) w.writeInt(stormCloudsOn ? 1 : 0, 8);
        if (w.writeFlag((mask & StormFogOnMask) && !(mask & InitMask))) w.writeInt(stormFogOn ? 1 : 0, 8);
        if (w.writeFlag(mask & VisibilityMask)) {
            w.writeF32(Fields::f32(script, "visibleDistance", 0));
            w.writeF32(Fields::f32(script, "fogDistance", 0));
        }
        if (w.writeFlag(mask & StormCloudMask)) {
            w.writeU32((uint32_t)cloudState);
            w.writeF32(cloudTime);
        }
        if (w.writeFlag((mask & StormFogMask) && !(mask & InitMask))) {
            w.writeF32(fogEndPercentage);
            w.writeF32(fogTime);
            w.writeU32((uint32_t)fogVolume);
            fogStartTime = currentTimeMS();
        }
        if (w.writeFlag(mask & StormRealFogMask)) {
            w.writeU32((uint32_t)realFog);
            w.writeF32(realFogMax);
            w.writeF32(realFogMin);
            w.writeF32(realFogSpeed);
        }
        if (w.writeFlag(mask & WindMask)) w.writePoint({wind.x, wind.y, wind.z});
        return 0;
    }

    void stormCloudsShow(bool show) {
        stormCloudsOn = show;
        setMaskBits(StormCloudsOnMask);
    }
    void stormFogShow(bool show) {
        stormFogOn = show;
        setMaskBits(StormFogOnMask);
    }
    void stormClouds(int32_t state, float time) {
        cloudState = state ? comingIn : goingOut;
        cloudTime = time;
        setMaskBits(StormCloudMask);
    }
    void stormFog(float percentage, float time) {
        fogTime = time;
        if (fogEndPercentage >= 0.0f) {
            fogState = fogEndPercentage > percentage ? goingOut : comingIn;
            fogPercentage = fogEndPercentage;
        } else {
            fogState = fogPercentage > percentage ? goingOut : comingIn;
        }
        fogEndPercentage = percentage;
        setMaskBits(StormFogMask);
    }
    void stormRealFog(int32_t value, float max, float min, float speed) {
        realFog = value;
        realFogMax = max;
        realFogMin = min;
        realFogSpeed = speed;
        setMaskBits(StormRealFogMask);
    }
    void setWindVelocity(const Point3F& velocity) {
        wind = velocity;
        setMaskBits(WindMask);
    }
    Point3F wind{1, 0, 0};

private:
    static uint32_t currentTimeMS() { return (uint32_t)(SimState::simTime() * 1000.0); }
    bool stormCloudsOn = true, stormFogOn = false;
    int32_t cloudState = isDone;
    float cloudTime = 0.0f;
    // mFogPercentage is never set on the engine's server before a storm.
    float fogPercentage = 0.0f, fogTime = 0.0f, fogEndPercentage = -1.0f;
    int32_t fogState = isDone, fogVolume = -1;
    uint32_t fogStartTime = 0;
    int32_t realFog = 0;
    float realFogMax = 0.0f, realFogMin = 0.0f, realFogSpeed = 0.0f;
};

// sun.cc (retail: textures, then 19 floats with the lens flare values)
class SunObject : public SceneObject {
public:
    SunObject() { scopeAlways = true; }
    const char* netClassName() const override { return "Sun"; }
    uint32_t packUpdate(GameConnection&, uint32_t mask, TorqueBitWriter& w) override {
        static const char* defaults[5] = {"special/sunFlare", "special/sunFlare02", "special/LensFlare/flare01",
                                          "special/LensFlare/flare02", "special/LensFlare/flare03"};
        if (w.writeFlag(mask & InitMask))
            for (int i = 0; i < 5; ++i)
                w.writeString(Fields::string(script, ("texture[" + std::to_string(i) + "]").c_str(),
                              Fields::string(script, ("texture" + std::to_string(i)).c_str(), defaults[i])));
        if (w.writeFlag(mask & InitMask)) {
            // Sun::conformLight: direction normalized, colours clamped.
            auto dir = Fields::point(script, "direction", {0.0f, 0.707f, -0.707f});
            const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            if (len > 0) for (float& v : dir) v /= len;
            auto color = Fields::color(script, "color", {0.7f, 0.7f, 0.7f, 1});
            auto ambient = Fields::color(script, "ambient", {0.3f, 0.3f, 0.3f, 1});
            for (float& v : color) v = std::clamp(v, 0.0f, 1.0f);
            for (float& v : ambient) v = std::clamp(v, 0.0f, 1.0f);
            const auto flare = Fields::color(script, "flareColor", {1, 1, 1, 1});
            for (float v : dir) w.writeF32(v);
            for (float v : color) w.writeF32(v);
            for (float v : ambient) w.writeF32(v);
            w.writeF32(Fields::f32(script, "lensFlareScale", 0.7f));
            w.writeF32(Fields::f32(script, "lensFlareIntensity", 1.0f));
            w.writeF32(Fields::f32(script, "frontFlareSize", 300.0f));
            w.writeF32(Fields::f32(script, "backFlareSize", 450.0f));
            for (float v : flare) w.writeF32(v);
        }
        return 0;
    }
};

// interiorInstance.cc
class InteriorInstanceObject : public SceneObject {
public:
    enum InteriorMasks : uint32_t { AlarmMask = 1u << 1 };
    InteriorInstanceObject() { scopeAlways = true; ServerContainer::invalidate(); }
    ~InteriorInstanceObject() override { ServerContainer::invalidate(); }
    const char* netClassName() const override { return "InteriorInstance"; }
    uint32_t crc = 0;
    bool alarmState = false; // mAlarmState
    void readFields() override {
        SceneObject::readFields();
        crc = fileCrc("interiors/" + Fields::string(script, "interiorFile"));
    }
    // InteriorInstance::setAlarmMode: only an interior with alarm lighting.
    void setAlarmMode(bool alarm) {
        if (!ServerContainer::interiorHasAlarmState(Fields::string(script, "interiorFile"))) return;
        if (alarmState == alarm) return;
        alarmState = alarm;
        setMaskBits(AlarmMask);
    }
    // The audioProfile / audioEnvironment datablock, if any.
    static void writeDataBlockRef(TorqueBitWriter& w, const std::string& name) {
        ScriptObject* data = name.empty() ? nullptr : ScriptEngine::instance().findObject(name.c_str());
        const int id = data ? ScriptEngine::instance().objectId(data) : 0;
        if (w.writeFlag(id >= 3 && id <= 2050)) w.writeRangedU32((uint32_t)id, 3, 2050);
    }
    uint32_t packUpdate(GameConnection&, uint32_t mask, TorqueBitWriter& w) override {
        if (w.writeFlag(mask & InitMask)) {
            w.writeU32(crc);
            w.writeString(Fields::string(script, "interiorFile"));
            w.writeFlag(Fields::boolean(script, "showTerrainInside", false));
            writeTransform(w);
            writeScale(w);
            w.writeFlag(alarmState);
            w.writeString(Fields::string(script, "skinBase"));
            writeDataBlockRef(w, Fields::string(script, "audioProfile"));
            writeDataBlockRef(w, Fields::string(script, "audioEnvironment"));
        } else {
            // The shipped layout (the client's reader): transform, the alarm
            // state, then skin and audio changes.
            w.writeFlag(false); // transform
            w.writeFlag(alarmState);
            w.writeFlag(false); // skin
            w.writeFlag(false); // audio
        }
        return 0;
    }
};

// tsStatic.cc
class TSStaticObject : public SceneObject {
public:
    TSStaticObject() { scopeAlways = true; }
    const char* netClassName() const override { return "TSStatic"; }
    uint32_t packUpdate(GameConnection&, uint32_t, TorqueBitWriter& w) override {
        writeTransform(w);
        writeScale(w);
        w.writeString(Fields::string(script, "shapeName"));
        return 0;
    }
};

// missionArea.cc
class MissionAreaObject : public NetObject {
public:
    MissionAreaObject() { scopeAlways = true; }
    const char* netClassName() const override { return "MissionArea"; }
    uint32_t packUpdate(GameConnection&, uint32_t mask, TorqueBitWriter& w) override {
        if (w.writeFlag(mask & InitMask)) {
            auto area = Fields::s32Vector(script, "area");
            if (area.size() < 4) area = {768, 768, 512, 512};
            for (int i = 0; i < 4; ++i) w.writeU32((uint32_t)area[i]);
            w.writeF32(Fields::f32(script, "flightCeiling", 2000.0f));
            w.writeF32(Fields::f32(script, "flightCeilingRange", 50.0f));
        }
        return 0;
    }
};

// audioEmitter.cc: the emitter's sound for the clients, everything in the
// initial update with what matches the default description (or a
// datablock description) left out, as AudioEmitter::packData leaves it.
class AudioEmitterObject : public SceneObject {
public:
    AudioEmitterObject() { scopeAlways = true; }
    const char* netClassName() const override { return "AudioEmitter"; }
    uint32_t packUpdate(GameConnection&, uint32_t mask, TorqueBitWriter& w) override {
        enum Dirty {
            Profile = 1u << 0, Description = 1u << 1, Filename = 1u << 2, UseProfileDescription = 1u << 3,
            Volume = 1u << 4, IsLooping = 1u << 5, Is3D = 1u << 6, MinDistance = 1u << 7, MaxDistance = 1u << 8,
            ConeInsideAngle = 1u << 9, ConeOutsideAngle = 1u << 10, ConeOutsideVolume = 1u << 11,
            ConeVector = 1u << 12, LoopCount = 1u << 13, MinLoopGap = 1u << 14, MaxLoopGap = 1u << 15,
            AudioType = 1u << 16, OutsideAmbient = 1u << 17, AllDirtyMask = (1u << 19) - 1,
            LoopingMask = LoopCount | MinLoopGap | MaxLoopGap,
            Is3DMask = MinDistance | MaxDistance | ConeInsideAngle | ConeOutsideAngle | ConeOutsideVolume | ConeVector,
            UseProfileDescriptionMask = Volume | LoopingMask | Is3DMask | IsLooping | Is3D | AudioType,
        };
        auto& engine = ScriptEngine::instance();
        auto datablockId = [&](const char* field) {
            const std::string name = Fields::string(script, field);
            ScriptObject* block = name.empty() ? nullptr : engine.findDataBlock(name);
            return block ? (uint32_t)engine.objectId(block) : 0u;
        };
        const uint32_t profile = datablockId("profile"), description = datablockId("description");
        const std::string fileName = Fields::string(script, "fileName");
        bool useProfileDescription = Fields::boolean(script, "useProfileDescription", false);
        const bool outsideAmbient = Fields::boolean(script, "outsideAmbient", true);
        const float volume = Fields::f32(script, "volume", 1.0f);
        const bool isLooping = Fields::boolean(script, "isLooping", true);
        const bool is3D = Fields::boolean(script, "is3D", true);
        const float minDistance = Fields::f32(script, "minDistance", 1.0f);
        const float maxDistance = Fields::f32(script, "maxDistance", 100.0f);
        const int32_t coneInside = Fields::s32(script, "coneInsideAngle", 360);
        const int32_t coneOutside = Fields::s32(script, "coneOutsideAngle", 360);
        const float coneOutsideVolume = Fields::f32(script, "coneOutsideVolume", 1.0f);
        const auto coneVector = Fields::point(script, "coneVector", {0, 0, 1});
        const int32_t loopCount = Fields::s32(script, "loopCount", -1);
        const int32_t minLoopGap = Fields::s32(script, "minLoopGap", 0);
        const int32_t maxLoopGap = Fields::s32(script, "maxLoopGap", 0);

        uint32_t dirty = 0;
        // initial update
        if (w.writeFlag(mask & InitMask)) {
            mask |= TransformMask;
            dirty = AllDirtyMask;
            if (!profile) dirty &= ~Profile;
            if (!description) dirty &= ~Description;
            if (fileName.empty()) dirty &= ~Filename;
            if (!useProfileDescription) dirty &= ~UseProfileDescription;
            if (outsideAmbient) dirty &= ~OutsideAmbient;
        }
        if (w.writeFlag(mask & TransformMask)) writeAffineTransform(w);
        if (w.writeFlag(dirty & Profile))
            if (w.writeFlag(profile != 0)) w.writeRangedU32(profile, DataBlockPack::ObjectIdFirst, DataBlockPack::ObjectIdLast);
        if (w.writeFlag(dirty & Description))
            if (w.writeFlag(description != 0))
                w.writeRangedU32(description, DataBlockPack::ObjectIdFirst, DataBlockPack::ObjectIdLast);
        if (w.writeFlag(dirty & Filename)) w.writeString(fileName);
        if (w.writeFlag(dirty & UseProfileDescription)) {
            if (useProfileDescription) {
                if (!profile) useProfileDescription = false;
                else dirty &= ~UseProfileDescriptionMask;
            }
            if (!useProfileDescription) dirty |= UseProfileDescriptionMask;
            w.writeFlag(useProfileDescription);
        }
        if (description && !useProfileDescription) dirty &= ~UseProfileDescriptionMask;
        // check initial update against the default description
        if ((mask & InitMask) && !useProfileDescription) {
            if (volume == 1.0f) dirty &= ~Volume;
            if (!isLooping) {
                dirty &= ~LoopingMask;
            } else {
                dirty &= ~IsLooping;
                if (loopCount == -1) dirty &= ~LoopCount;
                if (minLoopGap == 0) dirty &= ~MinLoopGap;
                if (maxLoopGap == 0) dirty &= ~MaxLoopGap;
            }
            if (!is3D) {
                dirty &= ~Is3DMask;
            } else {
                dirty &= ~Is3D;
                if (minDistance == 1.0f) dirty &= ~MinDistance;
                if (maxDistance == 100.0f) dirty &= ~MaxDistance;
                if (coneInside == 360) dirty &= ~ConeInsideAngle;
                if (coneOutside == 360) dirty &= ~ConeOutsideAngle;
                if (coneOutsideVolume == 1.0f) dirty &= ~ConeOutsideVolume;
                if (coneVector[0] == 0 && coneVector[1] == 0 && coneVector[2] == 1) dirty &= ~ConeVector;
            }
            dirty &= ~AudioType;
        }
        if (w.writeFlag(dirty & Volume)) w.writeF32(volume);
        if (w.writeFlag(dirty & IsLooping)) {
            if (isLooping) dirty |= LoopingMask;
            else dirty &= ~LoopingMask;
            w.writeFlag(isLooping);
        }
        if (w.writeFlag(dirty & Is3D)) {
            if (is3D) dirty |= Is3DMask;
            else dirty &= ~Is3DMask;
            w.writeFlag(is3D);
        }
        if (w.writeFlag(dirty & MinDistance)) w.writeF32(minDistance);
        if (w.writeFlag(dirty & MaxDistance)) w.writeF32(maxDistance);
        if (w.writeFlag(dirty & ConeInsideAngle)) w.writeU32((uint32_t)coneInside);
        if (w.writeFlag(dirty & ConeOutsideAngle)) w.writeU32((uint32_t)coneOutside);
        if (w.writeFlag(dirty & ConeOutsideVolume)) w.writeF32(coneOutsideVolume);
        if (w.writeFlag(dirty & ConeVector)) {
            w.writeF32(coneVector[0]);
            w.writeF32(coneVector[1]);
            w.writeF32(coneVector[2]);
        }
        if (w.writeFlag(dirty & LoopCount)) w.writeU32((uint32_t)loopCount);
        if (w.writeFlag(dirty & MinLoopGap)) w.writeU32((uint32_t)minLoopGap);
        if (w.writeFlag(dirty & MaxLoopGap)) w.writeU32((uint32_t)maxLoopGap);
        if (w.writeFlag(dirty & AudioType)) w.writeU32(3); // EffectAudioType: never sent (always cleared)
        if (w.writeFlag(dirty & OutsideAmbient)) w.writeFlag(outsideAmbient);
        return 0;
    }

private:
    enum { TransformMask = 1u << 1 };
};

// particleEmitter.cc ParticleEmissionDummy: the transform, scale and the
// emitter datablock (no emitter: onAdd fails and it never ghosts).
class ParticleEmissionDummyObject : public GameBase {
public:
    ParticleEmissionDummyObject() { ghostable = true; }
    const char* netClassName() const override { return "ParticleEmissionDummy"; }
    void readFields() override {
        GameBase::readFields();
        ghostable = emitterId() != 0 && !dataBlock().empty();
    }
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override {
        const uint32_t ret = GameBase::packUpdate(connection, mask, w);
        writeTransform(w);
        writeScale(w);
        const uint32_t emitter = emitterId();
        if (w.writeFlag(emitter != 0)) w.writeRangedU32(emitter, DataBlockPack::ObjectIdFirst, DataBlockPack::ObjectIdLast);
        return ret;
    }

private:
    uint32_t emitterId() const {
        const std::string name = Fields::string(script, "emitter");
        ScriptObject* block = name.empty() ? nullptr : ScriptEngine::instance().findDataBlock(name);
        return block ? (uint32_t)ScriptEngine::instance().objectId(block) : 0u;
    }
};

// fireballAtmosphere.cc: the drop parameters (the client drops the
// fireballs); its box covers the world.
class FireballAtmosphereObject : public GameBase {
public:
    FireballAtmosphereObject() { ghostable = true; }
    const char* netClassName() const override { return "FireballAtmosphere"; }
    bool objectBox(float lo[3], float hi[3]) const override {
        for (int i = 0; i < 3; ++i) {
            lo[i] = -1e6f;
            hi[i] = 1e6f;
        }
        return true;
    }
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override {
        const uint32_t ret = GameBase::packUpdate(connection, mask, w);
        if (w.writeFlag(mask & InitialUpdateMask)) {
            w.writeF32(Fields::f32(script, "dropRadius", 600.0f));
            w.writeF32(Fields::f32(script, "dropsPerMinute", 3.0f));
            w.writeF32(Fields::f32(script, "maxDropAngle", 30.0f));
            w.writeF32(Fields::f32(script, "minDropAngle", 0.0f));
            w.writeF32(Fields::f32(script, "startVelocity", 20.0f));
            w.writeF32(Fields::f32(script, "dropHeight", 500.0f));
            // onAdd normalizes the drop direction.
            auto dir = Fields::point(script, "dropDir", {0.5f, 0.5f, -0.5f});
            const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            if (len > 0.0f) for (float& c : dir) c /= len;
            w.writeF32(dir[0]);
            w.writeF32(dir[1]);
            w.writeF32(dir[2]);
        }
        return ret;
    }
};

// vehicles/vehicleBlocker.cc: a box (x and y half extents, height from
// the base) only vehicles collide with; flags carry one.
class VehicleBlockerObject : public SceneObject {
public:
    VehicleBlockerObject() { scopeAlways = true; }
    const char* netClassName() const override { return "VehicleBlocker"; }
    bool objectBox(float lo[3], float hi[3]) const override {
        const auto d = Fields::point(script, "dimensions", {0, 0, 0});
        lo[0] = -d[0]; lo[1] = -d[1]; lo[2] = 0;
        hi[0] = d[0]; hi[1] = d[1]; hi[2] = d[2];
        return true;
    }
    uint32_t packUpdate(GameConnection&, uint32_t, TorqueBitWriter& w) override {
        writeTransform(w);
        writeScale(w);
        const auto d = Fields::point(script, "dimensions", {0, 0, 0});
        w.writePoint({d[0], d[1], d[2]});
        return 0;
    }
};

// stationFXPersonal.cc / stationFXVehicle.cc: the station effect, placed
// at its station, the station's ghost sent once, deleted after the
// datablock's lifetime.
class StationFXObject : public GameBase {
public:
    StationFXObject(const char* netClass, float boxLo[3], float boxHi[3], float defaultLifetime)
        : netClass(netClass), defaultLifetime(defaultLifetime) {
        ghostable = true;
        for (int i = 0; i < 3; ++i) {
            lo[i] = boxLo[i];
            hi[i] = boxHi[i];
        }
    }
    const char* netClassName() const override { return netClass; }
    bool objectBox(float outLo[3], float outHi[3]) const override {
        for (int i = 0; i < 3; ++i) {
            outLo[i] = lo[i];
            outHi[i] = hi[i];
        }
        return true;
    }
    void onAdded() override {
        const int id = Fields::s32(script, "stationObject", -1);
        ScriptObject* object = id != -1 ? ScriptEngine::instance().findObject(std::to_string(id).c_str()) : nullptr;
        auto* shape = object ? dynamic_cast<SceneObject*>(object->engine.get()) : nullptr;
        if (shape && EngineClasses::isA(object->className, "ShapeBase")) {
            station = ScriptEngine::instance().objectKey(object);
            transform = shape->transform;
        }
    }
    void processMove(const ClientMoveIn* move) override {
        GameBase::processMove(move);
        currMS += 32;
        if (currMS >= dataFloat("lifetime", defaultLifetime) * 1000.0f) {
            ScriptEngine::instance().deleteScriptObject(ScriptEngine::instance().objectKey(script));
            return;
        }
    }
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override {
        const uint32_t ret = GameBase::packUpdate(connection, mask, w);
        if (w.writeFlag(mask & InitialUpdateMask)) {
            const int ghost = station.empty() || !EngineObjects::find(station) ? -1 : connection.ghostIndex(station);
            if (w.writeFlag(ghost != -1)) w.writeRangedU32((uint32_t)ghost, 0, 1024);
        }
        return ret;
    }

private:
    const char* netClass;
    float defaultLifetime;
    float lo[3], hi[3];
    std::string station;
    uint32_t currMS = 0;
};

// waterBlock.cc
class WaterBlockObject : public SceneObject {
public:
    WaterBlockObject() { scopeAlways = true; ServerContainer::invalidate(); }
    ~WaterBlockObject() override { ServerContainer::invalidate(); }
    const char* netClassName() const override { return "WaterBlock"; }
    uint32_t packUpdate(GameConnection&, uint32_t, TorqueBitWriter& w) override {
        static const char* types[] = {"Water", "OceanWater", "RiverWater", "StagnantWater",
                                      "Lava", "HotLava", "CrustyLava", "Quicksand"};
        int32_t liquidType = 1; // eOceanWater
        const std::string name = Fields::string(script, "liquidType");
        for (int i = 0; i < 8; ++i) if (strcasecmp(name.c_str(), types[i]) == 0) liquidType = i;
        writeAffineTransform(w);
        writeScale(w);
        w.writeString(Fields::string(script, "surfaceTexture"));
        w.writeString(Fields::string(script, "envMapTexture"));
        w.writeString(Fields::string(script, "submergeTexture[0]", Fields::string(script, "submergeTexture0")));
        w.writeString(Fields::string(script, "submergeTexture[1]", Fields::string(script, "submergeTexture1")));
        w.writeU32((uint32_t)liquidType);
        w.writeF32(Fields::f32(script, "density", 1));
        w.writeF32(Fields::f32(script, "viscosity", 15));
        w.writeF32(Fields::f32(script, "waveMagnitude", 1));
        w.writeF32(Fields::f32(script, "surfaceOpacity", 0.75f));
        w.writeF32(Fields::f32(script, "envMapIntensity", 1));
        w.writeInt(Fields::boolean(script, "removeWetEdges", true) ? 1 : 0, 8);
        w.writeFlag(false); // audio environment
        return 0;
    }
};

} // namespace

// InteriorInstance::setAlarmMode("On" | anything else).
void registerSceneObjectNatives(TorqueScript& ts) {
    auto sky = [](const std::vector<VMValue>& args) -> SkyObject* {
        return args.empty() ? nullptr : EngineObjects::get<SkyObject>(args[0].toString());
    };
    auto boolArg = [](const std::vector<VMValue>& args, size_t i) {
        // dAtob: "true" or a non-zero number.
        const std::string v = i < args.size() ? args[i].toString() : std::string();
        return strcasecmp(v.c_str(), "true") == 0 || std::atof(v.c_str()) != 0.0;
    };
    auto f = [](const std::vector<VMValue>& args, size_t i) { return i < args.size() ? (float)std::atof(args[i].toString().c_str()) : 0.0f; };
    auto n = [](const std::vector<VMValue>& args, size_t i) { return i < args.size() ? std::atoi(args[i].toString().c_str()) : 0; };
    ts.registerNative("Sky::stormCloudsShow", [sky, boolArg](const std::vector<VMValue>& args) -> VMValue {
        if (auto* s = sky(args)) s->stormCloudsShow(boolArg(args, 1));
        return VMValue("");
    });
    ts.registerNative("Sky::stormFogShow", [sky, boolArg](const std::vector<VMValue>& args) -> VMValue {
        if (auto* s = sky(args)) s->stormFogShow(boolArg(args, 1));
        return VMValue("");
    });
    ts.registerNative("Sky::stormClouds", [sky, f, n](const std::vector<VMValue>& args) -> VMValue {
        if (auto* s = sky(args)) s->stormClouds(n(args, 1), f(args, 2));
        return VMValue("");
    });
    ts.registerNative("Sky::stormFog", [sky, f](const std::vector<VMValue>& args) -> VMValue {
        if (auto* s = sky(args)) s->stormFog(f(args, 1), f(args, 2));
        return VMValue("");
    });
    ts.registerNative("Sky::realFog", [sky, f, n](const std::vector<VMValue>& args) -> VMValue {
        if (auto* s = sky(args)) s->stormRealFog(n(args, 1), f(args, 2), f(args, 3), f(args, 4));
        return VMValue("");
    });
    ts.registerNative("Sky::setWindVelocity", [sky, f](const std::vector<VMValue>& args) -> VMValue {
        if (auto* s = sky(args)) s->setWindVelocity({f(args, 1), f(args, 2), f(args, 3)});
        return VMValue("");
    });
    ts.registerNative("Sky::getWindVelocity", [sky](const std::vector<VMValue>& args) -> VMValue {
        const Point3F w = sky(args) ? sky(args)->wind : Point3F{0, 0, 0};
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%f %f %f", w.x, w.y, w.z);
        return VMValue(buffer);
    });
    ts.registerNative("InteriorInstance::setAlarmMode", [](const std::vector<VMValue>& args) -> VMValue {
        auto* interior = args.empty() ? nullptr : EngineObjects::get<InteriorInstanceObject>(args[0].toString());
        if (interior) interior->setAlarmMode(args.size() > 1 && strcasecmp(args[1].toString().c_str(), "On") == 0);
        return VMValue("");
    });
    // MissionArea::getArea: "x y width height" (768 768 512 512 unset).
    ts.registerNative("MissionArea::getArea", [](const std::vector<VMValue>& args) -> VMValue {
        auto* area = args.empty() ? nullptr : EngineObjects::get<NetObject>(args[0].toString());
        auto rect = area ? Fields::s32Vector(area->script, "area") : std::vector<int32_t>{};
        if (rect.size() < 4) rect = {768, 768, 512, 512};
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "%d %d %d %d", rect[0], rect[1], rect[2], rect[3]);
        return VMValue(buffer);
    });
    // MissionArea::setArea(x, y, width, height): the clients get it again.
    ts.registerNative("MissionArea::setArea", [](const std::vector<VMValue>& args) -> VMValue {
        auto* area = args.empty() ? nullptr : EngineObjects::get<NetObject>(args[0].toString());
        if (!area || args.size() < 5) return VMValue("");
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "%d %d %d %d", std::atoi(args[1].toString().c_str()),
                      std::atoi(args[2].toString().c_str()), std::atoi(args[3].toString().c_str()),
                      std::atoi(args[4].toString().c_str()));
        ScriptEngine::instance().setObjectField(area->script, "area", VMValue(buffer));
        area->setMaskBits(InitMask);
        return VMValue("");
    });
}

void registerSceneObjectClasses() {
    EngineObjects::registerClass("TerrainBlock", [] { return std::make_shared<TerrainBlockObject>(); });
    EngineObjects::registerClass("Sky", [] { return std::make_shared<SkyObject>(); });
    EngineObjects::registerClass("Sun", [] { return std::make_shared<SunObject>(); });
    EngineObjects::registerClass("InteriorInstance", [] { return std::make_shared<InteriorInstanceObject>(); });
    EngineObjects::registerClass("TSStatic", [] { return std::make_shared<TSStaticObject>(); });
    EngineObjects::registerClass("MissionArea", [] { return std::make_shared<MissionAreaObject>(); });
    EngineObjects::registerClass("WaterBlock", [] { return std::make_shared<WaterBlockObject>(); });
    EngineObjects::registerClass("ParticleEmissionDummy", [] { return std::make_shared<ParticleEmissionDummyObject>(); });
    EngineObjects::registerClass("FireballAtmosphere", [] { return std::make_shared<FireballAtmosphereObject>(); });
    EngineObjects::registerClass("VehicleBlocker", [] { return std::make_shared<VehicleBlockerObject>(); });
    EngineObjects::registerClass("StationFXPersonal", [] {
        float lo[3] = {-3, -3, -3}, hi[3] = {3, 3, 3};
        return std::make_shared<StationFXObject>("StationFXPersonal", lo, hi, 2.0f);
    });
    EngineObjects::registerClass("StationFXVehicle", [] {
        float lo[3] = {-12, -12, 0}, hi[3] = {12, 12, 12};
        return std::make_shared<StationFXObject>("StationFXVehicle", lo, hi, 6.0f);
    });
    EngineObjects::registerClass("AudioEmitter", [] { return std::make_shared<AudioEmitterObject>(); });
}
