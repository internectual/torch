#include "sim/game_connection.h"
#include "sim/engine_classes.h"
#include "sim/target_manager.h"
#include "sim/net_string_table.h"
#include "sim/net_object.h"
#include "sim/datablock_pack.h"
#include "sim/shape_base.h"
#include "sim/containers.h"
#include "sim/player.h"
#include "sim/torque_math.h"
#include "net/remote_command.h"
#include "net/v12_bitstream.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "core/console.h"
#include "core/timer.h"
#include <algorithm>

namespace {
constexpr int NetEventClassBits = 6;
// NetConnection.cc's gPacketUpdateDelayToServer (1024 / PacketRateToServer).
uint32_t gPacketUpdateDelayToServer = 32;

// A $pref::Net value; the engine variable's default until a script sets it.
uint32_t netPref(const char* name, uint32_t fallback) {
    auto* ts = ScriptEngine::instance().ts();
    const VMValue value = ts ? ts->getGlobal(name) : VMValue();
    return value.toString().empty() ? fallback : (uint32_t)std::max(0, value.toInt());
}
}

void GameConnection::checkMaxRate() {
    const uint32_t rateToServer = std::clamp(netPref("$pref::Net::PacketRateToServer", 32), 8u, 32u);
    const uint32_t rateToClient = std::clamp(netPref("$pref::Net::PacketRateToClient", 10), 1u, 32u);
    const uint32_t packetSize = std::clamp(netPref("$pref::Net::PacketSize", 200), 100u, 450u);
    gPacketUpdateDelayToServer = 1024 / rateToServer;
    const uint32_t toClientUpdateDelay = 1024 / rateToClient;
    if (maxRate.updateDelay != toClientUpdateDelay || maxRate.packetSize != packetSize) {
        maxRate.updateDelay = toClientUpdateDelay;
        maxRate.packetSize = packetSize;
        maxRate.changed = true;
    }
}

// NetConnection::checkPacketSend: the changed rates, before writePacket.
void GameConnection::writeRates(TorqueBitWriter& w) {
    rateInFlight[protocol.lastSent()] = {curRate.changed, maxRate.changed};
    if (w.writeFlag(curRate.changed)) {
        w.writeInt((int32_t)curRate.updateDelay, 10);
        w.writeInt((int32_t)curRate.packetSize, 10);
        curRate.changed = false;
    }
    if (w.writeFlag(maxRate.changed)) {
        w.writeInt((int32_t)maxRate.updateDelay, 10);
        w.writeInt((int32_t)maxRate.packetSize, 10);
        maxRate.changed = false;
    }
}

// NetConnection::handlePacket: the sender's current rate, and its maximum
// clamped to ours.
void GameConnection::readRates(V12BitStream& stream) {
    if (stream.readFlag()) {
        curRate.updateDelay = stream.readUnsigned(10);
        curRate.packetSize = stream.readUnsigned(10);
    }
    if (stream.readFlag()) {
        uint32_t omaxDelay = stream.readUnsigned(10);
        uint32_t omaxSize = stream.readUnsigned(10);
        if (omaxDelay < maxRate.updateDelay) omaxDelay = maxRate.updateDelay;
        if (omaxSize > maxRate.packetSize) omaxSize = maxRate.packetSize;
        if (omaxDelay != curRate.updateDelay || omaxSize != curRate.packetSize) {
            curRate.updateDelay = omaxDelay;
            curRate.packetSize = omaxSize;
            curRate.changed = true;
        }
    }
}

void GameConnection::postEvent(std::shared_ptr<NetEventOut> event) {
    if (!event) return;
    if (event->guarantee == NetEventOut::GuaranteedOrdered) {
        event->sequence = nextSendEventSeq++;
        orderedQueue.push_back(std::move(event));
    } else {
        unorderedQueue.push_back(std::move(event));
    }
}

void GameConnection::validateSendString(const std::string& value) {
    if (!NetStrings::isTag(value)) return;
    const uint32_t id = NetStrings::tagId(value);
    if (id == 0 || id >= NetStrings::MaxStrings || stringSent[id]) return;
    stringSent[id] = true;
    const std::string* text = NetStrings::lookup(id);
    auto event = std::make_shared<NetEventOut>();
    event->classIndex = NetString;
    const std::string copy = text ? *text : std::string();
    const bool hasText = text != nullptr;
    event->pack = [id, copy, hasText](TorqueBitWriter& w) {
        w.writeInt((int32_t)id, 10);
        if (w.writeFlag(hasText)) w.writeString(copy);
    };
    postEvent(event);
}

// NetConnection::packString.
void GameConnection::packString(TorqueBitWriter& w, const std::string& value) {
    enum { NullString = 0, CString = 1, TagString = 2, Integer = 3 };
    if (value.empty()) { w.writeInt(NullString, 2); return; }
    if (NetStrings::isTag(value)) {
        w.writeInt(TagString, 2);
        w.writeInt((int32_t)NetStrings::tagId(value), 10);
        return;
    }
    if (value[0] == '-' || (value[0] >= '0' && value[0] <= '9')) {
        const long num = std::strtol(value.c_str(), nullptr, 10);
        if (std::to_string((int32_t)num) == value) {
            w.writeInt(Integer, 2);
            int32_t n = (int32_t)num;
            if (w.writeFlag(n < 0)) n = -n;
            if (w.writeFlag(n < 128)) { w.writeInt(n, 7); return; }
            if (w.writeFlag(n < 32768)) { w.writeInt(n, 15); return; }
            w.writeInt(n, 31);
            return;
        }
    }
    w.writeInt(CString, 2);
    w.writeString(value);
}

void GameConnection::sendRemoteCommand(const std::vector<std::string>& argv) {
    if (argv.empty() || !NetStrings::isTag(argv[0])) {
        Console::instance().printf(LogLevel::Error, "Remote Command Error - command must be a tag.");
        return;
    }
    // Trailing empty arguments are not sent.
    std::vector<std::string> args = argv;
    while (args.size() > 1 && args.back().empty()) args.pop_back();
    if (args.size() > 20) args.resize(20);
    for (const auto& arg : args) validateSendString(arg);
    auto event = std::make_shared<NetEventOut>();
    event->classIndex = RemoteCommand;
    event->pack = [this, args](TorqueBitWriter& w) {
        w.writeInt((int32_t)args.size(), 5);
        for (const auto& arg : args) packString(w, arg);
    };
    postEvent(event);
}

void GameConnection::setMissionCRC(uint32_t crc) {
    auto event = std::make_shared<NetEventOut>();
    event->classIndex = SetMissionCRC;
    event->pack = [crc](TorqueBitWriter& w) { w.writeU32(crc); };
    postEvent(event);
}

// --- Datablocks (gameConnection.cc, gameConnectionEvents.cc) ---------------

static std::vector<std::string> dataBlockGroupMembers() {
    std::vector<std::string> members;
    ScriptObject* group = ScriptEngine::instance().findObject("DataBlockGroup");
    if (!group) return members;
    const int count = group->internals["__childCount"].toInt();
    for (int i = 0; i < count; ++i) members.push_back(group->internals["__child" + std::to_string(i)].toString());
    return members;
}

static int modifiedKey(const ScriptObject* data) {
    auto it = data->internals.find("__datablockKey");
    return it == data->internals.end() ? 0 : it->second.toInt();
}

static uint32_t dataBlockIdOf(const std::string& handle) {
    ScriptObject* object = ScriptEngine::instance().findObject(handle.c_str());
    if (!object || !object->internals.count("__datablockKey")) return 0;
    const int id = ScriptEngine::instance().objectId(object);
    return id >= (int)DataBlockPack::ObjectIdFirst && id <= (int)DataBlockPack::ObjectIdLast ? (uint32_t)id : 0;
}

