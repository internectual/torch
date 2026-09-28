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

struct ClientMoveIn {
    uint32_t id = 0;
    int16_t yaw = 0, pitch = 0, roll = 0; // 16-bit angle units
    int x = 16, y = 16, z = 16;           // 0..32, 16 = still
    bool freeLook = false;
    bool trigger[6]{};
};

class GameConnection : public EngineObject {
public:
    // Engine event class indices (NetEventClassFirst-relative).
    enum EventClass { GhostingMessage = 4, Gravity = 5, NetString = 7, RemoteCommand = 9,
                      SetMissionCRC = 13, SimDataBlock = 19, SimpleMessage = 22 };

    bool local = false;
    std::string address = "Local";
    std::function<void(const std::vector<uint8_t>&)> deliver;

    void postEvent(std::shared_ptr<NetEventOut> event);
    // sendRemoteCommand: tags among argv get their NetStringEvent first.
    void sendRemoteCommand(const std::vector<std::string>& argv);
    void setMissionCRC(uint32_t crc);

    // NetConnection::checkPacketSend.
    void checkPacketSend(double now);
    // processRawPacket from the client.
    void receivePacket(const uint8_t* data, size_t size);

    uint32_t lastMoveAck = 0;
    std::deque<ClientMoveIn> moves;
    bool firstPerson = true;
    double updateDelaySeconds = 0.032;

private:
    void validateSendString(const std::string& value);
    void packString(TorqueBitWriter& w, const std::string& value);
    void writePacket(TorqueBitWriter& w, std::vector<std::shared_ptr<NetEventOut>>& sent);
    void packetDropped(std::vector<std::shared_ptr<NetEventOut>>& events);
    void packetReceived(std::vector<std::shared_ptr<NetEventOut>>& events);

    V12::ProtocolState protocol;
    std::deque<std::shared_ptr<NetEventOut>> orderedQueue, unorderedQueue;
    std::map<uint32_t, std::vector<std::shared_ptr<NetEventOut>>> inFlight;
    std::vector<std::shared_ptr<NetEventOut>> notifyList;
    int nextSendEventSeq = 0;
    int lastAckedEventSeq = -1;
    std::bitset<1024> stringSent;
    double lastUpdate = -1.0;
    V12::NetStringTable remoteStrings; // the client's tag ids
};

void registerGameConnectionNatives(class TorqueScript& ts);
// serverNetProcess: every connection in ClientGroup gets its packet chance.
void serverNetProcess(double now);
