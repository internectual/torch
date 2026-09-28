#include "sim/game_connection.h"
#include "sim/net_string_table.h"
#include "net/remote_command.h"
#include "net/v12_bitstream.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "core/console.h"
#include <algorithm>

namespace {
constexpr int NetEventClassBits = 6;
constexpr size_t PacketBudgetBits = 1400 * 8; // MaxPacketDataSize less headroom
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
    // checkPacketSend: current and maximum rate unchanged.
    w.writeFlag(false);
    w.writeFlag(false);
    w.setStringBuffer(true);
    w.clearCompression();
    // GameConnection::writePacket, server half.
    w.writeU32(lastMoveAck);
    w.writeFlag(false); // damage flash / white out
    w.writeFlag(false); // lock / homing counts
    w.writeFlag(false); // seeker tracking
    w.writeFlag(false); // pinged
    w.writeFlag(false); // jammed
    w.writeFlag(false); // no control object
    w.writeFlag(false); // visible target masks unchanged
    w.writeFlag(false); // camera fov
    // eventWritePacket
    while (!unorderedQueue.empty() && w.bitPosition() < PacketBudgetBits) {
        auto event = unorderedQueue.front();
        unorderedQueue.pop_front();
        w.writeFlag(true);
        w.writeInt(event->classIndex, NetEventClassBits);
        if (event->pack) event->pack(w);
        sent.push_back(event);
    }
    w.writeFlag(false);
    int prevSeq = -2;
    while (!orderedQueue.empty() && w.bitPosition() < PacketBudgetBits) {
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
    // ghostWritePacket: not ghosting yet.
    w.writeFlag(false);
}

void GameConnection::checkPacketSend(double now) {
    if (!deliver) return;
    if (lastUpdate >= 0.0 && now < lastUpdate + updateDelaySeconds) return;
    lastUpdate = now;
    TorqueBitWriter w;
    protocol.writePacketHeader(w.raw(), V12::PacketType::Data);
    std::vector<std::shared_ptr<NetEventOut>> sent;
    writePacket(w, sent);
    inFlight[protocol.lastSent()] = std::move(sent);
    deliver(w.data());
}

void GameConnection::receivePacket(const uint8_t* data, size_t size) {
    V12BitStream stream(data, size);
    V12::DnetHeader header;
    if (!V12::readDnetHeader(stream, header)) return;
    const auto result = protocol.processReceived(header);
    for (const auto& ack : result.acknowledgements) {
        auto it = inFlight.find(ack.sequence);
        if (it == inFlight.end()) continue;
        if (ack.acknowledged) packetReceived(it->second);
        else packetDropped(it->second);
        inFlight.erase(it);
    }
    if (!result.accepted) return;
    if (header.packetType == V12::PacketType::Ping) {
        if (deliver) deliver(protocol.buildPacket(V12::PacketType::Ack));
        return;
    }
    if (!result.dispatchData || header.packetType != V12::PacketType::Data) return;
    // Rate fields.
    if (stream.readFlag()) { stream.readUnsigned(10); stream.readUnsigned(10); }
    if (stream.readFlag()) { stream.readUnsigned(10); stream.readUnsigned(10); }
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
        if (event.classId != RemoteCommand || event.rawArguments.empty()) continue;
        // RemoteCommandEvent::process on the server: serverCmd<name>(%client, ...).
        std::vector<std::string> args = RemoteCommand::scriptArguments(event.rawArguments, event.taggedArguments);
        std::vector<VMValue> callArgs{VMValue(std::to_string(ScriptEngine::instance().objectId(script)))};
        for (size_t i = 1; i < args.size(); ++i) callArgs.emplace_back(args[i]);
        const std::string function = "serverCmd" + args[0];
        if (ts->isFunction(function)) ts->callFunction(function, callArgs);
    }
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
    ts.registerNative("GameConnection::setMissionCRC", [](const Args& args) -> VMValue {
        if (auto* connection = args.empty() ? nullptr : EngineObjects::get<GameConnection>(args[0].toString()))
            if (args.size() > 1) connection->setMissionCRC((uint32_t)args[1].toDouble());
        return VMValue("");
    });
    ts.registerNative("NetConnection::getAddress", [](const Args& args) -> VMValue {
        auto* connection = args.empty() ? nullptr : EngineObjects::get<GameConnection>(args[0].toString());
        return VMValue(connection ? connection->address : std::string());
    });
    ts.registerNative("GameConnection::isAIControlled", [](const Args&) -> VMValue { return VMValue(0); });
}
