// Lightning (game/fx/lightning.cc), server side as the shipped build runs
// it: the storm volume's ghost, a strike every 60 / strikesPerMinute
// seconds at a random point of the volume (the highest damageable object
// under it, struck with chanceToHitTarget, takes the datablock's
// applyDamage), and the LightningStrikeEvent every client receives.
#include "sim/weather.h"
#include "sim/containers.h"
#include "sim/engine_object.h"
#include "sim/game_base.h"
#include "sim/game_connection.h"
#include "sim/nav_graph.h"
#include "sim/player.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include <cstdio>
#include <memory>

namespace {

constexpr uint32_t TickMs = 32;
constexpr int LightningStrikeEventClass = 6; // NetEventClassFirst-relative

class LightningObject : public GameBase {
public:
    LightningObject() {
        ghostable = true;
        scopeAlways = true;
    }
    const char* netClassName() const override { return "Lightning"; }

    void readFields() override {
        GameBase::readFields();
        // Lightning::Lightning: setScale(512, 512, 300) before the fields.
        if (!Fields::present(script, "scale")) {
            scale[0] = 512.0f;
            scale[1] = 512.0f;
            scale[2] = 300.0f;
        }
    }

    int32_t strikesPerMinute() const { return Fields::s32(script, "strikesPerMinute", 12); }
    float chanceToHitTarget() const { return Fields::f32(script, "chanceToHitTarget", 0.5f); }
    float strikeRadius() const { return Fields::f32(script, "strikeRadius", 20.0f); }

    void processMove(const ClientMoveIn* move) override {
        GameBase::processMove(move);
        const int32_t perMinute = strikesPerMinute();
        if (perMinute <= 0) return;
        const int32_t msBetweenStrikes = (int32_t)(60.0 / perMinute * 1000.0);
        lastThink += TickMs;
        if ((int32_t)lastThink > msBetweenStrikes) {
            strikeRandomPoint();
            lastThink -= msBetweenStrikes;
        }
    }

    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override {
        const uint32_t ret = GameBase::packUpdate(connection, mask, w);
        if (w.writeFlag(mask & InitialUpdateMask)) {
            w.writePoint({transform[3], transform[7], transform[11]});
            w.writePoint({scale[0], scale[1], scale[2]});
            w.writeF32(Fields::f32(script, "strikeWidth", 2.5f));
            w.writeF32(chanceToHitTarget());
            w.writeF32(strikeRadius());
            w.writeF32(Fields::f32(script, "boltStartRadius", 20.0f));
            const auto color = Fields::color(script, "color", {1, 1, 1, 1});
            const auto fade = Fields::color(script, "fadeColor", {0.1f, 0.1f, 1, 1});
            w.writeF32(color[0]);
            w.writeF32(color[1]);
            w.writeF32(color[2]);
            w.writeF32(fade[0]);
            w.writeF32(fade[1]);
            w.writeF32(fade[2]);
            w.writeInt(Fields::boolean(script, "useFog", true) ? 1 : 0, 8);
            w.writeU32((uint32_t)strikesPerMinute());
        }
        return ret;
    }

    // Lightning::warningFlashes: an event with no strike point or target.
    void warningFlashes() { postStrikeEvents(0.0f, 0.0f, std::string()); }