void GameConnection::transmitDataBlocks(uint32_t sequence) {
    dataBlockSequence = sequence;
    const auto members = dataBlockGroupMembers();
    if (members.empty()) return;
    // The first one the client does not have.
    size_t i = 0;
    for (; i < members.size(); ++i) {
        ScriptObject* data = ScriptEngine::instance().findObject(members[i].c_str());
        if (data && modifiedKey(data) > dataBlockModifiedKey) break;
    }
    if (i == members.size()) {
        if (auto* ts = ScriptEngine::instance().ts())
            ts->callObjectMethod(ScriptEngine::instance().objectKey(script), "dataBlocksDone",
                                 {VMValue(std::to_string(sequence))});
        return;
    }
    maxDataBlockModifiedKey = dataBlockModifiedKey;
    const size_t max = std::min(i + DataBlockQueueCount, members.size());
    for (; i < max; ++i) postDataBlock(members[i], (uint32_t)i, (uint32_t)members.size(), sequence);
}

void GameConnection::postDataBlock(const std::string& object, uint32_t index, uint32_t total, uint32_t sequence) {
    auto event = std::make_shared<NetEventOut>();
    event->classIndex = SimDataBlock;
    event->pack = [this, object, index, total](TorqueBitWriter& w) {
        ScriptObject* data = ScriptEngine::instance().findObject(object.c_str());
        const int key = data ? modifiedKey(data) : 0;
        if (!w.writeFlag(data && dataBlockModifiedKey < key)) return;
        if (key > maxDataBlockModifiedKey) maxDataBlockModifiedKey = key;
        const int id = ScriptEngine::instance().objectId(data);
        const int classIndex = DataBlockPack::classIndex(data->className);
        const DataBlockPack::PackFn* pack = DataBlockPack::find(data->className);
        if (classIndex < 0 || !pack)
            Console::instance().printf(LogLevel::Error, "SimDataBlockEvent: no packData for %s (%s)",
                                       data->className.c_str(), object.c_str());
        w.writeInt(id - (int)DataBlockPack::ObjectIdFirst, 11);
        w.writeInt(std::max(classIndex, 0), 7);
        w.writeInt((int32_t)index, 11);
        w.writeInt((int32_t)total, 12);
        if (!pack) return;
        DataBlockPack::Context context(data, w, dataBlockIdOf);
        (*pack)(context);
    };
    event->notify = [this, index, sequence](bool delivered) {
        if (delivered) dataBlockDelivered(index, sequence);
    };
    postEvent(event);
}

// SimDataBlockEvent::notifyDelivered.
void GameConnection::dataBlockDelivered(uint32_t index, uint32_t sequence) {
    if (dataBlockSequence != sequence) return;
    const auto members = dataBlockGroupMembers();
    const size_t next = index + DataBlockQueueCount;
    if (!members.empty() && index == members.size() - 1) {
        dataBlockModifiedKey = maxDataBlockModifiedKey;
        if (auto* ts = ScriptEngine::instance().ts())
            ts->callObjectMethod(ScriptEngine::instance().objectKey(script), "dataBlocksDone",
                                 {VMValue(std::to_string(sequence))});
    }
    if (members.size() <= next) return;
    postDataBlock(members[next], (uint32_t)next, (uint32_t)members.size(), sequence);
}

// NetConnection::eventPacketDropped: ordered events go back in sequence,
// guaranteed ones to the front, unguaranteed ones are lost.
void GameConnection::packetDropped(std::vector<std::shared_ptr<NetEventOut>>& events) {
    std::vector<std::shared_ptr<NetEventOut>> ordered;
    for (auto& event : events) {
        switch (event->guarantee) {
            case NetEventOut::GuaranteedOrdered: ordered.push_back(event); break;
            case NetEventOut::Guaranteed: unorderedQueue.push_front(event); break;
            case NetEventOut::Unguaranteed: if (event->notify) event->notify(false); break;
        }
    }
    for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
        auto pos = std::find_if(orderedQueue.begin(), orderedQueue.end(),
            [&](const auto& queued) { return queued->sequence > (*it)->sequence; });
        orderedQueue.insert(pos, *it);
    }
}

// NetConnection::eventPacketReceived: ordered events are acknowledged in
// sequence.
void GameConnection::packetReceived(std::vector<std::shared_ptr<NetEventOut>>& events) {
    for (auto& event : events) {
        if (event->guarantee != NetEventOut::GuaranteedOrdered) {
            if (event->notify) event->notify(true);
            continue;
        }
        notifyList.push_back(event);
    }
    std::sort(notifyList.begin(), notifyList.end(),
              [](const auto& a, const auto& b) { return a->sequence < b->sequence; });
    while (!notifyList.empty() && notifyList.front()->sequence == lastAckedEventSeq + 1) {
        ++lastAckedEventSeq;
        if (notifyList.front()->notify) notifyList.front()->notify(true);
        notifyList.erase(notifyList.begin());
    }
}

void GameConnection::writePacket(TorqueBitWriter& w, std::vector<std::shared_ptr<NetEventOut>>& sent) {
    writeRates(w);
    w.setStringBuffer(true);
    w.clearCompression();
    // GameConnection::writePacket, server half: the moves processed so far.
    w.writeU32(lastMoveAck - (uint32_t)moves.size());
    uint32_t noteKey = 0;
    writeControlObject(w, noteKey);
    if (noteKey) controlKeyInFlight[protocol.lastSent()] = noteKey;
    // The visible target mask: the words of the client's sensor group's
    // ping mask that changed since the last packet.
    {
        const uint32_t* ping = ServerTargets::sensorGroupPingMask(ServerTargets::connectionSensorGroup(*this));
        std::array<uint32_t, 16> changed{};
        for (uint32_t i = 0; i < 16; ++i) {
            changed[i] = ping[i] ^ targetVisibleMask[i];
            targetVisibleMask[i] = ping[i];
            if (changed[i]) {
                w.writeFlag(true);
                w.writeInt((int32_t)i, 4);
                w.writeInt((int32_t)changed[i], 32);
            }
        }
        w.writeFlag(false);
        visibleXorInFlight[protocol.lastSent()] = changed;
    }
    w.writeFlag(false); // camera fov
    writeEvents(w, sent);
}

