#pragma once

#include "sim/game_connection.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace LiveMovePolicy {

constexpr float TwoPi = 6.28318530717958647692f;

inline float angleRadians(int16_t angle) {
    return static_cast<float>(angle) * (TwoPi / 65536.0f);
}

struct Input {
    bool forward = false;
    bool backward = false;
    bool left = false;
    bool right = false;
    bool jump = false;
    bool jet = false;
    bool fire = false;
    bool altFire = false;
    bool freeLook = false;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
};

inline void accumulateLook(float& pitch, float& yaw, float pitchDelta, float yawDelta) {
    if (std::isfinite(pitchDelta)) pitch += pitchDelta;
    if (std::isfinite(yawDelta)) yaw += yawDelta;
}

inline bool shouldCaptureMouse(bool gameplayActive, bool targetFinderOpen,
                               bool liveClient, bool liveMatchEnded) {
    return gameplayActive && !targetFinderOpen && !(liveClient && liveMatchEnded);
}

inline bool shouldResumeLiveGameForContent(const std::string& content,
                                           bool liveClient, bool livePlaybackActive) {
    return content == "PlayGui" && liveClient && livePlaybackActive;
}

inline ClientMoveIn makeMove(const Input& input) {
    auto angle = [](float radians) {
        if (!std::isfinite(radians)) return int16_t{0};
        const float wrapped = std::remainder(radians, TwoPi);
        return (int16_t)(uint16_t)((uint32_t)(int64_t)((wrapped / TwoPi) * 0x10000) & 0xFFFF);
    };
    auto axis = [](float value) {
        return value < -1.0f ? 0 : value > 1.0f ? 32 : (int)((value + 1.0f) * 16.0f);
    };

    ClientMoveIn move;
    move.pitch = angle(input.pitch);
    move.yaw = angle(input.yaw);
    move.roll = angle(input.roll);
    move.x = axis((input.right ? 1.0f : 0.0f) - (input.left ? 1.0f : 0.0f));
    move.y = axis((input.forward ? 1.0f : 0.0f) - (input.backward ? 1.0f : 0.0f));
    move.z = axis((input.jump ? 1.0f : 0.0f) - (input.jet ? 1.0f : 0.0f));
    move.trigger[0] = input.fire;
    move.trigger[1] = input.altFire;
    move.trigger[2] = input.jump;
    move.trigger[3] = input.jet;
    move.freeLook = input.freeLook;
    return move;
}

} // namespace LiveMovePolicy
