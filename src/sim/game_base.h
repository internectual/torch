#pragma once
// GameBase: an object with a datablock, reporting to the datablock's
// namespace (%data.onX(%obj)).
#include "sim/net_object.h"
#include <string>

class GameBase : public SceneObject {
public:
    // Not ghosted until the ShapeBase-family packUpdate writers land.
    GameBase() { ghostable = false; }
    bool processesTicks() const override { return true; }
    // The datablock object's id, or "" when none is set.
    std::string dataBlock() const;
    float dataFloat(const char* field, float fallback) const;
    bool dataBool(const char* field, bool fallback) const;
    // Con::executef(mDataBlock, ..., callback, scriptThis(), args...)
    void callDataBlock(const char* callback, const std::vector<std::string>& extra = {}) const;
    std::string handle() const;
};
