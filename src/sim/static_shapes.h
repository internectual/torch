#pragma once
// StaticShape (game/staticShape.cc), Turret (game/turret.cc) and Item
// (game/item.cc) ghosts in the layout the Tribes 2 client reads.
#include "sim/shape_base.h"

class StaticShapeObject : public ShapeBase {
public:
    enum StaticShapeMasks : uint32_t { PositionMask = ShapeBase::NextFreeMask, NextFreeMask = ShapeBase::NextFreeMask << 1 };
    explicit StaticShapeObject(const char* netClass = "StaticShape", bool always = false) : netClass(netClass) {
        ghostable = true;
        scopeAlways = always;
    }
    const char* netClassName() const override { return netClass; }
    bool powered = false; // mPowered
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;

private:
    const char* netClass;
};

class TurretObject : public StaticShapeObject {
public:
    TurretObject() : StaticShapeObject("Turret") {}
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
};

class ItemObject : public ShapeBase {
public:
    enum ItemMasks : uint32_t {
        HiddenMask = ShapeBase::NextFreeMask,
        ThrowSrcMask = ShapeBase::NextFreeMask << 1,
        PositionMask = ShapeBase::NextFreeMask << 2,
        RotationMask = ShapeBase::NextFreeMask << 3,
    };
    ItemObject() { ghostable = true; }
    const char* netClassName() const override { return "Item"; }
    void readFields() override;
    bool rotate = false, isStatic = false, collideable = false, atRest = true;
    float velocity[3] = {0, 0, 0};
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
};

void registerStaticShapeNatives(class TorqueScript& ts);