// The control object's flash, lock and tracking state, then its packet data
// when it changed (or every ControlStateSkipAmount packets), otherwise its
// position as the compression point.
void GameConnection::writeControlObject(TorqueBitWriter& w, uint32_t& noteKey) {
    auto* control = controlObject_.empty() ? nullptr : EngineObjects::get<ShapeBase>(controlObject_);
    const int ghost = control ? ghostIndex(controlObject_) : -1;
    if (control) {
        const float flash = control->damageFlash, whiteOut = control->whiteOut;
        if (w.writeFlag(flash != 0 || whiteOut != 0)) {
            if (w.writeFlag(flash != 0)) w.writeFloat(flash, 7);
            if (w.writeFlag(whiteOut != 0)) w.writeFloat(whiteOut / 1.5f, 7);
        }
        // The lock and homing counts on the control object and its mount.
        int lockCount = control->lockCount, homingCount = control->homingCount;
        if (auto* mount = control->mount.empty() ? nullptr : EngineObjects::get<ShapeBase>(control->mount)) {
            lockCount += mount->lockCount;
            homingCount += mount->homingCount;
        }
        if (w.writeFlag(lockCount | homingCount)) {
            w.writeFlag(lockCount > 0);
            w.writeFlag(homingCount > 0);
        }
        // The seeker's lock, on what the control object drives. The shipped
        // build follows the tracking flag with a position this port has not
        // recovered, so tracking is not sent.
        ShapeBase* co = control;
        if (auto* player = dynamic_cast<PlayerObject*>(control); player && !player->controlObject.empty())
            if (auto* driven = EngineObjects::get<ShapeBase>(player->controlObject)) co = driven;
        if (w.writeFlag(co->lockMode != ShapeBase::NotLocked)) {
            w.writeFlag(false); // tracking
            w.writeRangedU32((uint32_t)co->lockMode, ShapeBase::NotLocked, ShapeBase::LockPosition);
            if (co->lockMode == ShapeBase::LockObject) {
                const int gi = co->lockTarget.empty() ? -1 : ghostIndex(co->lockTarget);
                if (w.writeFlag(gi != -1)) w.writeRangedU32((uint32_t)gi, 0, 1023);
            } else if (co->lockMode == ShapeBase::LockPosition) {
                w.writeF32(co->lockPosition.x);
                w.writeF32(co->lockPosition.y);
                w.writeF32(co->lockPosition.z);
            }
        }
    } else {
        w.writeFlag(false);
        w.writeFlag(false);
        w.writeFlag(false);
    }
    w.writeFlag(false); // pinged
    w.writeFlag(false); // jammed
    if (!w.writeFlag(ghost != -1)) return;
    if (w.writeFlag(controlObjectModifyKey != ackedControlObjectModifyKey ||
                    controlStateSkipCount >= ControlStateSkipAmount)) {
        w.writeInt(ghost, 10);
        if (control->writePacketData(*this, w)) {
            noteKey = controlObjectModifyKey;
            controlStateSkipCount = 0;
        }
    } else {
        ++controlStateSkipCount;
        w.writeF32(control->transform[3]);
        w.writeF32(control->transform[7]);
        w.writeF32(control->transform[11]);
        w.setCompressionPoint({control->transform[3], control->transform[7], control->transform[11]});
    }
}

void GameConnection::setControlObject(const std::string& object) {
    if (object == controlObject_) return;
    if (isServer) ++controlObjectModifyKey;
    const std::string self = ScriptEngine::instance().objectKey(script);
    if (auto* old = controlObject_.empty() ? nullptr : EngineObjects::get<GameBase>(controlObject_))
        old->controllingClient.clear();
    if (auto* next = object.empty() ? nullptr : EngineObjects::get<GameBase>(object)) {
        if (!next->controllingClient.empty() && next->controllingClient != self)
            if (auto* other = EngineObjects::get<GameConnection>(next->controllingClient))
                other->setControlObject({});
        next->controllingClient = self;
    }
    controlObject_ = object;
    // setScopeObject: the scope object is always in scope.
    if (!object.empty()) objectInScope(object);
}

// GameConnection::writePacket, client half (Tribes 2: first-person flag and
// control-object checksum, then moveWritePacket and the camera fov).
void GameConnection::writeClientPacket(TorqueBitWriter& w, std::vector<std::shared_ptr<NetEventOut>>& sent) {
    writeRates(w);
    w.setStringBuffer(true);
    w.clearCompression();
    w.writeFlag(firstPerson);
    w.writeU32(0); // control object checksum
    size_t offset = 0;
    while (offset < moves.size() && moves[offset].sendCount >= MaxMovePacketSends) ++offset;
    if (offset == moves.size() && !moves.empty()) --offset;
    const size_t count = std::min(moves.size() - offset, (size_t)MaxMoveCount);
    w.writeU32(lastMoveAck + (uint32_t)offset);
    w.writeInt((int32_t)count, 5);
    for (size_t i = 0; i < count; ++i) {
        ClientMoveIn& move = moves[offset + i];
        ++move.sendCount;
        // Move::pack.
        if (w.writeFlag(move.yaw != 0)) w.writeInt((uint16_t)move.yaw, 16);
        if (w.writeFlag(move.pitch != 0)) w.writeInt((uint16_t)move.pitch, 16);
        if (w.writeFlag(move.roll != 0)) w.writeInt((uint16_t)move.roll, 16);
        w.writeInt(move.x, 6);
        w.writeInt(move.y, 6);
        w.writeInt(move.z, 6);
        w.writeFlag(move.freeLook);
        for (bool trigger : move.trigger) w.writeFlag(trigger);
    }
    w.writeFlag(false); // camera fov
    writeEvents(w, sent);
}

bool GameConnection::pushMove(const ClientMoveIn& move) {
    if (moves.size() > MaxMoveQueueSize) return false;
    ClientMoveIn queued = move;
    queued.id = lastMoveAck + (uint32_t)moves.size();
    queued.sendCount = 0;
    moves.push_back(queued);
    return true;
}

// NetConnection::eventWritePacket.
void GameConnection::writeEvents(TorqueBitWriter& w, std::vector<std::shared_ptr<NetEventOut>>& sent) {
    while (!unorderedQueue.empty() && w.bitPosition() <= packetBudgetBits()) {
        auto event = unorderedQueue.front();
        unorderedQueue.pop_front();
        w.writeFlag(true);
        w.writeInt(event->classIndex, NetEventClassBits);
        if (event->pack) event->pack(w);
        sent.push_back(event);
    }
    w.writeFlag(false);
    int prevSeq = -2;
    while (!orderedQueue.empty() && w.bitPosition() <= packetBudgetBits()) {
        auto event = orderedQueue.front();
        if (event->sequence > lastAckedEventSeq + 126) break;
        orderedQueue.pop_front();
        w.writeFlag(true);
        if (!w.writeFlag(event->sequence == prevSeq + 1)) w.writeInt(event->sequence & 0x7f, 7);
        prevSeq = event->sequence;
        w.writeInt(event->classIndex, NetEventClassBits);
        if (event->pack) event->pack(w);
        sent.push_back(event);
    }
    w.writeFlag(false);
}

void GameConnection::checkPacketSend(double now) {
    if (!deliver) return;
    const uint32_t delay = isServer ? curRate.updateDelay : gPacketUpdateDelayToServer;
    if (lastUpdate >= 0.0 && now < lastUpdate + delay / 1000.0) return;
    if (protocol.windowFull()) return;
    lastUpdate = now;
    TorqueBitWriter w;
    protocol.writePacketHeader(w.raw(), V12::PacketType::Data);
    std::vector<std::shared_ptr<NetEventOut>> sent;
    std::vector<GhostRef> refs;
    if (isServer) {
        writePacket(w, sent);
        writeGhosts(w, refs);
    } else {
        writeClientPacket(w, sent);
    }
    inFlight[protocol.lastSent()] = std::move(sent);
    ghostsInFlight[protocol.lastSent()] = std::move(refs);
    sendTimeInFlight[protocol.lastSent()] = Timer::now() * 1000.0;
    deliver(w.data());
}

