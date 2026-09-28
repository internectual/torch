#pragma once
// GameBase: an object with a datablock, reporting to the datablock's
// namespace (%data.onX(%obj)).
#include "sim/net_object.h"
#include <string>

struct ClientMoveIn;

class GameBase : public SceneObject {
public:
    enum MaskBits : uint32_t {
        InitialUpdateMask = 1u << 0,
        DataBlockMask = 1u << 1,
        ExtendedInfoMask = 1u << 2,
        NextFreeMask = 1u << 3,
    };
    // Not ghosted until its class's packUpdate writer lands.
    GameBase() { ghostable = false; }
    bool processesTicks() const override { return true; }
    // ProcessList::advanceObjects: a controlled object ticks once per move
    // from its connection, others with no move.
    void processTick() override { processMove(nullptr); }
    virtual void processMove(const ClientMoveIn* move) { (void)move; }
    // The datablock object's id, or "" when none is set.
    std::string dataBlock() const;
    float dataFloat(const char* field, float fallback) const;
    bool dataBool(const char* field, bool fallback) const;
    // Con::executef(mDataBlock, ..., callback, scriptThis(), args...)
    void callDataBlock(const char* callback, const std::vector<std::string>& extra = {}) const;
    std::string handle() const;

    // The connection controlling this object (its key), empty when none.
    std::string controllingClient;
    int targetId = -1;
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
    // GameBase::writePacketData: the control object's state for its client.
    // Returns false when it has to be sent again soon.
    virtual bool writePacketData(GameConnection& connection, TorqueBitWriter& w) { (void)connection; (void)w; return true; }
};
