#include "net/v12_ghost_packet.h"

namespace V12 {

bool readGhostUpdates(V12BitStream& stream, GhostTracker& tracker,
                      std::vector<GhostUpdate>& updates,
                      const GhostPayloadReader& readPayload) {
    if (!stream.readFlag()) return !stream.failed();
    const uint32_t idBits = stream.readUnsigned(3) + 3;
    if (stream.failed() || idBits > 10) return false;

    bool more = stream.readFlag();
    while (more && !stream.failed()) {
        GhostUpdate update;
        update.index = (uint16_t)stream.readUnsigned((int)idBits);
        const bool deleted = stream.readFlag();
        if (stream.failed()) return false;

        if (deleted) {
            update.operation = GhostUpdate::Operation::Delete;
            update.dataBegin = update.dataEnd = stream.position();
            if (!tracker.erase(update.index)) update.failed = true;
            updates.push_back(update);
        } else {
            const bool initial = tracker.get(update.index) == nullptr;
            update.operation = initial ? GhostUpdate::Operation::Create
                                       : GhostUpdate::Operation::Update;
            if (initial) update.classId = (uint16_t)stream.readUnsigned(7);
            else update.classId = tracker.get(update.index)->classId;
            update.dataBegin = stream.position();
            bool payloadOk = !readPayload || readPayload(stream, update.index,
                                                         update.classId, initial);
            update.dataEnd = stream.position();
            if (!payloadOk || stream.failed()) {
                update.failed = true;
                updates.push_back(update);
                return false;
            }
            if (initial) {
                if (!tracker.create(update.index, update.classId)) {
                    update.failed = true;
                    updates.push_back(update);
                    return false;
                }
            } else if (!tracker.update(update.index)) {
                update.failed = true;
                updates.push_back(update);
                return false;
            }
            updates.push_back(update);
        }
        more = stream.readFlag();
    }
    return !stream.failed();
}

} // namespace V12