void GameConnection::receivePacket(const uint8_t* data, size_t size) {
    V12BitStream stream(data, size);
    V12::DnetHeader header;
    if (!V12::readDnetHeader(stream, header)) return;
    const auto result = protocol.processReceived(header);
    for (const auto& ack : result.acknowledgements) {
        // NetConnection::handleNotify (the shipped build): the round trip
        // averages in each ack; the loss is the dropped share of the last 32.
        if (auto it = sendTimeInFlight.find(ack.sequence); it != sendTimeInFlight.end()) {
            if (ack.acknowledged) {
                roundTripTime = (roundTripTime + (float)(Timer::now() * 1000.0 - it->second)) * 0.5f;
                packetLossHistory <<= 1;
            } else {
                packetLossHistory = (packetLossHistory << 1) | 1;
            }
            packetLoss = (float)__builtin_popcount(packetLossHistory) * 0.03125f;
            sendTimeInFlight.erase(it);
        }
        if (auto it = inFlight.find(ack.sequence); it != inFlight.end()) {
            if (ack.acknowledged) packetReceived(it->second);
            else packetDropped(it->second);
            inFlight.erase(it);
        }
        if (auto it = visibleXorInFlight.find(ack.sequence); it != visibleXorInFlight.end()) {
            if (!ack.acknowledged)
                for (uint32_t i = 0; i < 16; ++i) targetVisibleMask[i] ^= it->second[i];
            visibleXorInFlight.erase(it);
        }
        if (auto it = rateInFlight.find(ack.sequence); it != rateInFlight.end()) {
            if (it->second.first && !ack.acknowledged) curRate.changed = true;
            if (it->second.second && !ack.acknowledged) maxRate.changed = true;
            rateInFlight.erase(it);
        }
        if (auto it = controlKeyInFlight.find(ack.sequence); it != controlKeyInFlight.end()) {
            if (ack.acknowledged) ackedControlObjectModifyKey = it->second;
            controlKeyInFlight.erase(it);
        }
        if (auto it = ghostsInFlight.find(ack.sequence); it != ghostsInFlight.end()) {
            if (ack.acknowledged) ghostPacketReceived(it->second);
            else ghostPacketDropped(it->second);
            ghostsInFlight.erase(it);
        }
    }
    if (!result.accepted) return;
    if (header.packetType == V12::PacketType::Ping) {
        if (deliver) deliver(protocol.buildPacket(V12::PacketType::Ack));
        return;
    }
    if (!result.dispatchData || header.packetType != V12::PacketType::Data) return;
    if (!isServer) {
        clientReadPacket(stream, data, size);
        return;
    }
    readRates(stream);
    // GameConnection::readPacket, client half (Tribes 2: first-person flag,
    // control-object checksum, then moveReadPacket).
    firstPerson = stream.readFlag();
    stream.readUnsigned(32);
    const uint32_t start = stream.readUnsigned(32);
    const uint32_t count = stream.readUnsigned(5);
    int skip = (int)lastMoveAck - (int)start;
    if (skip < 0) { lastMoveAck = start; skip = 0; }
    for (uint32_t i = 0; i < count && !stream.failed(); ++i) {
        ClientMoveIn move;
        if (stream.readFlag()) move.yaw = (int16_t)stream.readUnsigned(16);
        if (stream.readFlag()) move.pitch = (int16_t)stream.readUnsigned(16);
        if (stream.readFlag()) move.roll = (int16_t)stream.readUnsigned(16);
        move.x = (int)stream.readUnsigned(6);
        move.y = (int)stream.readUnsigned(6);
        move.z = (int)stream.readUnsigned(6);
        move.freeLook = stream.readFlag();
        for (bool& trigger : move.trigger) trigger = stream.readFlag();
        if ((int)i < skip) continue;
        move.id = start + i;
        moves.push_back(move);
        ++lastMoveAck;
    }
    if (stream.readFlag()) stream.readUnsigned(8); // camera fov
    if (stream.failed()) return;
    stream.setStringBuffer(true);
    std::vector<V12::ServerEvent> events;
    if (!V12::readServerEvents(stream, remoteStrings, events)) return;
    auto* ts = ScriptEngine::instance().ts();
    if (!ts) return;
    for (const auto& event : events) {
        if (event.classId == GhostingMessage && event.hasGhostingMessage)
            handleGhostMessage(event.ghostMessage, event.ghostSequence);
        if (event.classId != RemoteCommand || event.rawArguments.empty()) continue;
        // RemoteCommandEvent::process on the server: serverCmd<name>(%client, ...).
        std::vector<std::string> args = RemoteCommand::scriptArguments(event.rawArguments, event.taggedArguments);
        std::vector<VMValue> callArgs{VMValue(std::to_string(ScriptEngine::instance().objectId(script)))};
        for (size_t i = 1; i < args.size(); ++i) callArgs.emplace_back(args[i]);
        const std::string function = "serverCmd" + args[0];
        if (ts->isFunction(function)) ts->callFunction(function, callArgs);
    }
}

// GameConnection::readPacket, client half: the move acknowledgement; the
// packet then goes to the client's reader whole.
void GameConnection::clientReadPacket(V12BitStream& stream, const uint8_t* data, size_t size) {
    readRates(stream);
    const uint32_t ack = stream.readUnsigned(32);
    if (stream.failed()) return;
    while (lastMoveAck < ack && !moves.empty()) {
        moves.pop_front();
        ++lastMoveAck;
    }
    if (lastMoveAck < ack) lastMoveAck = ack;
    if (onServerPacket) onServerPacket(std::vector<uint8_t>(data, data + size));
}

void GameConnection::clientGhostMessage(int message, uint32_t sequence, uint32_t count) {
    auto* ts = ScriptEngine::instance().ts();
    switch (message) {
        case GhostAlwaysDone: {
            auto event = std::make_shared<NetEventOut>();
            event->classIndex = GhostingMessage;
            event->pack = [sequence](TorqueBitWriter& w) {
                w.writeU32(sequence);
                w.writeInt(ReadyForNormalGhosts, 3);
                w.writeInt(0, 11);
            };
            postEvent(event);
            break;
        }
        case GhostAlwaysStarting:
            if (ts) ts->callFunction("ghostAlwaysStarted", {VMValue(std::to_string(count))});
            break;
        default: break;
    }
}

// --- Ghosting (netGhost.cc) -------------------------------------------------

static NetObject* netObjectFor(const std::string& key) {
    return key.empty() ? nullptr : EngineObjects::get<NetObject>(key);
}

static void postGhostingMessage(GameConnection& connection, int message, uint32_t sequence, uint32_t count) {
    auto event = std::make_shared<NetEventOut>();
    event->classIndex = GameConnection::GhostingMessage;
    event->pack = [message, sequence, count](TorqueBitWriter& w) {
        w.writeU32(sequence);
        w.writeInt(message, 3);
        w.writeInt((int32_t)count, 11);
    };
    connection.postEvent(event);
}

void GameConnection::activateGhosting() {
    ++ghostingSequence;
    std::vector<std::string> always;
    for (auto& [name, object] : ScriptEngine::instance().objects) {
        auto* net = object ? dynamic_cast<NetObject*>(object->engine.get()) : nullptr;
        if (net && net->ghostable && net->scopeAlways && net->netClassId() >= 0)
            always.push_back(std::to_string(ScriptEngine::instance().objectId(object)));
    }
    std::sort(always.begin(), always.end(), [](const std::string& a, const std::string& b) {
        return std::atoi(a.c_str()) < std::atoi(b.c_str());
    });
    ghosts.assign(1024, {});
    scoping = true;
    // The ghost-always objects take the top indices, in set order.
    const size_t count = std::min<size_t>(always.size(), 1024);
    postGhostingMessage(*this, GhostAlwaysStarting, ghostingSequence, (uint32_t)count);
    for (size_t j = 0; j < count; ++j) {
        const int index = (int)(1024 - count + j);
        GhostInfo& ghost = ghosts[index];
        ghost.object = always[j];
        ghost.index = index;
        ghost.flags = GhostInfo::InScope | GhostInfo::ScopeAlways | GhostInfo::ScopedEvent;
        ghost.updateMask = 0;
    }
    for (size_t j = count; j-- > 0;) {
        const int index = (int)(1024 - count + j);
        const std::string object = always[j];
        auto event = std::make_shared<NetEventOut>();
        event->classIndex = 3; // GhostAlwaysObjectEvent
        event->pack = [this, object, index](TorqueBitWriter& w) {
            w.writeInt(index, 10);
            NetObject* net = netObjectFor(object);
            if (w.writeFlag(net != nullptr)) {
                w.writeInt(net->netClassId(), 7);
                net->packUpdate(*this, 0xFFFFFFFFu, w);
            }
        };
        postEvent(event);
    }
    postGhostingMessage(*this, GhostAlwaysDone, ghostingSequence, 0);
}

