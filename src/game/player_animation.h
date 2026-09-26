#pragma once
// Client-side player animation selection, following Tribes 2's
// PlayerData::preload action table and Player::pickActionAnimation.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace PlayerAnimation {

// Table actions occupy network action indices 0-7, in engine order. The
// server never sends these for movement; clients pick them from velocity.
inline constexpr const char* TableActionNames[] = {
    "root", "run", "back", "side", "fall", "jet", "jump", "land",
};
inline constexpr int NumTableActions = 8;
enum TableAction { Root = 0, Run, Back, Side, Fall, Jet, Jump, Land };

// Ticks without run-surface contact before a player counts as airborne.
inline constexpr int AirborneContactTicks = 30;

inline bool sameName(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    return i == a.size() && !b[i];
}

// PlayerData::preload: the fixed table actions first (the first sequence
// with each name), then every remaining sequence in shape order. Returns
// action index -> index into `sequenceNames`, with -1 for a missing table
// action.
inline std::vector<int> buildActionTable(const std::vector<std::string>& sequenceNames) {
    std::vector<int> table(NumTableActions, -1);
    std::vector<bool> used(sequenceNames.size(), false);
    for (int action = 0; action < NumTableActions; ++action) {
        for (size_t i = 0; i < sequenceNames.size(); ++i) {
            if (sameName(sequenceNames[i], TableActionNames[action])) {
                table[action] = (int)i;
                used[i] = true;
                break;
            }
        }
    }
    for (size_t i = 0; i < sequenceNames.size(); ++i)
        if (!used[i]) table.push_back((int)i);
    return table;
}

struct MoveAnimation {
    int action = Root;
    float timeScale = 1.0f; // -1 plays Side reversed for a right strafe
};

// Player::pickActionAnimation. Velocity is in Torque world space (Z up);
// bodyYaw is the Torque rotation about Z, whose forward is (sin, cos).
inline MoveAnimation pickMoveAnimation(float vx, float vy, float bodyYaw,
                                       int contactTimer, bool falling, bool jetting) {
    if (falling) return {Fall, 1.0f};
    if (contactTimer >= AirborneContactTicks) return {jetting ? Jet : Root, 1.0f};
    const float sinY = std::sin(bodyYaw), cosY = std::cos(bodyYaw);
    // mWorldToObj.mulV(velocity)
    const float localX = vx * cosY - vy * sinY;
    const float localY = vx * sinY + vy * cosY;
    constexpr float MoveThreshold = 0.1f;
    const float forward = localY, back = -localY, left = -localX, right = localX;
    const float best = std::max(std::max(forward, back), std::max(left, right));
    if (best <= MoveThreshold) return {Root, 1.0f};
    if (best == forward) return {Run, 1.0f};
    if (best == back) return {Back, 1.0f};
    if (best == left) return {Side, 1.0f};
    return {Side, -1.0f};
}

// Player::findContact: a surface within 0.03 m of the feet whose normal is
// within runSurfaceAngle (degrees) of vertical counts as run contact.
inline bool hasRunContact(float feetHeight, float floorHeight, float normalUp,
                          float runSurfaceAngleDeg) {
    if (!std::isfinite(floorHeight) || floorHeight < -1e8f) return false;
    if (std::fabs(feetHeight - floorHeight) > 0.03f && feetHeight > floorHeight)
        return false;
    return normalUp > std::cos(runSurfaceAngleDeg * 3.14159265358979323846f / 180.0f);
}

// Normalized position of a wired action at `now`: the packed position plus
// the time elapsed since the update, clamped to the clip.
inline float sampleActionPosition(float packedPosition, bool atEnd, float updateTime,
                                  float now, float duration) {
    if (atEnd) return 1.0f;
    if (!(duration > 0.0f)) return std::clamp(packedPosition, 0.0f, 1.0f);
    const float elapsed = std::max(0.0f, now - updateTime);
    return std::clamp(packedPosition + elapsed / duration, 0.0f, 1.0f);
}

} // namespace PlayerAnimation
