// Precipitation (game/fx/precipitation.cc), server side: the precipitation
// ghost (percentage, colours, drop speeds and counts, radius) and the
// storm controls the clients follow (setPercentage, stormPrecipitation,
// stormShow).
#include "sim/weather.h"
#include "sim/engine_object.h"
#include "sim/game_base.h"
#include "core/console.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include <array>
#include <cstdlib>
#include <memory>
#include <strings.h>

namespace {

constexpr int32_t MaxNumDrops = 2000;
constexpr int MaxNumColor = 3;

class PrecipitationObject : public GameBase {
public:
    // Precipitation's own bits (they share GameBase's low bits as shipped).
    enum NetMaskBits : uint32_t { InitMask = 1u << 0, PercentageMask = 1u << 1, StormMask = 1u << 2, StormShowMask = 1u << 3 };
    PrecipitationObject() {
        ghostable = true;
        scopeAlways = true;
    }
    const char* netClassName() const override { return "Precipitation"; }

    // Precipitation::onAdd: the percentage checked, the server object named
    // "Precipitation", then setDefaultValues.
    void onAdded() override {
        percentage = Fields::f32(script, "percentage", 1.0f);
        if (percentage > 1.0f || percentage < 0.0f) {
            Console::instance().printf(LogLevel::Warn, "Precipitation::onAdd - Percentage is invalid. <= 1.0 or >= 0.0 ");
            percentage = 1.0f;
        }
        ScriptEngine::instance().setObjectName(script, "Precipitation");
        setDefaultValues();
    }

    void processMove(const ClientMoveIn* move) override {
        GameBase::processMove(move);
        stormCurrentTime += 1.0f;
    }

    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override {
        GameBase::packUpdate(connection, mask, w);
        if (w.writeFlag(mask & InitMask)) {
            w.writeF32(percentage);
            w.writeU32((uint32_t)colorCount);
            // Stream::write(ColorF) sends ColorF::operator ColorI: U8(c * 255 + 0.5).
            for (int i = 0; i < colorCount; ++i)
                for (int c = 0; c < 4; ++c) w.writeInt((int32_t)(uint8_t)(int32_t)(colors[i][c] * 255.0f + 0.5f), 8);
            w.writeF32(offsetSpeed);
            w.writeF32(minVelocity);
            w.writeF32(maxVelocity);
            w.writeU32((uint32_t)maxDrops);
            w.writeU32((uint32_t)radius);
            if (w.writeFlag((stormCurrentTime / 32.0f) < stormTime)) {
                w.writeF32(stormCurrentTime);
                w.writeF32(stormEndPercentage);
                w.writeF32(stormTime);
            }
        }
        if (w.writeFlag(mask & StormShowMask)) w.writeInt(stormPrecipitationOn ? 1 : 0, 8);
        if (w.writeFlag(mask & StormMask)) {
            w.writeF32(stormEndPercentage);
            w.writeF32(stormTime);
            stormCurrentTime = 0.0f;
        }
        if (w.writeFlag(mask & PercentageMask)) w.writeF32(percentage);
        return 0;
    }

    void setPercentage(float newPer) {
        if (newPer <= 1.0f && newPer >= 0.0f) {
            percentage = newPer;
            setMaskBits(PercentageMask);
        }
    }
    void stormShow(bool show) {
        stormPrecipitationOn = show;
        setMaskBits(StormShowMask);
    }
    // Precipitation::setupStorm.
    void setupStorm(float newPercentage, float time) {
        stormTime = time;
        if (stormEndPercentage >= 0.0f) percentage = stormEndPercentage;
        stormEndPercentage = newPercentage;
        setMaskBits(StormMask);
    }

private:
    float percentage = 1.0f;
    int colorCount = 0;
    std::array<float, 4> colors[MaxNumColor] = {};
    float offsetSpeed = 0.25f, minVelocity = -1.0f, maxVelocity = -1.0f;
    int32_t maxDrops = -1, radius = -1;
    float stormEndPercentage = -1.0f, stormTime = 0.0f, stormCurrentTime = 0.0f;
    bool stormPrecipitationOn = true;

    // Precipitation::setDefaultValues: the leading colours set, else one by
    // the datablock's type; speeds and radius by type; drops clamped.
    void setDefaultValues() {
        ScriptObject* data = ScriptEngine::instance().findObject(dataBlock().c_str());
        const int type = Fields::s32(data, "type", 0);
        colorCount = 0;
        for (int i = 0; i < MaxNumColor; ++i) {
            colors[i] = Fields::color(script, ("color" + std::to_string(i + 1)).c_str(), {-1.0f, 0.0f, 0.0f, 1.0f});
            if (colors[i][0] < 0.0f) break;
            colorCount++;
        }
        if (colorCount == 0) {
            colors[0] = type == 0 ? std::array<float, 4>{0.6f, 0.6f, 0.6f, 1.0f}
                      : type == 1 ? std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}
                                  : std::array<float, 4>{0.9f, 0.8f, 0.5f, 1.0f};
            colorCount = 1;
        }
        offsetSpeed = Fields::f32(script, "offsetSpeed", 0.25f);
        minVelocity = Fields::f32(script, "minVelocity", -1.0f);
        maxVelocity = Fields::f32(script, "maxVelocity", -1.0f);
        maxDrops = Fields::s32(script, "maxNumDrops", -1);
        radius = Fields::s32(script, "maxRadius", -1);
        if (minVelocity == -1.0f) minVelocity = type == 0 ? 1.25f : 0.25f;
        if (maxVelocity == -1.0f) maxVelocity = type == 0 ? 4.0f : type == 1 ? 1.5f : 1.0f;
        if (radius == -1) radius = type == 1 ? 125 : 80;
        if (maxDrops > MaxNumDrops || maxDrops < 0) maxDrops = MaxNumDrops;
    }
};

} // namespace

void registerPrecipitationNatives(TorqueScript& ts) {
    EngineObjects::registerClass("Precipitation", [] { return std::make_shared<PrecipitationObject>(); });
    auto precipitation = [](const std::vector<VMValue>& args) -> PrecipitationObject* {
        return args.empty() ? nullptr : EngineObjects::get<PrecipitationObject>(args[0].toString());
    };
    ts.registerNative("Precipitation::setPercentage", [precipitation](const std::vector<VMValue>& args) -> VMValue {
        if (auto* p = precipitation(args); p && args.size() > 1) p->setPercentage(args[1].toFloat());
        return VMValue("");
    });
    ts.registerNative("Precipitation::stormPrecipitation", [precipitation](const std::vector<VMValue>& args) -> VMValue {
        if (auto* p = precipitation(args); p && args.size() > 2) p->setupStorm(args[1].toFloat(), args[2].toFloat());
        return VMValue("");
    });
    ts.registerNative("Precipitation::stormShow", [precipitation](const std::vector<VMValue>& args) -> VMValue {
        const std::string show = args.size() > 1 ? args[1].toString() : std::string();
        // dAtob: "true" or a non-zero number.
        if (auto* p = precipitation(args)) p->stormShow(strcasecmp(show.c_str(), "true") == 0 || std::atof(show.c_str()) != 0.0);
        return VMValue("");
    });
}