void GameConnection::resetGhosting() {
    ghosting = false;
    scoping = false;
    postGhostingMessage(*this, EndGhosting, ghostingSequence, 0);
    ++ghostingSequence;
    ghosts.assign(1024, {});
    ghostsInFlight.clear();
}

void GameConnection::handleGhostMessage(int message, uint32_t sequence) {
    if (message == ReadyForNormalGhosts && sequence == ghostingSequence) {
        ghosting = true;
        for (auto& ghost : ghosts)
            if (ghost.flags & GhostInfo::ScopedEvent) ghost.flags &= ~(GhostInfo::Ghosting | GhostInfo::ScopedEvent);
    }
}

// NetConnection::getGhostIndex: only a ghost the client has (its creation
// acknowledged, not being killed).
int GameConnection::ghostIndex(const std::string& object) const {
    constexpr uint32_t pending = GhostInfo::NotYetGhosted | GhostInfo::Ghosting |
                                 GhostInfo::KillGhost | GhostInfo::KillingGhost;
    for (const auto& ghost : ghosts)
        if (ghost.index >= 0 && ghost.object == object && !(ghost.flags & pending)) return ghost.index;
    return -1;
}

void GameConnection::objectInScope(const std::string& object) {
    if (!scoping) return;
    for (auto& ghost : ghosts)
        if (ghost.index >= 0 && ghost.object == object) { ghost.flags |= GhostInfo::InScope; return; }
    NetObject* net = netObjectFor(object);
    if (!net || !net->ghostable || net->netClassId() < 0) return;
    for (int index = 0; index < 1024; ++index) {
        GhostInfo& ghost = ghosts[index];
        if (ghost.index >= 0) continue;
        ghost.object = object;
        ghost.index = index;
        ghost.updateMask = 0xFFFFFFFFu;
        ghost.flags = GhostInfo::NotYetGhosted | GhostInfo::InScope |
                      (net && net->scopeAlways ? GhostInfo::ScopeAlways : 0);
        ghost.updateSkipCount = 0;
        return;
    }
}

void GameConnection::objectLocalScopeAlways(const std::string& object) {
    objectInScope(object);
    for (auto& ghost : ghosts)
        if (ghost.index >= 0 && ghost.object == object) { ghost.flags |= GhostInfo::ScopeLocalAlways; return; }
}

// NetConnection::ghostWritePacket scoping: every ghost not scoped always
// leaves scope, the scope object scopes the scene (onCameraScopeQuery:
// itself, then SceneGraph::scopeScene within the visible distance), and a
// ghost left out of scope is killed.
void GameConnection::scopeScene() {
    for (auto& ghost : ghosts)
        if (ghost.index >= 0 && !(ghost.flags & (GhostInfo::ScopeAlways | GhostInfo::ScopeLocalAlways)))
            ghost.flags &= ~GhostInfo::InScope;
    auto* scope = controlObject_.empty() ? nullptr : EngineObjects::get<SceneObject>(controlObject_);
    if (scope) {
        objectInScope(controlObject_);
        float visible = 0.0f;
        auto& engine = ScriptEngine::instance();
        for (auto& [name, object] : engine.objects)
            if (object && object->className == "Sky") {
                visible = Fields::f32(object, "visibleDistance", 0.0f);
                break;
            }
        const float x = scope->transform[3], y = scope->transform[7], z = scope->transform[11];
        std::vector<std::string> inRange;
        for (auto& [name, object] : engine.objects) {
            auto* net = object ? dynamic_cast<SceneObject*>(object->engine.get()) : nullptr;
            if (!net || !net->ghostable || net->scopeAlways || net->netClassId() < 0) continue;
            // A hidden shape is out of the scene.
            if (auto* shape = dynamic_cast<ShapeBase*>(net); shape && shape->hidden) continue;
            const float dx = net->transform[3] - x, dy = net->transform[7] - y, dz = net->transform[11] - z;
            if (dx * dx + dy * dy + dz * dz <= visible * visible) inRange.push_back(engine.objectKey(object));
        }
        for (const auto& key : inRange) objectInScope(key);
    }
    // GameConnection::doneScopingScene: in the commander map, everything
    // of the map's types (commanderScopeCallback); otherwise the
    // sensor-visible set.
    if (inCommanderMap) {
        constexpr uint32_t scopeMask = SimContainer::TerrainObjectType | SimContainer::InteriorObjectType |
                                       SimContainer::WaterObjectType | SimContainer::PlayerObjectType |
                                       SimContainer::VehicleObjectType | SimContainer::StaticShapeObjectType;
        std::vector<std::string> all;
        auto& engine = ScriptEngine::instance();
        for (auto& [name, object] : engine.objects) {
            auto* net = object ? dynamic_cast<NetObject*>(object->engine.get()) : nullptr;
            if (!net || !net->ghostable || net->netClassId() < 0) continue;
            if (SimContainer::typeMask(object) & scopeMask) all.push_back(engine.objectKey(object));
        }
        for (const auto& key : all) objectInScope(key);
    } else {
        const uint32_t group = ServerTargets::connectionSensorGroup(*this);
        std::vector<std::string> visible;
        auto& engine = ScriptEngine::instance();
        for (auto& [name, object] : engine.objects) {
            auto* shape = object ? dynamic_cast<ShapeBase*>(object->engine.get()) : nullptr;
            if (!shape || !shape->scopeWhenSensorVisible || shape->hidden || shape->targetId == -1 ||
                !shape->ghostable || shape->netClassId() < 0)
                continue;
            if (ServerTargets::isTargetVisible(shape->targetId, group)) visible.push_back(engine.objectKey(object));
        }
        for (const auto& key : visible) objectInScope(key);
    }
    for (auto& ghost : ghosts)
        if (ghost.index >= 0 && !(ghost.flags & (GhostInfo::InScope | GhostInfo::ScopeAlways |
                                                 GhostInfo::ScopeLocalAlways | GhostInfo::KillGhost |
                                                 GhostInfo::KillingGhost))) {
            ghost.flags |= GhostInfo::KillGhost;
            ghost.updateMask = 0xFFFFFFFFu;
        }
}

