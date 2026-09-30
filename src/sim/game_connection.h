#pragma once
// Server side of a client connection (sim/netConnection.cc, netEvent.cc,
// game/gameConnection.cc): packet sequencing and acknowledgement through
// ConnectionProtocol (V12::ProtocolState), the guaranteed/ordered event
// queues, remote commands and net strings, and the server half of
// GameConnection::writePacket. Packets are written in the form the demo
// reader decodes and handed to the peer by `deliver`.
#include "sim/engine_object.h"
#include "net/torque_bit_writer.h"
#include "net/v12_events.h"
#include "net/v12_protocol.h"
#include <bitset>
#include <deque>
#include <functional>
#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct NetEventOut {
    enum Guarantee { Unguaranteed, Guaranteed, GuaranteedOrdered };
    Guarantee guarantee = GuaranteedOrdered;
    int classIndex = 0; // id - NetEventClassFirst
    int sequence = -1;
    std::function<void(TorqueBitWriter&)> pack;
    std::function<void(bool delivered)> notify;
};

// NetConnection::GhostInfo.
struct GhostInfo {
    enum Flags { InScope = 1, ScopeAlways = 2, NotYetGhosted = 4, Ghosting = 8, KillGhost = 16,
                 KillingGhost = 32, ScopedEvent = 64, ScopeLocalAlways = 128 };
    std::string object; // script object key; empty once detached
    int index = -1;
    uint32_t flags = 0;
    uint32_t updateMask = 0;
    int updateSkipCount = 0;
};

struct ClientMoveIn {
    uint32_t id = 0;
    int16_t yaw = 0, pitch = 0, roll = 0; // 16-bit angle units
    int x = 16, y = 16, z = 16;           // 0..32, 16 = still
    bool freeLook = false;
    bool trigger[6]{};
    int sendCount = 0; // client: packets that carried this move
    // An AIConnection's move is never clamped: its floats reach the control
    // object as they are (the packed fields above are for packing it).
    bool exact = false;
    float fx = 0, fy = 0, fz = 0, fyaw = 0, fpitch = 0, froll = 0;
};

class GameConnection : public EngineObject {
public:
    // Engine event class indices (NetEventClassFirst-relative).
    enum EventClass { GhostingMessage = 4, Gravity = 5, NetString = 7, RemoteCommand = 9,
                      SetMissionCRC = 13, Sim2DAudio = 17, Sim3DAudio = 18, SimDataBlock = 19,
                      SimpleMessage = 22 };

    // NetConnection role: a client's connection on the server (ClientGroup),
    // or the client's ServerConnection.
    bool isServer = true;
    bool local = false;
    std::string address = "Local";
    std::function<void(const std::vector<uint8_t>&)> deliver;

    void postEvent(std::shared_ptr<NetEventOut> event);
    // sendRemoteCommand: tags among argv get their NetStringEvent first.
    void sendRemoteCommand(const std::vector<std::string>& argv);
    void setMissionCRC(uint32_t crc);
    // GameConnection::play2D / play3D (Sim2DAudioEvent / Sim3DAudioEvent).
    void play2D(int profileId);
    void play3D(ScriptObject* profile, const std::array<float, 16>& transform);

    // GameConnection::transmitDataBlocks(seq): DataBlockQueueCount
    // SimDataBlockEvents in flight, each delivery posting the next; the
    // last one calls %conn.dataBlocksDone(seq).
    enum { DataBlockQueueCount = 16, ControlStateSkipAmount = 16 };
    void transmitDataBlocks(uint32_t sequence);
    uint32_t dataBlockSequence = 0;
    int dataBlockModifiedKey = 0;
    int maxDataBlockModifiedKey = 0;

    // NetConnection ghosting (netGhost.cc).
    enum GhostMessage { GhostAlwaysDone = 0, ReadyForNormalGhosts = 1, EndGhosting = 2, GhostAlwaysStarting = 3 };
    void activateGhosting();
    void resetGhosting();
    // An object came into scope (ScopeAlways objects added while scoping).
    void objectInScope(const std::string& object);
    // NetObject::scopeToClient: in scope, and kept there.
    void objectLocalScopeAlways(const std::string& object);

    // GameConnection::setControlObject: the object this client's moves
    // drive, and its scope object.
    void setControlObject(const std::string& object);
    const std::string& controlObject() const { return controlObject_; }
    void setControlObjectDirty() { ++controlObjectModifyKey; }
    int ghostIndex(const std::string& object) const;
    bool isGhosting() const { return ghosting; }

    // NetConnection::checkPacketSend.
    void checkPacketSend(double now);
    // processRawPacket from the client.
    void receivePacket(const uint8_t* data, size_t size);
    // NetConnection::setConnectSequence (clientSeq ^ serverSeq for a network
    // connection): its low bit is the dnet header's connect-sequence bit.
    void setConnectSequence(uint32_t sequence) { protocol.setConnectSequence(sequence); }
    uint32_t getConnectSequence() const { return protocol.connectionSequence(); }

