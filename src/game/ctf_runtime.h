#pragma once

#include <algorithm>
#include <cstdint>

namespace CtfRuntime {
enum class FlagState { Home, Held, Dropped };
struct Flag { int team = 0; FlagState state = FlagState::Home; int carrier = -1; };

struct Match {
    int scoreLimit = 5;
    int score[3]{};
    bool started = false;
    bool ended = false;
    int64_t clockMs = 0;
    void reset(int64_t durationMs) {
        score[1] = score[2] = 0; started = false; ended = false;
        clockMs = std::max<int64_t>(0, durationMs);
    }
    void start() { started = true; ended = false; }
    bool tick(int64_t elapsedMs) {
        if (!started || ended) return false;
        clockMs = std::max<int64_t>(0, clockMs - std::max<int64_t>(0, elapsedMs));
        if (clockMs == 0) ended = true;
        return ended;
    }
    bool capture(int team) {
        if (!started || ended || team < 1 || team > 2) return false;
        if (++score[team] >= scoreLimit) ended = true;
        return true;
    }
};

inline bool take(Flag& flag, int playerTeam, int player) {
    if (flag.state == FlagState::Held || flag.team == playerTeam || player < 0) return false;
    flag.state = FlagState::Held; flag.carrier = player; return true;
}
inline bool drop(Flag& flag, int player) {
    if (flag.state != FlagState::Held || flag.carrier != player) return false;
    flag.state = FlagState::Dropped; flag.carrier = -1; return true;
}
inline bool returnHome(Flag& flag, int playerTeam) {
    if (flag.state != FlagState::Dropped || flag.team != playerTeam) return false;
    flag.state = FlagState::Home; return true;
}
inline bool canCapture(const Flag& carrierFlag, const Flag& homeFlag,
                       int carrierTeam, int carrier) {
    return carrierFlag.state == FlagState::Held && carrierFlag.carrier == carrier &&
           carrierTeam >= 1 && carrierTeam <= 2 && homeFlag.team == carrierTeam &&
           homeFlag.state == FlagState::Home;
}
}