    void strikeRandomPoint();

private:
    uint32_t lastThink = 0;
    void postStrikeEvents(float x, float y, const std::string& target);
    void applyDamage(const Point3F& hitPosition, const Point3F& hitNormal, ScriptObject* hitObject);
};

void LightningObject::strikeRandomPoint() {
    auto& rand = Nav::gRandGen();
    Point3F strikePoint{rand.randF(), rand.randF(), 0.0f};
    strikePoint = {strikePoint.x * scale[0] + transform[3] - scale[0] * 0.5f,
                   strikePoint.y * scale[1] + transform[7] - scale[1] * 0.5f, transform[11]};
    // check if an object is within target range
    const float boxWidth = strikeRadius() * 2;
    const Point3F lo{strikePoint.x - boxWidth * 0.5f, strikePoint.y - boxWidth * 0.5f, strikePoint.z - scale[2] * 0.5f};
    const Point3F hi{strikePoint.x + boxWidth * 0.5f, strikePoint.y + boxWidth * 0.5f, strikePoint.z + scale[2] * 0.5f};
    ScriptObject* highestObj = nullptr;
    float highestPnt = 0.0f;
    for (ScriptObject* object : SimContainer::findObjects(lo, hi, SimContainer::DamageableMask)) {
        const Point3F objectCenter = SimContainer::worldBoxCenter(object);
        // check if object can be struck
        const Point3F start{objectCenter.x, objectCenter.y, scale[2] * 0.5f + transform[11]};
        const Point3F end{objectCenter.x, objectCenter.y, -scale[2] * 0.5f + transform[11]};
        SimContainer::RayInfo info;
        if (SimContainer::castRay(start, end, 0xFFFFFFFFu, info) && info.object == object) {
            if (!highestObj || objectCenter.z > highestPnt) {
                highestObj = object;
                highestPnt = objectCenter.z;
            }
        }
    }
    // hah haaaaa, we have a target!
    std::string target;
    if (highestObj && rand.randF() <= chanceToHitTarget()) {
        const Point3F objectCenter = SimContainer::worldBoxCenter(highestObj);
        // A player nobody controls (warmup) is spared.
        auto* player = dynamic_cast<PlayerObject*>(highestObj->engine.get());
        if (!player || !player->controllingClient.empty()) {
            applyDamage(objectCenter, {0.0f, 0.0f, 1.0f}, highestObj);
            target = ScriptEngine::instance().objectKey(highestObj);
        }
    }
    postStrikeEvents(strikePoint.x, strikePoint.y, target);
}

void LightningObject::applyDamage(const Point3F& hitPosition, const Point3F& hitNormal, ScriptObject* hitObject) {
    if (!hitObject) return;
    char pos[64], normal[64];
    std::snprintf(pos, sizeof(pos), "%f %f %f", hitPosition.x, hitPosition.y, hitPosition.z);
    std::snprintf(normal, sizeof(normal), "%f %f %f", hitNormal.x, hitNormal.y, hitNormal.z);
    callDataBlock("applyDamage", {std::to_string(ScriptEngine::instance().objectId(hitObject)), pos, normal});
}

// LightningStrikeEvent::pack: the lightning's ghost, the strike point
// (written with writeFloat(10) as shipped, though it is a world position)
// and the struck object's ghost.
void LightningObject::postStrikeEvents(float x, float y, const std::string& target) {
    auto& engine = ScriptEngine::instance();
    ScriptObject* group = engine.findObject("ClientGroup");
    if (!group) return;
    const std::string self = engine.objectKey(script);
    const int count = group->internals["__childCount"].toInt();
    for (int i = 0; i < count; ++i) {
        auto* connection = EngineObjects::get<GameConnection>(group->internals["__child" + std::to_string(i)].toString());
        if (!connection) continue;
        auto event = std::make_shared<NetEventOut>();
        event->classIndex = LightningStrikeEventClass;
        event->pack = [connection, self, x, y, target](TorqueBitWriter& w) {
            const int id = connection->ghostIndex(self);
            if (!w.writeFlag(id != -1)) return;
            w.writeRangedU32((uint32_t)id, 0, 1024);
            w.writeFloat(x, 10);
            w.writeFloat(y, 10);
            const int targetGhost = target.empty() ? -1 : connection->ghostIndex(target);
            if (w.writeFlag(targetGhost != -1)) w.writeRangedU32((uint32_t)targetGhost, 0, 1024);
        };
        connection->postEvent(event);
    }
}

} // namespace

void registerLightningNatives(TorqueScript& ts) {
    EngineObjects::registerClass("Lightning", [] { return std::make_shared<LightningObject>(); });
    auto lightning = [](const std::vector<VMValue>& args) -> LightningObject* {
        return args.empty() ? nullptr : EngineObjects::get<LightningObject>(args[0].toString());
    };
    ts.registerNative("Lightning::warningFlashes", [lightning](const std::vector<VMValue>& args) -> VMValue {
        if (auto* l = lightning(args)) l->warningFlashes();
        return VMValue("");
    });
    ts.registerNative("Lightning::strikeRandomPoint", [lightning](const std::vector<VMValue>& args) -> VMValue {
        if (auto* l = lightning(args)) l->strikeRandomPoint();
        return VMValue("");
    });
    // Lightning::strikeObject: "not yet done" in the engine; nothing strikes.
    ts.registerNative("Lightning::strikeObject", [](const std::vector<VMValue>&) -> VMValue { return VMValue(""); });
}
