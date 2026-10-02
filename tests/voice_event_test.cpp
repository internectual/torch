#include "net/v12_events.h"

#include <array>
#include <cassert>
#include <cstdint>

int main() {
    V12BitWriter writer;
    writer.writeFlag(true);
    writer.writeUnsigned(21, V12::EventClassBits);
    writer.writeUnsigned(7, 7);
    writer.writeUnsigned(3, 2);
    writer.writeUnsigned(2, 2);
    writer.writeUnsigned(0x12345678, 32);
    writer.writeFlag(true);
    writer.writeUnsigned(1, 5);
    for (int i = 0; i < 33; ++i) writer.writeUnsigned((uint8_t)(i * 3), 8);
    writer.writeFlag(false);
    writer.writeFlag(false);

    V12BitStream stream(writer.data().data(), writer.data().size());
    V12::NetStringTable strings;
    std::vector<V12::ServerEvent> events;
    assert(V12::readServerEvents(stream, strings, events));
    assert(events.size() == 1);
    const auto& event = events.front();
    assert(event.hasVoiceStream && event.voiceSequence == 7);
    assert(event.voiceCodec == 3 && event.voiceStream == 2);
    assert(event.voiceClientId == 0x12345678 && event.voiceEndOfStream);
    assert(event.voiceFrames.size() == 1 && event.voiceFrames[0][10] == 30);

    V12BitWriter eosWriter;
    eosWriter.writeFlag(true);
    eosWriter.writeUnsigned(21, V12::EventClassBits);
    eosWriter.writeUnsigned(8, 7);
    eosWriter.writeUnsigned(3, 2);
    eosWriter.writeUnsigned(1, 2);
    eosWriter.writeUnsigned(9, 32);
    eosWriter.writeFlag(true);
    eosWriter.writeUnsigned(0, 5);
    eosWriter.writeFlag(false);
    eosWriter.writeFlag(false);
    V12BitStream eosStream(eosWriter.data().data(), eosWriter.data().size());
    events.clear();
    assert(V12::readServerEvents(eosStream, strings, events));
    assert(events.size() == 1 && events.front().voiceEndOfStream);
    assert(events.front().voiceFrames.empty());
    return 0;
}
