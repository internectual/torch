// Scene objects the server ghosts to every client (all Ghostable |
// ScopeAlways): packUpdate as the retail Tribes 2 client reads it (the demo
// reader), fields and defaults from the engine's initPersistFields.
#include "sim/net_object.h"
#include "sim/engine_crc.h"
#include "sim/server_container.h"
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
    TerrainBlockObject() { scopeAlways = true; }
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
    SkyObject() { scopeAlways = true; }
    const char* netClassName() const override { return "Sky"; }
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
            const auto wind = Fields::point(script, "windVelocity", {0, 0, 0});
            w.writePoint({wind[0], wind[1], wind[2]});
            w.writeF32(-1.0f);   // mFogVolume
            w.writeFlag(false);  // no storm fog in progress
        }
        w.writeFlag(false); // StormCloudsOnMask
        w.writeFlag(false); // StormFogOnMask
        if (w.writeFlag(mask & InitMask)) { // VisibilityMask
            w.writeF32(Fields::f32(script, "visibleDistance", 0));
            w.writeF32(Fields::f32(script, "fogDistance", 0));
        }
        w.writeFlag(false); // StormCloudMask
        w.writeFlag(false); // StormFogMask
        w.writeFlag(false); // StormRealFogMask
        w.writeFlag(false); // WindMask
        return 0;
    }
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
    InteriorInstanceObject() { scopeAlways = true; }
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

// waterBlock.cc
class WaterBlockObject : public SceneObject {
public:
    WaterBlockObject() { scopeAlways = true; }
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
}
