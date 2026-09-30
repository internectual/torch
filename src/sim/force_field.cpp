#include "sim/force_field.h"
#include "sim/target_manager.h"
#include "sim/engine_classes.h"
#include "sim/torque_math.h"
#include "net/torque_bit_writer.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include <algorithm>

uint32_t sensorGroupOf(const GameBase& object) {
    // GameBase::getSensorGroup: the target's group, 0 without a target.
    const ServerTargets::Target* target = object.targetId >= 0 ? ServerTargets::serverTarget(object.targetId) : nullptr;
    return target ? target->sensorGroup : 0;
}

void ForceFieldBareObject::open() {
    if (state != Opening && state != Open) {
        state = Opening;
        setMaskBits(StateChangeMask);
    }
}

void ForceFieldBareObject::close() {
    if (state != Closing && state != Closed) {
        state = Closing;
        setMaskBits(StateChangeMask);
    }
}

// ForceFieldBare::processServerTick: the fade toward open or closed.
void ForceFieldBareObject::processMove(const ClientMoveIn*) {
    const int fadeMS = (int)dataFloat("fadeMS", 1000);
    if (state == Opening) {
        position += 32;
        if (position >= fadeMS) {
            state = Open;
            position = fadeMS;
        }
    } else if (state == Closing) {
        position -= 32;
        if (position <= 0) {
            state = Closed;
            position = 0;
        }
    }
}

bool ForceFieldBareObject::isPermiableTo(const GameBase& pass) const {
    // Vehicles never pass through forcefields, even when unpowered.
    if (EngineClasses::isA(pass.script ? pass.script->className : std::string(), "Vehicle")) return false;
    const uint32_t mine = sensorGroupOf(*this), theirs = sensorGroupOf(pass);
    return isOpen() || (dataBool("otherPermiable", false) && mine != theirs) ||
           (dataBool("teamPermiable", false) && mine == theirs);
}

void ForceFieldBareObject::worldBox(Point3F& min, Point3F& max) const {
    min = {1e30f, 1e30f, 1e30f};
    max = {-1e30f, -1e30f, -1e30f};
    for (int c = 0; c < 8; ++c) {
        const float p[3] = {(c & 1) ? scale[0] : 0.0f, (c & 2) ? scale[1] : 0.0f, (c & 4) ? scale[2] : 0.0f};
        float w[3];
        TorqueMath::mulP(transform, p, w);
        min = {std::min(min.x, w[0]), std::min(min.y, w[1]), std::min(min.z, w[2])};
        max = {std::max(max.x, w[0]), std::max(max.y, w[1]), std::max(max.z, w[2])};
    }
}

uint32_t ForceFieldBareObject::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    const uint32_t ret = GameBase::packUpdate(connection, mask, w);
    if (w.writeFlag(mask & InitialUpdateMask)) {
        writeAffineTransform(w);
        writeScale(w);
    } else if (w.writeFlag(mask & TransformMask)) {
        writeAffineTransform(w);
        writeScale(w);
    }
    if (w.writeFlag(mask & StateChangeMask)) {
        w.writeInt(state, 2);
        if (state == Opening || state == Closing) w.writeU32((uint32_t)position);
    }
    return ret;
}

namespace ForceFields {

void gather(const GameBase* mover, const Point3F& min, const Point3F& max,
            std::vector<PlayerPrediction::Triangle>& out) {
    for (auto& [name, object] : ScriptEngine::instance().objects) {
        auto* field = object ? dynamic_cast<ForceFieldBareObject*>(object->engine.get()) : nullptr;
        if (!field) continue;
        if (mover ? field->isPermiableTo(*mover) : field->isOpen()) continue;
        Point3F lo, hi;
        field->worldBox(lo, hi);
        if (lo.x > max.x || hi.x < min.x || lo.y > max.y || hi.y < min.y || lo.z > max.z || hi.z < min.z) continue;
        // The box's six faces, two triangles each, normals outward.
        const Point3F c[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z},
                              {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}};
        static const int quads[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                        {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
        static const Point3F normals[6] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}};
        for (int q = 0; q < 6; ++q) {
            out.push_back({c[quads[q][0]], c[quads[q][1]], c[quads[q][2]], normals[q]});
            out.push_back({c[quads[q][0]], c[quads[q][2]], c[quads[q][3]], normals[q]});
        }
    }
}

} // namespace ForceFields

void registerForceFieldNatives(TorqueScript& ts) {
    EngineObjects::registerClass("ForceFieldBare", [] { return std::make_shared<ForceFieldBareObject>(); });
    using Args = std::vector<VMValue>;
    ts.registerNative("ForceFieldBare::open", [](const Args& args) -> VMValue {
        if (auto* f = args.empty() ? nullptr : EngineObjects::get<ForceFieldBareObject>(args[0].toString())) f->open();
        return VMValue("");
    });
    ts.registerNative("ForceFieldBare::close", [](const Args& args) -> VMValue {
        if (auto* f = args.empty() ? nullptr : EngineObjects::get<ForceFieldBareObject>(args[0].toString())) f->close();
        return VMValue("");
    });
}
