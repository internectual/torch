#include "net/v12_events.h"
#include "net/v12_protocol.h"
#include "game/voice_stream.h"

#include <array>
#include <cassert>
#include <cstdint>

int main() {
    assert(VoiceStream::acceptedCaptureSamples(0, 2048) == 2048);
    assert(VoiceStream::acceptedCaptureSamples(39990, 32) == 10);
    assert(VoiceStream::acceptedCaptureSamples(40000, 32) == 0);
    assert(VoiceStream::startsNewBurst(false, 1, -1));
    assert(VoiceStream::startsNewBurst(true, 0, 12));
    assert(!VoiceStream::startsNewBurst(true, 0, 127));
    assert(!VoiceStream::startsNewBurst(true, 0, 123));
    assert(VoiceStream::startsNewBurst(true, 0, 127, true));
    assert(!VoiceStream::startsNewBurst(true, 1, 12));
    {
        std::array<uint8_t, 33> payload{};
        for (int i = 0; i < 33; ++i) payload[i] = static_cast<uint8_t>(255 - i);
        const auto event = V12::makeVoiceStreamEvent(17, 3, 1, false, {payload});
        V12BitWriter packet;
        packet.writeFlag(true); // an unguaranteed event follows
        packet.writeUnsigned(event.classId, V12::EventClassBits);
        event.write(packet);
        packet.writeFlag(false); // end unguaranteed events
        packet.writeFlag(false); // end guaranteed events
        V12BitStream stream(packet.data().data(), packet.data().size());
        V12::NetStringTable strings;
        std::vector<V12::ServerEvent> events;
        constexpr uint32_t senderId = 0xabcdef01;
        assert(V12::readServerEvents(stream, strings, events, {}, false, senderId));
        assert(events.size() == 1);
        const auto& decoded = events.front();
        assert(decoded.hasVoiceStream && decoded.voiceSequence == 17);
        assert(decoded.voiceCodec == 3 && decoded.voiceStream == 1);
        assert(decoded.voiceClientId == senderId && !decoded.voiceEndOfStream);
        assert(decoded.voiceFrames.size() == 1 && decoded.voiceFrames[0] == payload);
    }
    {
        std::vector<std::array<uint8_t, 33>> payload(2);
        payload[0].fill(0x3c);
        payload[1].fill(0xc3);
        const auto event = V12::makeVoiceStreamEvent(18, 3, 1, true, payload);
        V12BitWriter packet;
        packet.writeFlag(true);
        packet.writeUnsigned(event.classId, V12::EventClassBits);
        event.write(packet);
        packet.writeFlag(false);
        packet.writeFlag(false);
        V12BitStream stream(packet.data().data(), packet.data().size());
        V12::NetStringTable strings;
        std::vector<V12::ServerEvent> events;
        assert(V12::readServerEvents(stream, strings, events, {}, false, 5));
        assert(events.size() == 1 && events[0].voiceEndOfStream);
        assert(events[0].voiceFrames == payload);
    }

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