void GameConnection::writeGhosts(TorqueBitWriter& w, std::vector<GhostRef>& refs) {
    if (!w.writeFlag(ghosting)) return;
    if (scoping) scopeScene();
    // Dirty state set on the objects since the last packet.
    int maxIndex = 0;
    std::vector<GhostInfo*> updates;
    for (auto& ghost : ghosts) {
        if (ghost.index < 0) continue;
        if (NetObject* net = netObjectFor(ghost.object)) {
            if (net->pendingMask && !(ghost.flags & GhostInfo::NotYetGhosted)) ghost.updateMask |= net->pendingMask;
        } else if (!(ghost.flags & (GhostInfo::KillGhost | GhostInfo::KillingGhost))) {
            ghost.flags |= GhostInfo::KillGhost; // the object is gone
            ghost.updateMask = 0xFFFFFFFFu;
        }
        if (!ghost.updateMask || (ghost.flags & (GhostInfo::KillingGhost | GhostInfo::Ghosting))) continue;
        maxIndex = std::max(maxIndex, ghost.index);
        updates.push_back(&ghost);
    }
    int sendSize = 1;
    for (int m = maxIndex; m >>= 1;) ++sendSize;
    if (sendSize < 3) sendSize = 3;
    w.writeInt(sendSize - 3, 3);
    for (GhostInfo* ghost : updates) {
        if (w.bitPosition() > packetBudgetBits()) break;
        w.writeFlag(true);
        w.writeInt(ghost->index, sendSize);
        const uint32_t updateMask = ghost->updateMask;
        GhostRef ref{ghost->index, updateMask, 0};
        if (ghost->flags & GhostInfo::KillGhost) {
            ghost->flags = (ghost->flags & ~GhostInfo::KillGhost) | GhostInfo::KillingGhost;
            ghost->updateMask = 0;
            ref.flags = GhostInfo::KillingGhost;
            w.writeFlag(true);
        } else {
            w.writeFlag(false);
            NetObject* net = netObjectFor(ghost->object);
            if (ghost->flags & GhostInfo::NotYetGhosted) {
                ghost->flags = (ghost->flags & ~GhostInfo::NotYetGhosted) | GhostInfo::Ghosting;
                ref.flags = GhostInfo::Ghosting;
                w.writeInt(net->netClassId(), 7);
            }
            const uint32_t retMask = net->packUpdate(*this, updateMask, w);
            ghost->updateMask = retMask;
            ref.mask = updateMask & ~retMask;
        }
        ghost->updateSkipCount = 0;
        refs.push_back(ref);
    }
    w.writeFlag(false);
}

void GameConnection::ghostPacketDropped(std::vector<GhostRef>& refs) {
    for (const auto& ref : refs) {
        GhostInfo& ghost = ghosts[ref.index];
        if (ghost.index < 0) continue;
        ghost.updateMask |= ref.mask;
        if (ref.flags & GhostInfo::Ghosting)
            ghost.flags = (ghost.flags | GhostInfo::NotYetGhosted) & ~GhostInfo::Ghosting;
        else if (ref.flags & GhostInfo::KillingGhost)
            ghost.flags = (ghost.flags | GhostInfo::KillGhost) & ~GhostInfo::KillingGhost;
    }
}

void GameConnection::ghostPacketReceived(std::vector<GhostRef>& refs) {
    for (const auto& ref : refs) {
        GhostInfo& ghost = ghosts[ref.index];
        if (ghost.index < 0) continue;
        if (ref.flags & GhostInfo::Ghosting) ghost.flags &= ~GhostInfo::Ghosting;
        else if (ref.flags & GhostInfo::KillingGhost) ghost = GhostInfo{};
    }
}

std::function<void(GameConnection&)> gLocalClientStarted;

void clientNetProcess(double now) {
    if (!ScriptEngine::exists()) return;
    if (auto* connection = EngineObjects::get<GameConnection>("ServerConnection"))
        if (!connection->isServer) connection->checkPacketSend(now);
}

void serverNetProcess(double now) {
    if (!ScriptEngine::exists()) return;
    ScriptObject* group = ScriptEngine::instance().findObject("ClientGroup");
    if (!group) return;
    const int count = group->internals["__childCount"].toInt();
    for (int i = 0; i < count; ++i) {
        const std::string key = group->internals["__child" + std::to_string(i)].toString();
        if (auto* connection = EngineObjects::get<GameConnection>(key)) connection->checkPacketSend(now);
    }
    // NetObject::collapseDirtyList: every connection has taken the dirty bits.
    for (auto& [name, object] : ScriptEngine::instance().objects)
        if (auto* net = object ? dynamic_cast<NetObject*>(object->engine.get()) : nullptr) net->pendingMask = 0;
}

