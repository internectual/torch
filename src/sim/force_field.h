#pragma once
// ForceFieldBare (game/forceFieldBare.cc), server side: the open/close
// state, the ghost update, and the box that blocks what it is not
// permeable to.
#include "sim/game_base.h"
#include "game/player_prediction.h"
#include <string>
#include <vector>

class ForceFieldBareObject : public GameBase {
public:
    enum State { Open = 0, Opening = 1, Closing = 2, Closed = 3 };
    enum Masks : uint32_t { TransformMask = GameBase::NextFreeMask, StateChangeMask = GameBase::NextFreeMask << 1 };
    ForceFieldBareObject() { ghostable = true; }
    const char* netClassName() const override { return "ForceFieldBare"; }
    void processMove(const ClientMoveIn* move) override;
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
    void open();
    void close();
    bool isOpen() const { return state == Open; }
    // ForceFieldBare::isPermiableTo: vehicles never pass.
    bool isPermiableTo(const GameBase& pass) const;
    // The world box (the unit box scaled and transformed).
    void worldBox(Point3F& min, Point3F& max) const;

    State state = Closed;
    int position = 0; // mCurrPosition (ms into the fade)
};

namespace ForceFields {
// The boxes (as triangles) of the fields that block `mover` (nullptr: a
// ray, blocked by every field not open), overlapping [min, max].
void gather(const GameBase* mover, const Point3F& min, const Point3F& max,
            std::vector<PlayerPrediction::Triangle>& out, std::vector<const ScriptObject*>* owners = nullptr);
} // namespace ForceFields

uint32_t sensorGroupOf(const GameBase& object);
void registerForceFieldNatives(class TorqueScript& ts);
