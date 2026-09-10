#include "net/v12_ghosts.h"

#include <cmath>

namespace V12 {

static void readGameBasePayload(V12BitStream& stream, PlayerGhostState* state = nullptr) {
    if (stream.readFlag()) {
        const uint16_t datablock = (uint16_t)stream.readUnsigned(11);
        if (state) {
            state->datablockId = datablock;
            state->hasDatablock = true;
        }
    }
    if (stream.readFlag() && stream.readFlag()) stream.readUnsigned(9);
}

static bool readMissionAreaPayload(V12BitStream& stream) {
    if (stream.readFlag()) {
        for (int i = 0; i < 4; ++i) stream.readUnsigned(32);
        stream.readF32();
        stream.readF32();
    }
    return !stream.failed();
}

static bool readForceFieldBarePayload(V12BitStream& stream) {
    readGameBasePayload(stream);
    if (stream.readFlag()) {
        stream.readAffineTransform();
        stream.readPoint3F();
    } else if (stream.readFlag()) {
        stream.readAffineTransform();
        stream.readPoint3F();
    }
    if (stream.readFlag()) {
        const uint32_t state = stream.readUnsigned(2);
        if (state == 1 || state == 2) stream.readU32();
    }
    return !stream.failed();
}

static bool readPrecipitationPayload(V12BitStream& stream) {
    readGameBasePayload(stream);
    if (stream.readFlag()) {
        stream.readF32();
        const int32_t colorCount = stream.readSigned(32);
        if (colorCount < 0 || colorCount > 3) return false;
        for (int32_t i = 0; i < colorCount; ++i) {
            for (int channel = 0; channel < 4; ++channel) stream.readU8();
        }
        stream.readF32();
        stream.readF32();
        stream.readF32();
        stream.readSigned(32);
        stream.readF32();
        if (stream.readFlag()) {
            stream.readF32();
            stream.readF32();
            stream.readF32();
        }
    }
    if (stream.readFlag()) stream.readBool();
    if (stream.readFlag()) {
        stream.readF32();
        stream.readF32();
    }
    if (stream.readFlag()) stream.readF32();
    return !stream.failed();
}

static bool readSunPayload(V12BitStream& stream) {
    if (stream.readFlag()) {
        stream.readPoint3F();
        stream.readFloat(8);
        stream.readFloat(8);
        for (int i = 0; i < 3; ++i) stream.readUnsigned(8);
        stream.readFlag();
        stream.readFlag();
    }
    return !stream.failed();
}

static bool readTSStaticPayload(V12BitStream& stream) {
    for (int i = 0; i < 16; ++i) stream.readF32();
    stream.readPoint3F();
    return !stream.failed();
}

static bool readWaterBlockPayload(V12BitStream& stream) {
    stream.readAffineTransform();
    stream.readPoint3F();
    stream.readString();
    stream.readString();
    for (int i = 0; i < 2; ++i) stream.readString();
    stream.readSigned(32);
    for (int i = 0; i < 4; ++i) stream.readF32();
    stream.readU8();
    if (stream.readFlag()) stream.readUnsigned(11);
    return !stream.failed();
}

static bool readVehicleBlockerPayload(V12BitStream& stream) {
    for (int i = 0; i < 16; ++i) stream.readF32();
    stream.readPoint3F();
    stream.readPoint3F();
    return !stream.failed();
}

static bool readStationFXPayload(V12BitStream& stream, bool initial) {
    readGameBasePayload(stream);
    if (initial && stream.readFlag() && stream.readFlag())
        stream.readRange(0, 1024);
    return !stream.failed();
}

static bool readFireballAtmospherePayload(V12BitStream& stream, bool initial) {
    readGameBasePayload(stream);
    if (initial && stream.readFlag()) {
        for (int i = 0; i < 9; ++i) stream.readF32();
    }
    return !stream.failed();
}

static bool readSplashPayload(V12BitStream& stream) {
    readGameBasePayload(stream);
    if (stream.readFlag()) stream.readPoint3F();
    return !stream.failed();
}

static bool readShockwavePayload(V12BitStream& stream) {
    readGameBasePayload(stream);
    if (stream.readFlag()) {
        stream.readPoint3F();
        stream.readPoint3F();
    }
    return !stream.failed();
}

static bool readShapeBasePayload(V12BitStream& stream, bool initial,
                                  PlayerGhostState* state) {
    readGameBasePayload(stream, state);
    if (!stream.readFlag()) return !stream.failed();
    if (stream.readFlag()) {
        const float damage = stream.readFloat(6);
        if (state) {
            state->health = (1.0f - damage) * 100.0f;
            state->hasHealth = true;
        }
        stream.readUnsigned(2);
        stream.readFlag();
        stream.readNormalVector(8);
    }
    if (stream.readFlag()) {
        for (int i = 0; i < 4; ++i) {
            if (stream.readFlag()) {
                const bool playing = stream.readFlag();
                if (playing) stream.readUnsigned(11);
            }
        }
    }
    if (stream.readFlag()) {
        for (int i = 0; i < 4; ++i) {
            if (stream.readFlag()) {
                const int sequence = (int)stream.readUnsigned(5);
                const int threadState = (int)stream.readUnsigned(2);
                const float timescale = stream.readF32();
                const float position = stream.readF32();
                const bool atEnd = stream.readFlag();
                if (state) {
                    state->threads[i].sequence = sequence;
                    state->threads[i].state = threadState;
                    state->threads[i].timescale = timescale;
                    state->threads[i].position = position;
                    state->threads[i].atEnd = atEnd;
                    state->threads[i].valid = true;
                }
            }
        }
    }
    if (stream.readFlag()) {
        for (int i = 0; i < 8; ++i) {
            if (stream.readFlag()) {
                if (stream.readFlag()) stream.readUnsigned(11);
                if (stream.readFlag()) {
                    if (stream.readFlag()) stream.readUnsigned(10);
                    else stream.readHuffmanString();
                }
                for (int j = 0; j < 5; ++j) stream.readFlag();
                stream.readUnsigned(3);
                if (initial) stream.readFlag();
            }
        }
    }
    if (stream.readFlag()) {
        if (stream.readFlag()) {
            if (state) state->moving = stream.readFlag();
            else stream.readFlag();
            stream.readFlag();
            if (stream.readFlag()) {
                stream.readFlag();
                stream.readF32();
            }
        }
        if (stream.readFlag()) {
            if (stream.readFlag()) {
                stream.readFlag();
            } else {
                stream.readNormalVector(8);
                stream.readFloat(5);
            }
        }
        if (stream.readFlag()) {
                stream.readUnsigned(32);
                stream.readUnsigned(32);
        }
    }
    if (stream.readFlag()) {
        if (stream.readFlag()) {
            stream.readUnsigned(10);
            stream.readUnsigned(5);
        }
    }
    return !stream.failed();
}

static void readMove(V12BitStream& stream) {
    if (stream.readFlag()) stream.readUnsigned(16);
    if (stream.readFlag()) stream.readUnsigned(16);
    if (stream.readFlag()) stream.readUnsigned(16);
    stream.readUnsigned(6);
    stream.readUnsigned(6);
    stream.readUnsigned(6);
    stream.readFlag();
    for (int i = 0; i < 6; ++i) stream.readFlag();
}

static bool readVehiclePayload(V12BitStream& stream, const V12Vec3& compressionPoint) {
    stream.readFlag(); // jetting
    if (stream.readFlag()) return !stream.failed(); // control object shortcut
    stream.readFloat(9);
    stream.readFloat(9);
    readMove(stream);
    stream.readFlag(); // frozen
    if (stream.readFlag()) {
        stream.readCompressedPoint(compressionPoint);
        for (int i = 0; i < 4; ++i) stream.readF32();
        stream.readPoint3F();
        stream.readPoint3F();
    }
    if (stream.readFlag()) stream.readFloat(8);
    return !stream.failed();
}

static bool readStaticShapePayload(V12BitStream& stream, bool initial,
                                   const V12Vec3& compressionPoint,
                                   PlayerGhostState* state = nullptr) {
    if (!readShapeBasePayload(stream, initial, state)) return false;
    if (stream.readFlag()) {
        const V12AffineTransform transform = stream.readAffineTransform(compressionPoint);
        if (state) {
            state->position = transform.position;
            state->hasPosition = true;
            state->rotation = {transform.x, transform.y, transform.z};
            state->rotationW = transform.w;
            state->hasRotation = true;
        }
        stream.readPoint3F();
    }
    stream.readFlag(); // powered
    return !stream.failed();
}

static bool readItemPayload(V12BitStream& stream, bool initial,
                            const V12Vec3& compressionPoint,
                            PlayerGhostState* state) {
    if (!readShapeBasePayload(stream, initial, state)) return false;
    if (stream.readFlag()) {
        stream.readFlag();
        stream.readFlag();
        stream.readFlag();
        if (stream.readFlag()) {
            if (state) {
                state->position = stream.readPoint3F();
                state->hasPosition = true;
            } else {
                stream.readPoint3F();
            }
        }
    }
    if (stream.readFlag()) stream.readUnsigned(10);
    if (stream.readFlag()) {
        stream.readFlag();
        stream.readF32();
    }
    if (stream.readFlag()) {
        if (state) {
            state->position = stream.readPoint3F();
            state->hasPosition = true;
        } else {
            stream.readPoint3F();
        }
        const bool atRest = stream.readFlag();
        if (!atRest) stream.readPoint3F();
        stream.readFlag();
    }
    (void)compressionPoint;
    return !stream.failed();
}

static bool readMissionMarkerPayload(V12BitStream& stream, bool initial,
                                     const V12Vec3& compressionPoint,
                                     PlayerGhostState* state = nullptr) {
    if (!readShapeBasePayload(stream, initial, state)) return false;
    if (stream.readFlag()) {
        const V12AffineTransform transform = stream.readAffineTransform(compressionPoint);
        if (state) {
            state->position = transform.position;
            state->hasPosition = true;
            state->rotation = {transform.x, transform.y, transform.z};
            state->rotationW = transform.w;
            state->hasRotation = true;
        }
        stream.readPoint3F();
    }
    return !stream.failed();
}

static bool readAIObjectivePayload(V12BitStream& stream, bool initial,
                                   const V12Vec3& compressionPoint) {
    if (!readMissionMarkerPayload(stream, initial, compressionPoint)) return false;
    stream.readFlag(); // sphere update
    return !stream.failed();
}

static bool readTurretPayload(V12BitStream& stream, bool initial,
                              const V12Vec3& compressionPoint) {
    if (!readStaticShapePayload(stream, initial, compressionPoint)) return false;
    if (stream.readFlag()) return !stream.failed(); // controlling object
    if (stream.readFlag()) {
        stream.readFloat(10); // phi
        stream.readFloat(10); // theta
        stream.readFloat(8);  // activation
    }
    return !stream.failed();
}

static bool readWayPointPayload(V12BitStream& stream, bool initial,
                                const V12Vec3& compressionPoint) {
    if (!readMissionMarkerPayload(stream, initial, compressionPoint)) return false;
    if (stream.readFlag()) stream.readString();
    if (stream.readFlag()) stream.readSigned(32);
    if (stream.readFlag()) stream.readFlag();
    return !stream.failed();
}

static bool readLinearProjectilePayload(V12BitStream& stream, bool initial,
                                         const V12Vec3& compressionPoint) {
    readGameBasePayload(stream);
    if (stream.readFlag()) {
        if (stream.readFlag()) {
            stream.readCompressedPoint(compressionPoint);
            stream.readNormalVector(14);
            stream.readFlag();
        } else {
            stream.readCompressedPoint(compressionPoint);
            stream.readNormalVector(14);
            stream.readRange(0, 511);
            if (stream.readFlag()) {
                stream.readUnsigned(10);
                stream.readRange(0, 7);
                if (stream.readFlag()) {
                    stream.readRange(0, 255);
                    stream.readNormalVector(7);
                }
            }
            if (stream.readFlag()) stream.readUnsigned(10);
        }
    } else {
        stream.readCompressedPoint(compressionPoint);
        stream.readNormalVector(14);
        stream.readFlag();
    }
    (void)initial;
    return !stream.failed();
}

static bool readSniperProjectilePayload(V12BitStream& stream, bool initial) {
    readGameBasePayload(stream);
    if (stream.readFlag()) {
        stream.readFloat(7);
        stream.readPoint3F();
        stream.readPoint3F();
        stream.readFlag();
        stream.readFlag();
        if (stream.readFlag()) {
            stream.readUnsigned(11);
            stream.readRange(0, 7);
            stream.readFlag();
        }
    } else {
        if (stream.readFlag()) {
            stream.readUnsigned(11);
            stream.readRange(0, 7);
            stream.readFlag();
        } else {
            stream.readPoint3F();
        }
        stream.readPoint3F();
        stream.readFlag();
    }
    (void)initial;
    return !stream.failed();
}

static bool readBombProjectilePayload(V12BitStream& stream) {
    readGameBasePayload(stream);
    if (!stream.readFlag()) {
        if (stream.readFlag()) {
            stream.readPoint3F();
            stream.readPoint3F();
        }
        if (!stream.readFlag()) return !stream.failed();
        stream.readPoint3F();
        stream.readPoint3F();
        return !stream.failed();
    }
    stream.readPoint3F();
    stream.readPoint3F();
    stream.readUnsigned(12);
    if (stream.readFlag()) stream.readFlag();
    if (stream.readFlag()) {
        stream.readPoint3F();
        stream.readPoint3F();
    }
    if (stream.readFlag()) {
        stream.readUnsigned(11);
        stream.readUnsigned(3);
        stream.readFlag();
    }
    if (stream.readFlag()) stream.readUnsigned(11);
    return !stream.failed();
}

static bool readGrenadeProjectilePayload(V12BitStream& stream, bool initial) {
    readGameBasePayload(stream);
    if (stream.readFlag()) {
        stream.readPoint3F();
        stream.readPoint3F();
        stream.readRange(0, 4095);
        stream.readFlag();
        if (stream.readFlag()) {
            stream.readPoint3F();
            stream.readPoint3F();
        }
        if (stream.readFlag()) {
            stream.readUnsigned(11);
            stream.readUnsigned(3);
        }
        if (stream.readFlag()) stream.readUnsigned(11);
    } else {
        if (stream.readFlag()) {
            stream.readPoint3F();
            stream.readPoint3F();
        }
        if (stream.readFlag()) {
            stream.readPoint3F();
            stream.readPoint3F();
        }
    }
    (void)initial;
    return !stream.failed();
}

static bool readSeekerProjectilePayload(V12BitStream& stream) {
    readGameBasePayload(stream);
    const bool fullState = stream.readFlag();
    if (!fullState) {
        if (stream.readFlag()) {
            stream.readPoint3F();
            stream.readPoint3F();
            return !stream.failed();
        }
        stream.readPoint3F();
        stream.readPoint3F();
        if (stream.readFlag()) {
            if (stream.readFlag()) stream.readUnsigned(11);
            else stream.readPoint3F();
        }
        return !stream.failed();
    }
    stream.readPoint3F();
    stream.readPoint3F();
    stream.readPoint3F();
    if (stream.readFlag()) {
        stream.readUnsigned(11);
        stream.readUnsigned(3);
    }
    if (stream.readFlag()) {
        if (stream.readFlag()) stream.readUnsigned(11);
        else stream.readPoint3F();
    }
    stream.readFlag();
    return !stream.failed();
}

static bool readELFProjectilePayload(V12BitStream& stream) {
    readGameBasePayload(stream);
    if (stream.readFlag() && stream.readFlag()) {
        stream.readUnsigned(11);
        stream.readUnsigned(3);
        stream.readUnsigned(11);
    }
    return !stream.failed();
}

static bool readRepairProjectilePayload(V12BitStream& stream) {
    readGameBasePayload(stream);
    if (stream.readFlag() && stream.readFlag()) {
        stream.readUnsigned(11);
        stream.readUnsigned(3);
        stream.readUnsigned(11);
    }
    return !stream.failed();
}

static bool readTargetProjectilePayload(V12BitStream& stream, bool initial) {
    readGameBasePayload(stream);
    if (initial && stream.readFlag()) {
        stream.readPoint3F();
        stream.readPoint3F();
        stream.readFlag();
        if (stream.readFlag()) {
            stream.readUnsigned(11);
            stream.readUnsigned(3);
            stream.readFlag();
        }
        return !stream.failed();
    }
    if (stream.readFlag()) {
        stream.readUnsigned(11);
        stream.readUnsigned(3);
        stream.readFlag();
    } else {
        stream.readPoint3F();
    }
    stream.readPoint3F();
    stream.readFlag();
    return !stream.failed();
}

static bool readLightningPayload(V12BitStream& stream, bool initial) {
    readGameBasePayload(stream);
    if (stream.readFlag()) {
        stream.readPoint3F();
        stream.readPoint3F();
        for (int i = 0; i < 4; ++i) stream.readF32();
        for (int i = 0; i < 3; ++i) stream.readF32();
        for (int i = 0; i < 3; ++i) stream.readF32();
        stream.readU8();
        stream.readF32();
    }
    (void)initial;
    return !stream.failed();
}

static bool readParticleEmissionDummyPayload(V12BitStream& stream) {
    readGameBasePayload(stream);
    for (int i = 0; i < 16; ++i) stream.readF32();
    stream.readPoint3F();
    if (stream.readFlag()) stream.readUnsigned(11);
    return !stream.failed();
}

static bool readAudioEmitterPayload(V12BitStream& stream) {
    stream.readFlag();
    if (stream.readFlag()) stream.readAffineTransform();
    if (stream.readFlag()) {
        if (stream.readFlag()) stream.readUnsigned(11);
    }
    if (stream.readFlag()) {
        if (stream.readFlag()) stream.readUnsigned(11);
    }
    if (stream.readFlag()) stream.readString();
    if (stream.readFlag()) stream.readFlag();
    if (stream.readFlag()) stream.readF32();
    if (stream.readFlag()) stream.readFlag();
    if (stream.readFlag()) stream.readFlag();
    if (stream.readFlag()) stream.readF32();
    if (stream.readFlag()) stream.readF32();
    if (stream.readFlag()) stream.readSigned(32);
    if (stream.readFlag()) stream.readSigned(32);
    if (stream.readFlag()) stream.readF32();
    if (stream.readFlag()) stream.readPoint3F();
    if (stream.readFlag()) stream.readSigned(32);
    if (stream.readFlag()) stream.readSigned(32);
    if (stream.readFlag()) stream.readSigned(32);
    if (stream.readFlag()) stream.readSigned(32);
    if (stream.readFlag()) stream.readFlag();
    return !stream.failed();
}

static bool readBeaconObjectPayload(V12BitStream& stream, bool initial,
                                    const V12Vec3& compressionPoint) {
    if (!readStaticShapePayload(stream, initial, compressionPoint)) return false;
    if (stream.readFlag()) stream.readUnsigned(2);
    return !stream.failed();
}

static bool readDebrisPayload(V12BitStream& stream, bool initial) {
    readGameBasePayload(stream);
    for (int i = 0; i < 6; ++i) stream.readF32();
    for (int i = 0; i < 4; ++i) stream.readBool();
    for (int i = 0; i < 6; ++i) stream.readF32();
    for (int i = 0; i < 2; ++i) stream.readBool();
    for (int i = 0; i < 3; ++i) stream.readF32();
    stream.readBool();
    stream.readString();
    stream.readString();
    for (int i = 0; i < 3; ++i) {
        if (stream.readFlag()) stream.readUnsigned(11);
    }
    (void)initial;
    return !stream.failed();
}

static bool readPhysicalZonePayload(V12BitStream& stream, bool initial) {
    if (stream.readFlag()) {
        for (int i = 0; i < 16; ++i) stream.readF32();
        stream.readPoint3F();

        const uint32_t pointCount = stream.readU32();
        if (pointCount > stream.remainingBits() / 96) return false;
        for (uint32_t i = 0; i < pointCount; ++i) stream.readPoint3F();

        const uint32_t planeCount = stream.readU32();
        if (planeCount > stream.remainingBits() / 128) return false;
        for (uint32_t i = 0; i < planeCount; ++i) {
            for (int j = 0; j < 4; ++j) stream.readF32();
        }

        const uint32_t edgeCount = stream.readU32();
        if (edgeCount > stream.remainingBits() / 128) return false;
        for (uint32_t i = 0; i < edgeCount; ++i) {
            for (int j = 0; j < 4; ++j) stream.readU32();
        }
        stream.readF32();
        stream.readF32();
        stream.readPoint3F();
    }
    (void)initial;
    stream.readFlag();
    return !stream.failed();
}

static bool readTerrainBlockPayload(V12BitStream& stream, bool initial) {
    const auto readEmptySquareRuns = [&stream]() -> bool {
        const uint32_t count = stream.readU32();
        if (count > stream.remainingBits() / 32) return false;
        for (uint32_t i = 0; i < count; ++i) stream.readU32();
        return !stream.failed();
    };

    if (stream.readFlag()) {
        stream.readU32();
        stream.readString();
        stream.readString();
        stream.readU32();
        if (!readEmptySquareRuns()) return false;
    } else if (stream.readFlag() && !readEmptySquareRuns()) {
        return false;
    }
    return !stream.failed();
}

static bool readSkyPayload(V12BitStream& stream) {
    if (stream.readFlag()) {
        stream.readString();
        for (int i = 0; i < 3; ++i) stream.readF32();

        const uint32_t fogVolumeCount = stream.readU32();
        if (fogVolumeCount > 64 ||
            fogVolumeCount > stream.remainingBits() / 192) return false;
        stream.readBool();
        stream.readBool();
        for (int i = 0; i < 3; ++i) stream.readF32();
        stream.readBool();
        for (uint32_t i = 0; i < fogVolumeCount; ++i) {
            for (int j = 0; j < 6; ++j) stream.readF32();
        }
        for (int i = 0; i < 3; ++i) {
            stream.readString();
            stream.readF32();
            stream.readF32();
        }
        stream.readPoint3F();
        stream.readF32();
        if (stream.readFlag()) {
            for (int i = 0; i < 5; ++i) stream.readF32();
        }
    }
    if (stream.readFlag()) stream.readBool();
    if (stream.readFlag()) stream.readBool();
    if (stream.readFlag()) {
        stream.readF32();
        stream.readF32();
    }
    if (stream.readFlag()) {
        stream.readF32();
        stream.readF32();
    }
    if (stream.readFlag()) {
        for (int i = 0; i < 3; ++i) stream.readF32();
    }
    if (stream.readFlag()) {
        for (int i = 0; i < 4; ++i) stream.readF32();
    }
    if (stream.readFlag()) stream.readPoint3F();
    return !stream.failed();
}

static bool readShockLanceProjectilePayload(V12BitStream& stream, bool initial) {
    readGameBasePayload(stream);
    if (stream.readFlag()) stream.readUnsigned(11);
    if (stream.readFlag()) {
        stream.readPoint3F();
        stream.readPoint3F();
        stream.readFlag();
        if (stream.readFlag()) {
            stream.readUnsigned(11);
            stream.readUnsigned(3);
        }
    }
    (void)initial;
    return !stream.failed();
}

bool readGhostPayload(V12BitStream& stream, uint16_t classId, bool initial,
                      const V12Vec3& compressionPoint,
                      PlayerGhostState* playerState) {
    switch (classId) {
    case 0: return readAIObjectivePayload(stream, initial, compressionPoint);
    case 1: return readAudioEmitterPayload(stream);
    case 2: return readBeaconObjectPayload(stream, initial, compressionPoint);
    case 5: return readDebrisPayload(stream, initial);
    case 7:  // EnergyProjectile: same wire format as GrenadeProjectile
    case 9:  // FlareProjectile: same wire format as GrenadeProjectile
    case 13: // GrenadeProjectile
        return readGrenadeProjectilePayload(stream, initial);
    case 17: return readLightningPayload(stream, initial);
    case 23: return readParticleEmissionDummyPayload(stream);
    case 24: return readPhysicalZonePayload(stream, initial);
    case 29: return readStaticShapePayload(stream, initial, compressionPoint, playerState);
    case 18: // LinearFlareProjectile: same wire format as LinearProjectile
    case 19: // LinearProjectile
    case 46: // TracerProjectile: same wire format as LinearProjectile
        return readLinearProjectilePayload(stream, initial, compressionPoint);
    case 8: return readFireballAtmospherePayload(stream, initial);
    case 11: return readForceFieldBarePayload(stream);
    case 3: return readBombProjectilePayload(stream);
    case 6: return readELFProjectilePayload(stream);
    case 4: { // Camera
        if (!readShapeBasePayload(stream, initial, nullptr)) return false;
        if (stream.readFlag()) return !stream.failed();
        if (stream.readFlag()) for (int i = 0; i < 5; ++i) stream.readF32();
        return !stream.failed();
    }
    case 10: // FlyingVehicle
        if (!readShapeBasePayload(stream, initial, nullptr) ||
            !readVehiclePayload(stream, compressionPoint)) return false;
        if (stream.readFlag()) return !stream.failed();
        stream.readFlag(); stream.readUnsigned(3);
        return !stream.failed();
    case 14: // HoverVehicle
        return readShapeBasePayload(stream, initial, nullptr) &&
               readVehiclePayload(stream, compressionPoint) &&
               (stream.readUnsigned(3), !stream.failed());
    case 16: return readItemPayload(stream, initial, compressionPoint, playerState);
    case 20: // Marker
        stream.readPoint3F(); return !stream.failed();
    case 21: return readMissionAreaPayload(stream);
    case 22: return readMissionMarkerPayload(stream, initial, compressionPoint, playerState);
    case 25: return readPlayerGhostPayload(stream, initial, compressionPoint, playerState);
    case 26: return readPrecipitationPayload(stream);
    case 12: // GameBase
    case 27: // Projectile inherits the same wire layer
        readGameBasePayload(stream); return !stream.failed();
    case 28: return readRepairProjectilePayload(stream);
    case 44: return readTargetProjectilePayload(stream, initial);
    case 30: return readSeekerProjectilePayload(stream);
    case 31: return readShapeBasePayload(stream, initial, nullptr);
    case 33: return readShockwavePayload(stream);
    case 32: return readShockLanceProjectilePayload(stream, initial);
    case 34: // SimpleNetObject
        stream.readString(); return !stream.failed();
    case 35: return readSkyPayload(stream);
    case 37: {
        if (!readMissionMarkerPayload(stream, initial, compressionPoint, playerState)) return false;
        if (stream.readFlag()) {
            stream.readF32();
            stream.readF32();
            stream.readF32();
            stream.readF32();
        }
        return !stream.failed();
    }
    case 38: return readSplashPayload(stream);
    case 36: return readSniperProjectilePayload(stream, initial);
    case 39: return readStaticShapePayload(stream, initial, compressionPoint, playerState);
    case 40: // StationFXPersonal
    case 41: // StationFXVehicle
        return readStationFXPayload(stream, initial);
    case 42: return readSunPayload(stream);
    case 43: return readTSStaticPayload(stream);
    case 45: return readTerrainBlockPayload(stream, initial);
    case 47: return stream.readU32(), !stream.failed();
    case 48: return readTurretPayload(stream, initial, compressionPoint);
    case 49: return readVehicleBlockerPayload(stream);
    case 50: return readWaterBlockPayload(stream);
    case 51: return readWayPointPayload(stream, initial, compressionPoint);
    case 52: {
        if (!readShapeBasePayload(stream, initial, nullptr) ||
            !readVehiclePayload(stream, compressionPoint)) return false;
        stream.readFlag();
        for (int i = 0; i < 4; ++i) {
            stream.readF32(); stream.readF32(); stream.readF32();
        }
        return !stream.failed();
    }
    default: return false;
    }
}

bool readPlayerGhostPayload(V12BitStream& stream, bool initial,
                            const V12Vec3& compressionPoint,
                            PlayerGhostState* state) {
    if (!readShapeBasePayload(stream, initial, state)) return false;
    if (stream.readFlag()) stream.readUnsigned(3);
    if (stream.readFlag()) {
        stream.readUnsigned(8);
        stream.readFlag();
        const bool atEnd = stream.readFlag();
        stream.readFlag();
        if (!atEnd && stream.readFlag()) stream.readSignedFloat(6);
    }
    if (stream.readFlag()) stream.readUnsigned(8);
    if (stream.readFlag()) return !stream.failed();
    if (stream.readFlag()) {
        const uint32_t actionState = stream.readUnsigned(3);
        if (state) {
            state->moving = actionState > 0;
            state->hasMovement = true;
        }
        if (stream.readFlag()) stream.readUnsigned(7);
        stream.readFlag();
        stream.readFlag();
        if (state) {
            state->position = stream.readCompressedPoint(compressionPoint);
            state->hasPosition = true;
        } else stream.readCompressedPoint(compressionPoint);
        if (stream.readFlag()) {
            stream.readUnsigned(13);
            stream.readNormalVector(10);
        }
        const float headPitch = stream.readSignedFloat(6);
        const float headYaw = stream.readSignedFloat(6);
        const float bodyYaw = stream.readFloat(7) * 6.28318530717958647692f;
        if (state) {
            state->headPitch = headPitch;
            state->headYaw = headYaw;
            state->hasHeadAngles = true;
            const float half = bodyYaw * 0.5f;
            state->rotation = {0.0f, (float)std::sin(half), 0.0f};
            state->rotationW = (float)std::cos(half);
            state->hasRotation = true;
        }
        for (int i = 0; i < 3; ++i)
            if (stream.readFlag()) stream.readUnsigned(16);
        stream.readUnsigned(6);
        stream.readUnsigned(6);
        stream.readUnsigned(6);
        stream.readFlag();
        for (int i = 0; i < 6; ++i) stream.readFlag();
        stream.readFlag();
    }
    const float energy = stream.readFloat(5);
    if (state) {
        state->energy = energy * 100.0f;
        state->hasEnergy = true;
    }
    return !stream.failed();
}

bool readItemGhostPayload(V12BitStream& stream, bool initial,
                          const V12Vec3& compressionPoint,
                          PlayerGhostState* state) {
    return readItemPayload(stream, initial, compressionPoint, state);
}

PlayerGhostState mergePlayerGhostState(const PlayerGhostState& base,
                                       const PlayerGhostState& update) {
    PlayerGhostState merged = base;
    if (update.hasDatablock) {
        merged.datablockId = update.datablockId;
        merged.hasDatablock = true;
    }
    if (update.hasHealth) {
        merged.health = update.health;
        merged.hasHealth = true;
    }
    if (update.hasEnergy) {
        merged.energy = update.energy;
        merged.hasEnergy = true;
    }
    if (update.hasPosition) {
        merged.position = update.position;
        merged.hasPosition = true;
    }
    if (update.hasHeadAngles) {
        merged.headPitch = update.headPitch;
        merged.headYaw = update.headYaw;
        merged.hasHeadAngles = true;
    }
    if (update.hasRotation) {
        merged.rotation = update.rotation;
        merged.rotationW = update.rotationW;
        merged.hasRotation = true;
    }
    if (update.hasMovement) {
        merged.moving = update.moving;
        merged.hasMovement = true;
    }
    for (int i = 0; i < 4; ++i) {
        if (update.threads[i].valid) merged.threads[i] = update.threads[i];
    }
    return merged;
}

bool GhostTracker::create(uint16_t index, uint16_t classId) {
    if (index >= 1024 || classId >= GhostClassCount) return false;
    if (ghosts.contains(index)) return false;
    ghosts.emplace(index, GhostEntry{index, classId, ghostClassName(classId)});
    return true;
}

bool GhostTracker::update(uint16_t index) {
    return ghosts.contains(index);
}

bool GhostTracker::erase(uint16_t index) {
    return ghosts.erase(index) != 0;
}

const GhostEntry* GhostTracker::get(uint16_t index) const {
    auto it = ghosts.find(index);
    return it == ghosts.end() ? nullptr : &it->second;
}

} // namespace V12