void registerGameConnectionNatives(TorqueScript& ts) {
    EngineObjects::registerClass("GameConnection", [] { return std::make_shared<GameConnection>(); });
    using Args = std::vector<VMValue>;
    // game/net.cc commandToClient(client, 'cmd', args...).
    ts.registerNative("commandToClient", [](const Args& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        auto* connection = EngineObjects::get<GameConnection>(args[0].toString());
        if (!connection) return VMValue("");
        std::vector<std::string> argv;
        for (size_t i = 1; i < args.size(); ++i) argv.push_back(args[i].toString());
        connection->sendRemoteCommand(argv);
        return VMValue("");
    });
    // netDispatch.cc cLocalConnect: the client's ServerConnection and the
    // server's LocalClientConnection, linked in process; GameClientAdded
    // puts the latter in ClientGroup and calls its onConnect(args...).
    ts.registerNative("localConnect", [](const Args& args) -> VMValue {
        auto& engine = ScriptEngine::instance();
        engine.ensureEngineGroups();
        ScriptObject* clientObject = engine.createEngineObject("GameConnection", "ServerConnection");
        ScriptObject* serverObject = engine.createEngineObject("GameConnection", "LocalClientConnection");
        auto* client = EngineObjects::get<GameConnection>(engine.objectKey(clientObject));
        auto* server = EngineObjects::get<GameConnection>(engine.objectKey(serverObject));
        if (!client || !server) return VMValue("");
        client->isServer = false;
        client->releaseVoiceId();
        client->local = server->local = true;
        server->deliver = [client](const std::vector<uint8_t>& p) { client->receivePacket(p.data(), p.size()); };
        client->deliver = [server](const std::vector<uint8_t>& p) { server->receivePacket(p.data(), p.size()); };
        if (gLocalClientStarted) gLocalClientStarted(*client);
        if (ScriptObject* group = engine.findObject("ClientGroup")) engine.addToSet(group, serverObject);
        auto* script = engine.ts();
        if (!script) return VMValue("");
        std::vector<VMValue> connectArgs(args.begin(), args.end());
        script->callObjectMethod(engine.objectKey(serverObject), "onConnect", connectArgs);
        script->callFunction("LocalConnectionAccepted", {});
        return VMValue("");
    });
    // SceneLighting::lightScene: the client's world is lit as it loads,
    // so lighting completes at once (completed(true) runs the callback)
    // and nothing is left to light.
    ts.registerNative("lightScene", [](const Args& args) -> VMValue {
        auto* connection = EngineObjects::get<GameConnection>("ServerConnection");
        if (!connection || connection->isServer) {
            Console::instance().printf(LogLevel::Error, "SceneLighting:: no game connection");
            return VMValue(0);
        }
        const std::string callback = args.empty() ? std::string() : args[0].toString();
        if (auto* script = ScriptEngine::instance().ts(); script && !callback.empty())
            script->callFunction(callback, {});
        return VMValue(0);
    });
    ts.registerNative("GameConnection::setMissionCRC", [](const Args& args) -> VMValue {
        if (auto* connection = args.empty() ? nullptr : EngineObjects::get<GameConnection>(args[0].toString()))
            if (args.size() > 1) connection->setMissionCRC((uint32_t)args[1].toDouble());
        return VMValue("");
    });
    ts.registerNative("GameConnection::transmitDataBlocks", [](const Args& args) -> VMValue {
        if (auto* c = args.empty() ? nullptr : EngineObjects::get<GameConnection>(args[0].toString()))
            if (args.size() > 1) c->transmitDataBlocks((uint32_t)args[1].toDouble());
        return VMValue("");
    });
    ts.registerNative("GameConnection::setControlObject", [](const Args& args) -> VMValue {
        auto* c = args.empty() ? nullptr : EngineObjects::get<GameConnection>(args[0].toString());
        if (!c || args.size() < 2) return VMValue(0);
        ScriptObject* object = ScriptEngine::instance().findObject(args[1].toString().c_str());
        if (!object || !EngineObjects::get<ShapeBase>(args[1].toString())) return VMValue(0);
        c->setControlObject(ScriptEngine::instance().objectKey(object));
        return VMValue(1);
    });
    ts.registerNative("GameConnection::getControlObject", [](const Args& args) -> VMValue {
        auto* c = args.empty() ? nullptr : EngineObjects::get<GameConnection>(args[0].toString());
        ScriptObject* object = c && !c->controlObject().empty()
            ? ScriptEngine::instance().findObject(c->controlObject().c_str()) : nullptr;
        return VMValue(object ? ScriptEngine::instance().objectId(object) : 0);
    });
    ts.registerNative("NetObject::scopeToClient", [](const Args& args) -> VMValue {
        if (args.size() < 2) return VMValue("");
        auto* c = EngineObjects::get<GameConnection>(args[1].toString());
        ScriptObject* object = ScriptEngine::instance().findObject(args[0].toString().c_str());
        if (!c) {
            Console::instance().printf(LogLevel::Error,
                "NetObject::scopeToClient: Couldn't find connection %s", args[1].toString().c_str());
            return VMValue("");
        }
        if (object) c->objectLocalScopeAlways(ScriptEngine::instance().objectKey(object));
        return VMValue("");
    });
    ts.registerNative("NetConnection::getAddress", [](const Args& args) -> VMValue {
        auto* connection = args.empty() ? nullptr : EngineObjects::get<GameConnection>(args[0].toString());
        return VMValue(connection ? connection->address : std::string());
    });
    ts.registerNative("NetConnection::checkMaxRate", [](const Args& args) -> VMValue {
        if (auto* c = args.empty() ? nullptr : EngineObjects::get<GameConnection>(args[0].toString())) c->checkMaxRate();
        return VMValue("");
    });
    // GameConnection::play2D / play3D: a Sim2DAudioEvent / Sim3DAudioEvent,
    // a 3D one only within the description's maxDistance of the control
    // object (GameConnection::play3D).
    ts.registerNative("GameConnection::play2D", [](const Args& args) -> VMValue {
        auto* c = args.empty() ? nullptr : EngineObjects::get<GameConnection>(args[0].toString());
        ScriptObject* profile = args.size() > 1 ? ScriptEngine::instance().findObject(args[1].toString().c_str()) : nullptr;
        if (!c || !profile) return VMValue(0);
        c->play2D(ScriptEngine::instance().objectId(profile));
        return VMValue(1);
    });
    ts.registerNative("GameConnection::play3D", [](const Args& args) -> VMValue {
        auto* c = args.empty() ? nullptr : EngineObjects::get<GameConnection>(args[0].toString());
        ScriptObject* profile = args.size() > 1 ? ScriptEngine::instance().findObject(args[1].toString().c_str()) : nullptr;
        if (!c || !profile || args.size() < 3) return VMValue(0);
        float pos[3] = {0, 0, 0};
        TorqueMath::AngAxis aa{0, 0, 1, 0};
        std::sscanf(args[2].toString().c_str(), "%f %f %f %f %f %f %f", &pos[0], &pos[1], &pos[2], &aa.x, &aa.y,
                    &aa.z, &aa.angle);
        c->play3D(profile, TorqueMath::matrix(pos, aa));
        return VMValue(1);
    });
    auto conn = [](const Args& args, size_t i) -> GameConnection* {
        return i < args.size() ? EngineObjects::get<GameConnection>(args[i].toString()) : nullptr;
    };
    // GameConnection::setObjectActiveImage: the client's ghost of `obj` makes
    // the image in `slot` its active one (SetObjectActiveImageEvent).
    ts.registerNative("GameConnection::setObjectActiveImage", [conn](const Args& args) -> VMValue {
        auto* c = conn(args, 0);
        if (!c || !c->isServer) return VMValue("");
        const std::string objName = args.size() > 1 ? args[1].toString() : std::string();
        ScriptObject* object = ScriptEngine::instance().findObject(objName.c_str());
        if (!object || !dynamic_cast<ShapeBase*>(object->engine.get())) {
            Console::instance().printf(LogLevel::Error, "GameConnection::cSetObjectActiveImage: invalid object %s",
                                       objName.c_str());
            return VMValue("");
        }
        const uint32_t slot = args.size() > 2 ? (uint32_t)args[2].toInt() : 0;
        if (slot >= ShapeBase::MaxMountedImages) {
            Console::instance().printf(LogLevel::Error,
                                       "GameConnection::cSetControlObjectActiveImage: image slot out of range [%u]", slot);
            return VMValue("");
        }
        const int ghost = c->ghostIndex(ScriptEngine::instance().objectKey(object));
        if (ghost == -1) return VMValue("");
        auto event = std::make_shared<NetEventOut>();
        event->classIndex = GameConnection::SetObjectActiveImage;
        event->pack = [ghost, slot](TorqueBitWriter& w) {
            w.writeRangedU32((uint32_t)ghost, 0, 1023);
            w.writeRangedU32(slot, 0, ShapeBase::MaxMountedImages);
        };
        c->postEvent(event);
        return VMValue("");
    });
    ts.registerNative("GameConnection::scopeCommanderMap", [conn](const Args& args) -> VMValue {
        if (auto* c = conn(args, 0)) c->inCommanderMap = args.size() > 1 && args[1].toBool();
        return VMValue("");
    });
    ts.registerNative("GameConnection::isScopingCommanderMap", [conn](const Args& args) -> VMValue {
        auto* c = conn(args, 0);
        return VMValue(c && c->inCommanderMap ? 1 : 0);
    });
    ts.registerNative("NetConnection::getPing", [conn](const Args& args) -> VMValue {
        auto* c = conn(args, 0);
        return VMValue((int32_t)(c ? c->roundTripTime : 0.0f));
    });
    ts.registerNative("NetConnection::getPacketLoss", [conn](const Args& args) -> VMValue {
        auto* c = conn(args, 0);
        return VMValue((int32_t)(c ? 100.0f * c->packetLoss : 0.0f));
    });
    // The shipped GameConnection::getAuthInfo: the connection's WON account
    // fields, "" when it has none (a -nologin LAN client).
    ts.registerNative("GameConnection::getAuthInfo", [](const Args&) -> VMValue { return VMValue(""); });
    ts.registerNative("GameConnection::setVehicleTeleportEnabled", [conn](const Args& args) -> VMValue {
        if (auto* c = conn(args, 0)) c->vehicleTeleportEnabled = args.size() > 1 && args[1].toBool();
        return VMValue("");
    });
    ts.registerNative("GameConnection::isVehicleTeleportEnabled", [conn](const Args& args) -> VMValue {
        auto* c = conn(args, 0);
        return VMValue(c && c->vehicleTeleportEnabled ? 1 : 0);
    });
    ts.registerNative("GameConnection::listenEnabled", [conn](const Args& args) -> VMValue {
        auto* c = conn(args, 0);
        return VMValue(c && c->maxVoiceChannels > 0 ? 1 : 0);
    });
    ts.registerNative("GameConnection::getListenState", [conn](const Args& args) -> VMValue {
        auto* me = conn(args, 0);
        auto* him = conn(args, 1);
        return VMValue(me && him && me->wouldListenTo[him->voiceId] ? 1 : 0);
    });
    ts.registerNative("GameConnection::canListenTo", [conn](const Args& args) -> VMValue {
        auto* me = conn(args, 0);
        auto* him = conn(args, 1);
        return VMValue(me && him && me->canListen(*him) ? 1 : 0);
    });
    ts.registerNative("GameConnection::listenTo", [conn](const Args& args) -> VMValue {
        auto* me = conn(args, 0);
        auto* him = conn(args, 1);
        if (me && him) me->listenTo(him->voiceId, args.size() > 2 && args[2].toBool());
        return VMValue("");
    });
    ts.registerNative("GameConnection::listenToAll", [conn](const Args& args) -> VMValue {
        if (auto* c = conn(args, 0)) c->wouldListenTo.fill(true);
        return VMValue("");
    });
    ts.registerNative("GameConnection::listenToNone", [conn](const Args& args) -> VMValue {
        if (auto* c = conn(args, 0)) c->wouldListenTo.fill(false);
        return VMValue("");
    });
    ts.registerNative("GameConnection::setVoiceChannels", [conn](const Args& args) -> VMValue {
        if (auto* c = conn(args, 0))
            c->maxVoiceChannels = std::clamp(args.size() > 1 ? args[1].toInt() : 0, 0, (int)GameConnection::MaxVoiceChannels);
        return VMValue("");
    });
    ts.registerNative("GameConnection::setVoiceDecodingMask", [conn](const Args& args) -> VMValue {
        if (auto* c = conn(args, 0)) c->voiceDecodingMask = args.size() > 1 ? (uint32_t)args[1].toInt() : 0;
        return VMValue("");
    });
    ts.registerNative("GameConnection::setVoiceEncodingLevel", [conn](const Args& args) -> VMValue {
        if (auto* c = conn(args, 0)) c->voiceEncodingLevel = args.size() > 1 ? args[1].toInt() : -1;
        return VMValue("");
    });
    // GameConnection::isAIControlled (mAIControlled: an AIConnection).
    ts.registerNative("GameConnection::isAIControlled", [](const Args& args) -> VMValue {
        ScriptObject* object = args.empty() ? nullptr : ScriptEngine::instance().findObject(args[0].toString().c_str());
        return VMValue(object && EngineClasses::isA(object->className, "AIConnection") ? 1 : 0);
    });
    ts.registerNative("GameConnection::activateGhosting", [](const Args& args) -> VMValue {
        if (auto* c = args.empty() ? nullptr : EngineObjects::get<GameConnection>(args[0].toString())) c->activateGhosting();
        return VMValue("");
    });
    ts.registerNative("GameConnection::resetGhosting", [](const Args& args) -> VMValue {
        if (auto* c = args.empty() ? nullptr : EngineObjects::get<GameConnection>(args[0].toString())) c->resetGhosting();
        return VMValue("");
    });
}

