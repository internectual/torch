#include "net/v12_events.h"
#include "net/v12_datablocks.h"
#include "net/v12_registry.h"

#include <utility>
#include <sstream>
#include <cstdlib>

namespace V12 {

std::pair<uint16_t, bool> NetStringTable::getOrAdd(const std::string& value) {
    for (const auto& [id, existing] : values) {
        if (existing == value) return {id, false};
    }
    uint16_t id = 1;
    while (values.find(id) != values.end() && id < 1024) ++id;
    if (id >= 1024) return {0, false};
    values.emplace(id, value);
    return {id, true};
}

bool readEventHeader(V12BitStream& stream, bool guaranteedPhase, EventHeader& header) {
    header.guaranteed = guaranteedPhase;
    if (guaranteedPhase) {
        // A set bit means the next sequence is sequential; otherwise the
        // packet carries an explicit 7-bit sequence number.
        header.sequential = stream.readFlag();
        if (!header.sequential) header.sequence = (uint8_t)stream.readUnsigned(EventSequenceBits);
    }
    header.classId = (uint8_t)stream.readUnsigned(EventClassBits);
    return !stream.failed();
}

void writeEventHeader(V12BitWriter& writer, bool guaranteedPhase,
                      const EventHeader& header, bool sequential) {
    if (guaranteedPhase) {
        writer.writeFlag(sequential);
        if (!sequential) writer.writeUnsigned(header.sequence, EventSequenceBits);
    }
    writer.writeUnsigned(header.classId, EventClassBits);
}

bool readServerEvents(V12BitStream& stream, NetStringTable& strings,
                      std::vector<ServerEvent>& events) {
    bool guaranteedPhase = false;
    int previousSequence = -1;
    bool more = stream.readFlag();
    while (!stream.failed()) {
        if (!more) {
            if (guaranteedPhase) break;
            guaranteedPhase = true;
            more = stream.readFlag();
            if (!more) break;
        }
        EventHeader header;
        if (!readEventHeader(stream, guaranteedPhase, header)) return false;
        if (guaranteedPhase && header.sequential)
            header.sequence = (uint8_t)((previousSequence + 1) & 0x7f);
        ServerEvent event;
        event.guaranteed = guaranteedPhase;
        event.sequence = header.sequence;
        event.classId = header.classId;
        if (guaranteedPhase) previousSequence = header.sequence;

        if (header.classId == 7) {
            const uint16_t id = (uint16_t)stream.readUnsigned(10);
            if (stream.readFlag()) {
                const std::string value = stream.readString();
                if (stream.failed() || !strings.set(id, value)) return false;
            }
        } else if (header.classId == 9) {
            const uint32_t argc = stream.readUnsigned(5);
            if (stream.failed() || argc > 20) return false;
            for (uint32_t i = 0; i < argc; ++i) {
                std::string arg = stream.unpackNetString();
                if (arg.size() > 1 && arg[0] == '\x01') {
                    const uint16_t id = (uint16_t)std::strtoul(arg.c_str() + 1, nullptr, 10);
                    if (const std::string* value = strings.get(id)) arg = *value;
                }
                event.arguments.push_back(arg);
                if (!event.message.empty()) event.message.push_back(' ');
                event.message += arg;
            }
        } else if (header.classId == 0) {
            stream.readUnsigned(32);
            stream.readUnsigned(32);
            stream.readUnsigned(32);
            stream.readFlag();
        } else if (header.classId == 1) {
            stream.readUnsigned(32);
            stream.readUnsigned(32);
            stream.readUnsigned(32);
        } else if (header.classId == 3) {
            event.hasGhostAlways = true;
            event.ghostAlwaysIndex = (uint16_t)stream.readUnsigned(10);
            event.ghostAlwaysHasData = stream.readFlag();
            if (event.ghostAlwaysHasData) {
                event.ghostAlwaysClass = (uint8_t)stream.readUnsigned(7);
                // Embedded object data is class-specific and has no length.
                return false;
            }
        } else if (header.classId == 4) {
            event.hasGhostingMessage = true;
            event.ghostSequence = stream.readUnsigned(32);
            event.ghostMessage = (uint8_t)stream.readUnsigned(3);
            event.ghostCount = (uint16_t)stream.readUnsigned(11);
        } else if (header.classId == 5) {
            stream.readF32();
        } else if (header.classId == 6) {
            stream.readPoint3F();
            stream.readPoint3F();
        } else if (header.classId == 12) {
            stream.readUnsigned(4);
            stream.readUnsigned(32);
        } else if (header.classId == 13) {
            event.hasMissionCrc = true;
            event.missionCrc = stream.readU32();
        } else if (header.classId == 17 || header.classId == 18) {
            stream.readRange(0, 1024);
        } else if (header.classId == 20) {
            stream.readRange(0, 1024);
            stream.readRange(0, 1023);
        } else if (header.classId == 19) {
            event.datablockProcess = stream.readFlag();
            if (!event.datablockProcess) {
                events.push_back(std::move(event));
                more = stream.readFlag();
                continue;
            }
            // The definition payload is class-specific and must be decoded
            // before the next event can be located. Metadata alone is not a
            // valid boundary, so reject this packet until a class parser is
            // available rather than desynchronizing the stream.
            event.hasDatablock = true;
            event.datablockObject = (uint16_t)stream.readUnsigned(11);
            event.datablockClass = (uint8_t)stream.readUnsigned(7);
            event.datablockIndex = (uint16_t)stream.readUnsigned(11);
            event.datablockTotal = (uint16_t)stream.readUnsigned(12);
            if (const char* name = dataBlockClassName(event.datablockClass))
                event.datablockClassName = name;
            if (!V12::readDataBlockPayload(stream, event.datablockClass,
                                           &event.datablockData)) return false;
        } else if (header.classId == 22) {
            event.message = stream.readString();
        } else if (header.classId == 23) {
            stream.readUnsigned(4);
        } else if (header.classId == 23) {
            event.hasTargetFree = true;
            event.targetFreeId = (uint16_t)stream.readUnsigned(9);
        } else if (header.classId == 24) {
            event.hasTargetInfo = true;
            event.targetInfo.targetId = (uint16_t)stream.readUnsigned(9);
            auto readTag = [&](bool& present, std::string& value) {
                if (!stream.readFlag()) return;
                present = true;
                const uint16_t tag = stream.readFlag()
                    ? (uint16_t)stream.readUnsigned(10) : 0x400;
                if (tag != 0x400) {
                    if (const std::string* resolved = strings.get(tag)) value = *resolved;
                }
            };
            readTag(event.targetInfo.hasName, event.targetInfo.name);
            readTag(event.targetInfo.hasSkin, event.targetInfo.skin);
            readTag(event.targetInfo.hasSkinPreference, event.targetInfo.skinPreference);
            readTag(event.targetInfo.hasVoice, event.targetInfo.voice);
            readTag(event.targetInfo.hasType, event.targetInfo.type);
            if (stream.readFlag())
                event.targetInfo.sensorGroup = (int)stream.readUnsigned(5);
            if (stream.readFlag())
                event.targetInfo.dataBlockId = stream.readFlag()
                    ? (int)stream.readUnsigned(11) : -2;
            if (stream.readFlag())
                event.targetInfo.renderFlags = (int)stream.readUnsigned(9);
            if (stream.readFlag())
                event.targetInfo.voicePitch = stream.readFloat(7) * 1.5f + 0.5f;
        } else if (header.classId == 25) {
            if (stream.readFlag()) {
                stream.readUnsigned(9);
            }
            if (stream.readFlag()) {
                stream.readF32();
                stream.readF32();
                stream.readF32();
            }
            stream.readFlag();
        } else if (header.classId == 15) {
            event.hasSensorGroup = true;
            event.sensorGroup = (uint8_t)stream.readUnsigned(5);
        } else {
            // Unknown event payload lengths are class-specific; do not guess
            // and desynchronize the following event list.
            return false;
        }
        events.push_back(std::move(event));
        more = stream.readFlag();
    }
    return !stream.failed();
}

bool readServerPacketEvents(V12BitStream& stream, NetStringTable& strings,
                            std::vector<ServerEvent>& events,
                            ServerGameState* state,
                            V12Vec3* compressionPoint,
                            size_t* eventsEnd) {
    stream.setStringBuffer(true);
    const uint32_t rateBits = stream.readUnsigned(2);
    static constexpr int rateFields[] = {4, 6, 8, 10};
    if (stream.failed() || rateBits >= 4) return false;
    for (int i = 0; i < rateFields[rateBits]; ++i) stream.readFloat(7);

    const uint32_t lastMoveAck = stream.readUnsigned(32);
    if (state) state->lastMoveAck = lastMoveAck;
    if (stream.readFlag()) {
        if (stream.readFlag()) {
            const float value = stream.readFloat(7);
            if (state) state->damageFlash = value;
        }
        if (stream.readFlag()) {
            const float value = stream.readFloat(7) * 1.5f;
            if (state) state->whiteOut = value;
        }
    }
    if (stream.readFlag()) {
        stream.readFlag(); // self locked
        stream.readFlag(); // self homed
    }
    if (stream.readFlag()) {
        const bool tracking = stream.readFlag();
        if (tracking) {
            stream.readF32();
            stream.readF32();
            stream.readF32();
        }
        const uint32_t seekerMode = stream.readRange(0, 2);
        if (seekerMode == 1 && stream.readFlag()) stream.readUnsigned(10);
        else if (seekerMode == 2) {
            stream.readF32();
            stream.readF32();
            stream.readF32();
        }
    }
    stream.readFlag(); // pinged
    stream.readFlag(); // jammed
    if (stream.readFlag()) {
        if (state) state->controlPresent = true;
        const bool dirty = stream.readFlag();
        if (state) state->controlDirty = dirty;
        if (dirty) {
            // A derived control-object payload follows the base fields. Its
            // length depends on the datablock, so do not guess its boundary.
            return false;
        }
        const float x = stream.readF32();
        const float y = stream.readF32();
        const float z = stream.readF32();
        if (compressionPoint) *compressionPoint = {x, y, z};
        if (state) {
            state->compressionPoint = {x, y, z};
            state->hasCompressionPoint = true;
        }
    }
    while (stream.readFlag()) {
        stream.readUnsigned(4);
        stream.readUnsigned(32);
    }
    if (stream.readFlag()) {
        const uint8_t fov = (uint8_t)stream.readUnsigned(8);
        if (state) {
            state->hasCameraFov = true;
            state->cameraFov = fov;
        }
    }
    if (stream.failed()) return false;
    const bool result = readServerEvents(stream, strings, events);
    if (eventsEnd) *eventsEnd = stream.position();
    return result;
}

bool NetStringTable::set(uint16_t id, std::string value) {
    if (id >= 4096) return false;
    values[id] = std::move(value);
    return true;
}

const std::string* NetStringTable::get(uint16_t id) const {
    auto it = values.find(id);
    return it == values.end() ? nullptr : &it->second;
}

std::vector<std::pair<uint16_t, std::string>> NetStringTable::entries() const {
    std::vector<std::pair<uint16_t, std::string>> result;
    result.reserve(values.size());
    for (const auto& [id, value] : values) result.emplace_back(id, value);
    return result;
}

} // namespace V12