    // Client role (GameConnection::readPacket/writePacket, client half):
    // every server packet goes whole to `onServerPacket` (the demo reader
    // decodes it); moves wait in `moves` until the server acknowledges them.
    std::function<void(const std::vector<uint8_t>&)> onServerPacket;
    enum { MaxMoveCount = 30, MaxMoveQueueSize = 45, MaxMovePacketSends = 4 };
    // getNextMove: dropped while MaxMoveQueueSize moves are unacknowledged.
    bool pushMove(const ClientMoveIn& move);
    // Ghost messages the client decoded (NetConnection::handleGhostMessage).
    void clientGhostMessage(int message, uint32_t sequence, uint32_t count);

    // Server: the next move the client has not had processed. Client: the
    // server's acknowledgement (mLastMoveAck = mFirstMoveIndex).
    uint32_t lastMoveAck = 0;
    std::deque<ClientMoveIn> moves;
    // GameConnection::getMoveList: called as the control object ticks; an
    // AIConnection makes its move here.
    virtual void getMoveList() {}
    bool firstPerson = true;

    // NetConnection::mCurRate / mMaxRate: the packet interval (ms) and the
    // size the packet stream fills to, negotiated through the rate fields
    // each packet may carry.
    struct NetRate {
        uint32_t updateDelay = 102, packetSize = 200;
        bool changed = false;
    };
    NetRate curRate, maxRate;
    GameConnection() { checkMaxRate(); }
    // NetConnection::checkMaxRate: mMaxRate from $pref::Net::PacketRateToClient
    // and $pref::Net::PacketSize.
    void checkMaxRate();

private:
    void validateSendString(const std::string& value);
    void postDataBlock(const std::string& object, uint32_t index, uint32_t total, uint32_t sequence);
    void dataBlockDelivered(uint32_t index, uint32_t sequence);
    void packString(TorqueBitWriter& w, const std::string& value);
    void writePacket(TorqueBitWriter& w, std::vector<std::shared_ptr<NetEventOut>>& sent);
    void writeClientPacket(TorqueBitWriter& w, std::vector<std::shared_ptr<NetEventOut>>& sent);
    void writeEvents(TorqueBitWriter& w, std::vector<std::shared_ptr<NetEventOut>>& sent);
    void clientReadPacket(V12BitStream& stream, const uint8_t* data, size_t size);
    void packetDropped(std::vector<std::shared_ptr<NetEventOut>>& events);
    void packetReceived(std::vector<std::shared_ptr<NetEventOut>>& events);

    V12::ProtocolState protocol;
    std::deque<std::shared_ptr<NetEventOut>> orderedQueue, unorderedQueue;
    std::map<uint32_t, std::vector<std::shared_ptr<NetEventOut>>> inFlight;
    std::string controlObject_;
    uint32_t controlObjectModifyKey = 0, ackedControlObjectModifyKey = 0;
    int controlStateSkipCount = 0;
    std::map<uint32_t, uint32_t> controlKeyInFlight; // packet -> modify key written
    // PacketNotify::rateChanged / maxRateChanged: resent when dropped.
    std::map<uint32_t, std::pair<bool, bool>> rateInFlight;
    // mTargetVisibleMask: the sensor-visible targets the client has, and
    // each packet's change (re-applied when the packet drops).
    uint32_t targetVisibleMask[16]{};
    std::map<uint32_t, std::array<uint32_t, 16>> visibleXorInFlight;
    void writeRates(TorqueBitWriter& w);
    void readRates(V12BitStream& stream);
    size_t packetBudgetBits() const { return (size_t)curRate.packetSize * 8; }
    void writeControlObject(TorqueBitWriter& w, uint32_t& noteKey);
    std::vector<std::shared_ptr<NetEventOut>> notifyList;
    int nextSendEventSeq = 0;
    int lastAckedEventSeq = -1;
    std::bitset<1024> stringSent;
    double lastUpdate = -1.0;
    V12::NetStringTable remoteStrings; // the client's tag ids

    struct GhostRef { int index; uint32_t mask; uint32_t flags; };
    void writeGhosts(TorqueBitWriter& w, std::vector<GhostRef>& refs);
    void scopeScene();
    void ghostPacketDropped(std::vector<GhostRef>& refs);
    void ghostPacketReceived(std::vector<GhostRef>& refs);
    void handleGhostMessage(int message, uint32_t sequence);
    std::vector<GhostInfo> ghosts = std::vector<GhostInfo>(1024);
    std::map<uint32_t, std::vector<GhostRef>> ghostsInFlight;
    bool scoping = false;
    bool ghosting = false;
    uint32_t ghostingSequence = 0;
};

// localConnect: the client half is handed to the game's client, which
// reads its packets.
extern std::function<void(GameConnection& serverConnection)> gLocalClientStarted;
void registerGameConnectionNatives(class TorqueScript& ts);
// clientNetProcess: the ServerConnection's packet chance.
void clientNetProcess(double now);
// serverNetProcess: every connection in ClientGroup gets its packet chance.
void serverNetProcess(double now);