namespace {
constexpr int DataBlockObjectIdBitSize = 11;
}

void GameConnection::play2D(int profileId) {
    auto event = std::make_shared<NetEventOut>();
    event->classIndex = Sim2DAudio;
    event->pack = [profileId](TorqueBitWriter& w) { w.writeInt(profileId - (int)DataBlockPack::ObjectIdFirst, DataBlockObjectIdBitSize); };
    postEvent(event);
}

void GameConnection::play3D(ScriptObject* profile, const std::array<float, 16>& transform) {
    auto& engine = ScriptEngine::instance();
    const int profileId = engine.objectId(profile);
    ScriptObject* description = engine.findObject(Fields::string(profile, "description").c_str());
    const Point3F pos{transform[3], transform[7], transform[11]};
    if (auto* control = EngineObjects::get<SceneObject>(controlObject())) {
        // Only post the event if it's within audible range of the control object.
        const float dx = control->transform[3] - pos.x, dy = control->transform[7] - pos.y,
                    dz = control->transform[11] - pos.z;
        float maxDistance = description ? Fields::f32(description, "maxDistance", 100.0f) : 100.0f;
        if (description && Fields::boolean(description, "is3D", false)) {
            const float minDistance = std::max(0.0f, Fields::f32(description, "minDistance", 1.0f));
            if (maxDistance <= minDistance) maxDistance = minDistance + 0.01f;
        }
        if (std::sqrt(dx * dx + dy * dy + dz * dz) >= maxDistance) return;
    }
    const bool cone = description && (Fields::s32(description, "coneInsideAngle", 360) ||
                                      Fields::s32(description, "coneOutsideAngle", 360));
    const TorqueMath::Quat q = TorqueMath::normalize(TorqueMath::quat(transform));
    auto event = std::make_shared<NetEventOut>();
    event->classIndex = Sim3DAudio;
    event->pack = [profileId, cone, q, pos](TorqueBitWriter& w) {
        w.writeInt(profileId - (int)DataBlockPack::ObjectIdFirst, DataBlockObjectIdBitSize);
        if (w.writeFlag(cone)) {
            constexpr int SoundRotBits = 8;
            w.writeFloat(q.x, SoundRotBits);
            w.writeFloat(q.y, SoundRotBits);
            w.writeFloat(q.z, SoundRotBits);
            w.writeFlag(q.w < 0.0f);
        }
        constexpr float SoundPosAccuracy = 0.5f;
        w.writeCompressedPoint({pos.x, pos.y, pos.z}, SoundPosAccuracy);
    };
    postEvent(event);
}

namespace {
// GameConnection::smVoiceConnections: voice id -> connection.
GameConnection* gVoiceConnections[GameConnection::MaxClients + 1] = {};
}

// GameConnection::onAdd / onRemove: a client's connection takes the first
// free voice id and would listen to everyone.
GameConnection::GameConnection() {
    checkMaxRate();
    wouldListenTo.fill(true);
    for (int i = 1; i <= MaxClients; ++i)
        if (!gVoiceConnections[i]) {
            gVoiceConnections[i] = this;
            voiceId = i;
            break;
        }
}

GameConnection::~GameConnection() { releaseVoiceId(); }

void GameConnection::releaseVoiceId() {
    if (voiceId > 0 && gVoiceConnections[voiceId] == this) gVoiceConnections[voiceId] = nullptr;
    voiceId = 0;
}

bool GameConnection::canListen(const GameConnection& other) const {
    // never allow a connection to listen to self
    if (&other == this) return false;
    // Can't listen if no channels are available:
    if (maxVoiceChannels == 0) return false;
    // make sure encoder/decoder's match
    if (other.voiceEncodingLevel < 0) return false;
    if (!(voiceDecodingMask & (1u << other.voiceEncodingLevel))) return false;
    // check the listen mask for this group
    const uint32_t listenMask = ServerTargets::sensorGroupListenMask(ServerTargets::connectionSensorGroup(*this));
    return (listenMask & (1u << ServerTargets::connectionSensorGroup(other))) != 0;
}

void GameConnection::listenTo(int voice, bool listen) {
    if (voice < 0 || voice > MaxClients) return;
    if (listen) {
        wouldListenTo[voice] = true;
        return;
    }
    // terminate any existing stream
    for (GameConnection* connection : gVoiceConnections)
        if (connection && connection->isServer) connection->stopListening(voiceId);
    // refuse any future request to talk to me
    wouldListenTo[voice] = false;
}

void GameConnection::stopListening(int voice) {
    if (voice < 0 || voice > MaxClients) return;
    if (listeningTo[voice]) {
        curVoiceChannels--;
    } else if (wouldListenTo[voice] && gVoiceConnections[voice] && script) {
        // notify client that someone quit talking
        auto& engine = ScriptEngine::instance();
        if (auto* ts = engine.ts())
            ts->execute("commandToClient(" + engine.objectKey(script) + ", 'playerStoppedTalking', " +
                        (gVoiceConnections[voice]->script ? engine.objectKey(gVoiceConnections[voice]->script) : "0") +
                        ", 0);");
    }
    listeningTo[voice] = false;
}
